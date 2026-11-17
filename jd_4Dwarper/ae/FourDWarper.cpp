// 4Dwarper: After Effects effect. Apply to the TARGET layer, pick the SOURCE layer, press Analyze.
// Output = the source conformed to the target in time (two-frame blend for now) and space (homography).
// Analysis results live in an ARBITRARY param (w4d blob, see src/warp_record.h) so they save with the project.
// Render (SmartFX, 8/16/32 bpc) only READS that blob: safe for multi-frame rendering. All solving happens in Analyze:
// frames are fetched on the main thread (AE API), the fits run on worker threads that never touch the AE API.
#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_Macros.h"
#include "AEGP_SuiteHandler.h"
#include "Param_Utils.h"
#include "Smart_Utils.h"
#include "String_Utils.h"
#include "AEFX_SuiteHelper.h"
#include "adobesdk/DrawbotSuite.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../src/align_shot.h"
#include "../src/warp_record.h"
#include "../src/w4d_analysis.h"
#include "FourDWarperVersion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

static_assert(PF_VERSION(W4D_MAJOR_VERSION, W4D_MINOR_VERSION, W4D_BUG_VERSION, W4D_STAGE_VERSION, W4D_BUILD_VERSION) == W4D_PIPL_VERSION, "PiPL version");
static_assert((PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_CUSTOM_UI | PF_OutFlag_DEEP_COLOR_AWARE) == 0x02008002, "PiPL out_flags");
static_assert((PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_THREADED_RENDERING) == 0x08001400, "PiPL out_flags2");

// Param DISK IDs: saved in projects, NEVER renumber or reuse (AE matches a project's values to params by ID).
// 0.1-0.3.2 used ID = position; 0.3.3 reorders the panel, so position (P_*) and ID (D_*) are separate now.
enum { D_SOURCE = 1, D_ANALYZE, D_CLEAR, D_MODE, D_RANGE, D_MASK, D_LOCAL, D_SMOOTH, D_VIEW, D_EDGE, D_DATA, D_EDGES, D_BANNER,
       D_REGION_TL, D_REGION_BR, D_AUTO,
       D_SRC_MASK, D_MASK_CHANNEL, D_MASK_GROW, D_BLEND, D_AWIDTH,                       // 0.3.0
       D_ADV_START, D_HOLD_ON, D_HOLD_FRAME, D_STIFF_P, D_STIFF_L, D_AFF_ON, D_AFF_FROM, D_AFF_TO, D_ADV_END,
       D_STIFF_MASKS /* 0.3.1 */, D_SPACE_AMT, D_TIME_AMT /* 0.3.2 */,
       D_SETUP_START, D_SETUP_END, D_HELP, D_WARP_START, D_WARP_END /* 0.3.3 */, D_DOCS /* 0.3.4 */ };
static_assert(D_TIME_AMT == 33 && D_SETUP_START == 34, "param IDs of 0.3.2 projects must not move");
// Param POSITIONS (the order in the Effect Controls panel), 0.3.3: Setup, Analyze, Warp, View, Advanced (collapsed).
enum { P_INPUT = 0,
       P_SETUP_START, P_SOURCE, P_MASK, P_SRC_MASK, P_MASK_CHANNEL, P_MASK_GROW, P_SETUP_END,
       P_ANALYZE, P_CLEAR, P_BANNER, P_HELP, P_DOCS,
       P_WARP_START, P_SPACE_AMT, P_TIME_AMT, P_EDGES, P_BLEND, P_WARP_END,
       P_VIEW,
       P_ADV_START, P_MODE, P_RANGE, P_AWIDTH, P_LOCAL, P_SMOOTH, P_REGION_TL, P_REGION_BR, P_AUTO,
       P_HOLD_ON, P_HOLD_FRAME, P_STIFF_P, P_STIFF_L, P_STIFF_MASKS, P_AFF_ON, P_AFF_FROM, P_AFF_TO, P_ADV_END,
       P_EDGE, P_DATA, P_NUM };
enum { MASKCH_LUMA = 1, MASKCH_ALPHA };
enum { EDGES_EXTEND = 1, EDGES_TRANSPARENT, EDGES_TARGET };
enum { MODE_BOTH = 1, MODE_SPACE, MODE_TIME };
enum { VIEW_RESULT = 1, VIEW_DIFF, VIEW_MIX, VIEW_DIFF_PLAIN, VIEW_IGNORED };   // VIEW_DIFF = exposure-matched
enum { CK_IN = 1, CK_A, CK_B, CK_M };
#define ARB_REFCON ((void*)0x57344431)
static const double kAnalysisMaxBytes = 3.0e9;     // all reduced frames of both layers are held in memory
static const int kMaxFrames = 5000;
static AEGP_PluginID g_aegp_id = 0;                 // for the AEGP stream calls (source layer in/out)

static std::string LogPath() {
    const char* t = std::getenv("TEMP");
    return std::string(t ? t : ".") + "\\jd_4Dwarper_analyze.log";
}

// ---------------------------------------------------------------- arbitrary data (flat blob, variable size)
static PF_Err NewBlob(PF_InData* in_data, size_t bytes, PF_Handle* out) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_Handle h = suites.HandleSuite1()->host_new_handle((A_HandleSize)bytes);
    if (!h) return PF_Err_OUT_OF_MEMORY;
    void* p = suites.HandleSuite1()->host_lock_handle(h);
    std::memset(p, 0, bytes);
    w4d::init_header((w4d::BlobHeader*)p, 0, 0, 0, 0, 0);
    suites.HandleSuite1()->host_unlock_handle(h);
    *out = h;
    return PF_Err_NONE;
}
static PF_Err CopyBlob(PF_InData* in_data, PF_Handle src, PF_Handle* out) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    if (!src) return NewBlob(in_data, sizeof(w4d::BlobHeader), out);
    size_t n = (size_t)suites.HandleSuite1()->host_get_handle_size(src);
    PF_Err err = NewBlob(in_data, std::max(n, sizeof(w4d::BlobHeader)), out);
    if (!err) {
        std::memcpy(suites.HandleSuite1()->host_lock_handle(*out), suites.HandleSuite1()->host_lock_handle(src), n);
        suites.HandleSuite1()->host_unlock_handle(*out); suites.HandleSuite1()->host_unlock_handle(src);
    }
    return err;
}

static PF_Err HandleArbitrary(PF_InData* in_data, PF_OutData*, PF_ArbParamsExtra* extra) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    auto hs = suites.HandleSuite1();
    PF_Err err = PF_Err_NONE;
    switch (extra->which_function) {
    case PF_Arbitrary_NEW_FUNC:
        err = NewBlob(in_data, sizeof(w4d::BlobHeader), extra->u.new_func_params.arbPH);
        break;
    case PF_Arbitrary_DISPOSE_FUNC:
        if (extra->u.dispose_func_params.arbH) hs->host_dispose_handle(extra->u.dispose_func_params.arbH);
        break;
    case PF_Arbitrary_COPY_FUNC:
        err = CopyBlob(in_data, extra->u.copy_func_params.src_arbH, extra->u.copy_func_params.dst_arbPH);
        break;
    case PF_Arbitrary_FLAT_SIZE_FUNC:
        *extra->u.flat_size_func_params.flat_data_sizePLu = (A_u_long)hs->host_get_handle_size(extra->u.flat_size_func_params.arbH);
        break;
    case PF_Arbitrary_FLATTEN_FUNC: {
        PF_Handle h = extra->u.flatten_func_params.arbH;
        size_t n = (size_t)hs->host_get_handle_size(h);
        if (extra->u.flatten_func_params.buf_sizeLu >= n) {
            std::memcpy(extra->u.flatten_func_params.flat_dataPV, hs->host_lock_handle(h), n); hs->host_unlock_handle(h);
        }
        break;
    }
    case PF_Arbitrary_UNFLATTEN_FUNC: {
        size_t n = extra->u.unflatten_func_params.buf_sizeLu;
        // keep an older version's data too, so the banner can ask for a new Analyze (it is never read as v2)
        if (!w4d::valid(extra->u.unflatten_func_params.flat_dataPV, n) && !w4d::old_version(extra->u.unflatten_func_params.flat_dataPV, n)) {
            err = NewBlob(in_data, sizeof(w4d::BlobHeader), extra->u.unflatten_func_params.arbPH);
        } else {
            err = NewBlob(in_data, n, extra->u.unflatten_func_params.arbPH);
            if (!err) {
                PF_Handle h = *extra->u.unflatten_func_params.arbPH;
                std::memcpy(hs->host_lock_handle(h), extra->u.unflatten_func_params.flat_dataPV, n); hs->host_unlock_handle(h);
            }
        }
        break;
    }
    case PF_Arbitrary_INTERP_FUNC:   // not animatable: hold the left value
        err = CopyBlob(in_data, extra->u.interp_func_params.left_arbH, extra->u.interp_func_params.interpPH);
        break;
    case PF_Arbitrary_COMPARE_FUNC: {
        PF_Handle a = extra->u.compare_func_params.a_arbH, b = extra->u.compare_func_params.b_arbH;
        size_t na = a ? (size_t)hs->host_get_handle_size(a) : 0, nb = b ? (size_t)hs->host_get_handle_size(b) : 0;
        bool same = na == nb;
        if (same && na) { same = std::memcmp(hs->host_lock_handle(a), hs->host_lock_handle(b), na) == 0; hs->host_unlock_handle(a); hs->host_unlock_handle(b); }
        *extra->u.compare_func_params.compareP = same ? PF_ArbCompare_EQUAL : PF_ArbCompare_NOT_EQUAL;
        break;
    }
    case PF_Arbitrary_PRINT_SIZE_FUNC:
        *extra->u.print_size_func_params.print_sizePLu = 16;
        break;
    case PF_Arbitrary_PRINT_FUNC:
        if (extra->u.print_func_params.print_sizeLu >= 12) std::strcpy(extra->u.print_func_params.print_bufferPC, "jd_4Dwarper");
        break;
    case PF_Arbitrary_SCAN_FUNC:
        err = NewBlob(in_data, sizeof(w4d::BlobHeader), extra->u.scan_func_params.arbPH);
        break;
    default: break;
    }
    return err;
}

// ---------------------------------------------------------------- setup
static PF_Err About(PF_InData*, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    std::snprintf(out_data->return_msg, sizeof(out_data->return_msg),
                  "jd_4Dwarper %d.%d.%d\rConforms a source pass to this layer in time and space.\r1. Setup: pick the Source Layer (and the Ignore Layers that roto the talent).\r2. Press Analyze.\r3. Warp: Space / Time Warp Amount, Edges. View: Difference to check.\rDocs: press Open Docs.\rJean Delaunay",
                  W4D_MAJOR_VERSION, W4D_MINOR_VERSION, W4D_BUG_VERSION);
    return PF_Err_NONE;
}
static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    if (!g_aegp_id && in_data->appl_id != 'PrMr') {
        try { AEGP_SuiteHandler s(in_data->pica_basicP); s.UtilitySuite3()->AEGP_RegisterWithAEGP(NULL, W4D_NAME, &g_aegp_id); } catch (...) { g_aegp_id = 0; }
    }
    out_data->my_version = PF_VERSION(W4D_MAJOR_VERSION, W4D_MINOR_VERSION, W4D_BUG_VERSION, W4D_STAGE_VERSION, W4D_BUILD_VERSION);
    out_data->out_flags = PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_CUSTOM_UI | PF_OutFlag_DEEP_COLOR_AWARE;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    // 0.3.3: grouped by workflow. Every param keeps its 0.3.2 ID (D_*), so old projects load their values;
    // only the panel order and the display names changed. New params (topics, Help) got new IDs.
    def.flags = 0;
    AEFX_CLR_STRUCT(def); PF_ADD_TOPIC("Setup", D_SETUP_START);
    AEFX_CLR_STRUCT(def); PF_ADD_LAYER("Source Layer", PF_LayerDefault_NONE, D_SOURCE);
    AEFX_CLR_STRUCT(def); PF_ADD_LAYER("Target Ignore Layer", PF_LayerDefault_NONE, D_MASK);
    AEFX_CLR_STRUCT(def); PF_ADD_LAYER("Source Ignore Layer", PF_LayerDefault_NONE, D_SRC_MASK);
    AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Ignore Layers Use", 2, MASKCH_LUMA, "Luma (white = ignore)|Alpha (opaque = ignore)", D_MASK_CHANNEL);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Ignore Mask Grow (px)", 0, 400, 0, 100, 24, D_MASK_GROW);
    AEFX_CLR_STRUCT(def); PF_END_TOPIC(D_SETUP_END);
    AEFX_CLR_STRUCT(def); PF_ADD_BUTTON("Analysis", "Analyze", 0, PF_ParamFlag_SUPERVISE, D_ANALYZE);
    AEFX_CLR_STRUCT(def); PF_ADD_BUTTON("Clear Analysis", "Clear", 0, PF_ParamFlag_SUPERVISE, D_CLEAR);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Show Status Banner", "", TRUE, PF_ParamFlag_CANNOT_TIME_VARY, D_BANNER);
    AEFX_CLR_STRUCT(def); PF_ADD_BUTTON("Help", "About", 0, PF_ParamFlag_SUPERVISE, D_HELP);
    AEFX_CLR_STRUCT(def); PF_ADD_BUTTON("Docs", "Open Docs", 0, PF_ParamFlag_SUPERVISE, D_DOCS);   // 0.3.4, new ID
    AEFX_CLR_STRUCT(def); PF_ADD_TOPIC("Warp", D_WARP_START);
    // How far the source is conformed, keyframable. Keyframe them together: Time below 100 % with Space at 100 %
    // applies a warp solved for another source frame. Both at 0 % = the source layer untouched.
    AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDERX("Space Warp Amount", 0, 100, 0, 100, 100, 1, PF_ValueDisplayFlag_PERCENT, 0, D_SPACE_AMT);
    AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDERX("Time Warp Amount", 0, 100, 0, 100, 100, 1, PF_ValueDisplayFlag_PERCENT, 0, D_TIME_AMT);
    // Transparent by default (0.3.1): where the source take has no pixels nothing is invented. Extend smears the border.
    AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Edges", 3, EDGES_TRANSPARENT, "Extend Source Edge|Transparent|Show Target", D_EDGES);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Blend Neighbour Frames", "", FALSE, 0, D_BLEND);
    AEFX_CLR_STRUCT(def); PF_END_TOPIC(D_WARP_END);
    AEFX_CLR_STRUCT(def); PF_ADD_POPUP("View", 5, VIEW_RESULT, "Result|Difference (exposure matched)|50% Mix|Difference (plain)|Ignored Zones", D_VIEW);
    // Advanced: analysis settings; the defaults fit most shots. Starts collapsed.
    AEFX_CLR_STRUCT(def); def.flags = PF_ParamFlag_START_COLLAPSED; PF_ADD_TOPIC("Advanced", D_ADV_START);
    AEFX_CLR_STRUCT(def); PF_ADD_POPUP("Solve", 3, MODE_BOTH, "Time + Space|Space only|Time only", D_MODE);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Time Search Range (frames)", 0, 30, 0, 16, 8, D_RANGE);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Analysis Width (px)", 160, 4096, 320, 1920, 640, D_AWIDTH);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Local Warp", "", TRUE, 0, D_LOCAL);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Local Warp Smoothness (%)", 0, 100, 0, 100, 25, D_SMOOTH);
    // Analysis region: only the rectangle between these two points is measured (default = whole frame)
    AEFX_CLR_STRUCT(def); PF_ADD_POINT("Analysis Region Top Left", 0, 0, FALSE, D_REGION_TL);
    AEFX_CLR_STRUCT(def); PF_ADD_POINT("Analysis Region Bottom Right", 100, 100, FALSE, D_REGION_BR);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Auto Ignore Changes", "", FALSE, PF_ParamFlag_CANNOT_TIME_VARY, D_AUTO);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Hold Time After Frame", "", FALSE, 0, D_HOLD_ON);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Hold From Layer Frame", 0, 100000, 0, 500, 0, D_HOLD_FRAME);
    AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDERX("Perspective Stiffness", 0, 20, 0, 4, 1, 2, PF_ValueDisplayFlag_NONE, 0, D_STIFF_P);
    AEFX_CLR_STRUCT(def); PF_ADD_FLOAT_SLIDERX("Scale / Rotation Stiffness", 0, 20, 0, 4, 0.5, 2, PF_ValueDisplayFlag_NONE, 0, D_STIFF_L);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Use Stiffness With Ignore Layers", "", FALSE, 0, D_STIFF_MASKS);
    AEFX_CLR_STRUCT(def); PF_ADD_CHECKBOX("Affine Only In Range", "", FALSE, 0, D_AFF_ON);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Affine From Layer Frame", 0, 100000, 0, 500, 0, D_AFF_FROM);
    AEFX_CLR_STRUCT(def); PF_ADD_SLIDER("Affine To Layer Frame", 0, 100000, 0, 500, 0, D_AFF_TO);
    AEFX_CLR_STRUCT(def); PF_END_TOPIC(D_ADV_END);
    // hidden: unused since 0.2 (kept so old projects load) and the analysis data
    AEFX_CLR_STRUCT(def); def.ui_flags = PF_PUI_INVISIBLE; PF_ADD_CHECKBOX("Edge Matching", "", TRUE, 0, D_EDGE);
    PF_Handle dflt = nullptr;
    ERR(NewBlob(in_data, sizeof(w4d::BlobHeader), &dflt));
    AEFX_CLR_STRUCT(def);
    PF_ADD_ARBITRARY2("Analysis Data", 0, 0, PF_ParamFlag_CANNOT_TIME_VARY, PF_PUI_NO_ECW_UI, dflt, D_DATA, ARB_REFCON);
    if (!err) {   // status banner drawn over the comp / layer view (never part of a render)
        PF_CustomUIInfo ci; AEFX_CLR_STRUCT(ci);
        ci.events = PF_CustomEFlag_LAYER | PF_CustomEFlag_COMP;
        ci.comp_ui_alignment = PF_UIAlignment_NONE;
        ci.layer_ui_alignment = PF_UIAlignment_NONE;
        err = (*(in_data->inter.register_ui))(in_data->effect_ref, &ci);
    }
    out_data->num_params = P_NUM;
    return err;
}

// ---------------------------------------------------------------- pixel formats
static PF_PixelFormat FormatOf(PF_InData* in_data, PF_OutData* out_data, PF_EffectWorld* w) {
    PF_PixelFormat fmt = PF_WORLD_IS_DEEP(w) ? PF_PixelFormat_ARGB64 : PF_PixelFormat_ARGB32;
    try {
        AEFX_SuiteScoper<PF_WorldSuite2> ws(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
        PF_PixelFormat f = PF_PixelFormat_INVALID;
        if (!ws->PF_GetPixelFormat(w, &f) && f != PF_PixelFormat_INVALID) fmt = f;
    } catch (...) {}
    return fmt;
}

// Luma of an 8/16/32-bit ARGB world, box-reduced by an integer factor k. Float luma is clamped to 0..4 for matching.
template <typename PIX>
static moco::Image grey_of(const PF_EffectWorld* w, int k, float white) {
    moco::Image o(std::max(1, int(w->width) / k), std::max(1, int(w->height) / k));
    const float inv = 1.f / (float(k) * k * white);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            float s = 0;
            for (int j = 0; j < k; ++j) {
                const PIX* p = (const PIX*)((const char*)w->data + size_t(y * k + j) * w->rowbytes) + x * k;
                for (int i = 0; i < k; ++i, ++p) s += 0.2126f * float(p->red) + 0.7152f * float(p->green) + 0.0722f * float(p->blue);
            }
            s *= inv;
            o.at(x, y) = s == s ? std::min(std::max(s, 0.f), 4.f) : 0.f;
        }
    return o;
}
// Alpha of a world, box-reduced by k, 0..1.
template <typename PIX>
static moco::Image alpha_of(const PF_EffectWorld* w, int k, float white) {
    moco::Image o(std::max(1, int(w->width) / k), std::max(1, int(w->height) / k));
    const float inv = 1.f / (float(k) * k * white);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            float s = 0;
            for (int j = 0; j < k; ++j) {
                const PIX* p = (const PIX*)((const char*)w->data + size_t(y * k + j) * w->rowbytes) + x * k;
                for (int i = 0; i < k; ++i, ++p) s += float(p->alpha);
            }
            o.at(x, y) = s * inv;
        }
    return o;
}
static moco::Image world_to_alpha(PF_InData* in_data, PF_OutData* out_data, PF_EffectWorld* w, int k) {
    switch (FormatOf(in_data, out_data, w)) {
    case PF_PixelFormat_ARGB128: return alpha_of<PF_PixelFloat>(w, k, 1.f);
    case PF_PixelFormat_ARGB64: return alpha_of<PF_Pixel16>(w, k, 32768.f);
    default: return alpha_of<PF_Pixel8>(w, k, 255.f);
    }
}
static moco::Image world_to_grey(PF_InData* in_data, PF_OutData* out_data, PF_EffectWorld* w, int k) {
    switch (FormatOf(in_data, out_data, w)) {
    case PF_PixelFormat_ARGB128: return grey_of<PF_PixelFloat>(w, k, 1.f);
    case PF_PixelFormat_ARGB64: return grey_of<PF_Pixel16>(w, k, 32768.f);
    default: return grey_of<PF_Pixel8>(w, k, 255.f);
    }
}

// ---------------------------------------------------------------- Analyze
static void Message(PF_OutData* out_data, const char* msg) {
    std::snprintf(out_data->return_msg, sizeof(out_data->return_msg), "%s", msg);
    out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
}

// Stores the banner state in the blob (keeping any analysis records already there) and refreshes the UI.
static PF_Err StoreStatus(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], uint32_t status, const char* reason) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_Handle h = nullptr;
    PF_Err err = CopyBlob(in_data, params[P_DATA]->u.arb_d.value, &h);
    if (err) return err;
    size_t bytes = (size_t)suites.HandleSuite1()->host_get_handle_size(h);
    void* p = suites.HandleSuite1()->host_lock_handle(h);
    if (!w4d::valid(p, bytes)) w4d::init_header((w4d::BlobHeader*)p, 0, 0, 0, 0, 0);
    w4d::set_status((w4d::BlobHeader*)p, status, reason);
    suites.HandleSuite1()->host_unlock_handle(h);
    params[P_DATA]->u.arb_d.value = h;
    params[P_DATA]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
    return PF_Err_NONE;
}

// Modal progress with a Cancel button (PF App Suite), the documented way to keep AE alive in a long command.
// Blue/orange banner states are shown here as the dialog title, because the comp banner cannot redraw while
// Analyze blocks the main thread.
struct Progress {
    PF_InData* in_data; const PFAppSuite6* app = nullptr; PF_AppProgressDialogP dlg = nullptr;
    void open(const wchar_t* title) {
        close();
        if (app) app->PF_CreateNewAppProgressDialog((const A_UTF16Char*)title, nullptr, FALSE, &dlg);
    }
    bool update(long count, long total) {   // false = the user cancelled
        bool go = true;
        if (dlg && app->PF_AppProgressDialogUpdate(dlg, (A_long)count, (A_long)total) != PF_Err_NONE) go = false;
        if (PF_PROGRESS(in_data, (A_long)count, (A_long)total) != PF_Err_NONE) go = false;
        // Esc, read straight from the keyboard: AE cannot deliver key events while Analyze holds its main thread.
        // Only when After Effects is the foreground application.
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            DWORD pid = 0; HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId()) go = false;
        }
        return go;
    }
    void close() { if (dlg && app) app->PF_DisposeAppProgressDialog(dlg); dlg = nullptr; }
    ~Progress() { close(); }
};

// The effect's analysis settings (same struct and defaults as warper_cli / w4d_verify). Frame numbers the user types
// are LAYER frames; the solve counts from the first analysed frame f0.
static w4d::Settings SettingsFromParams(PF_ParamDef* params[], int f0) {
    w4d::Settings s;
    s.mode = params[P_MODE]->u.pd.value;
    s.range = std::max(0, int(params[P_RANGE]->u.sd.value));
    s.analysis_width = std::max(64, int(params[P_AWIDTH]->u.sd.value));
    s.auto_ignore = params[P_AUTO]->u.bd.value != 0;
    if (!params[P_LOCAL]->u.bd.value) { s.grid_w = 0; s.grid_h = 0; }
    s.grid_smooth = std::max(0.01, params[P_SMOOTH]->u.sd.value / 100.0);
    s.time_trust_end = params[P_HOLD_ON]->u.bd.value ? int(params[P_HOLD_FRAME]->u.sd.value) - f0 : -1;
    s.prior_persp = params[P_STIFF_P]->u.fs_d.value;
    s.prior_lin = params[P_STIFF_L]->u.fs_d.value;
    s.stiffness_with_masks = params[P_STIFF_MASKS]->u.bd.value != 0;
    if (params[P_AFF_ON]->u.bd.value) {
        s.affine_lo = int(params[P_AFF_FROM]->u.sd.value) - f0; s.affine_hi = int(params[P_AFF_TO]->u.sd.value) - f0;
        if (s.affine_hi < s.affine_lo) std::swap(s.affine_lo, s.affine_hi);
    }
    s.mask_grow_px = std::max(0, int(params[P_MASK_GROW]->u.sd.value));
    unsigned hc = std::thread::hardware_concurrency();
    s.threads = int(std::max(1u, hc > 2 ? hc - 2 : 1u));
    return s;
}

// The source layer's in/out, as indices on the analysed frame grid (frame i = layer frame f0 + i). AE hands out
// source frames outside the source layer's in/out (e.g. CG frames after the take's out point), so the solve must not
// use them. Returns false when it cannot be read (then nothing is clamped).
static bool SourceRange(PF_InData* in_data, int f0, int n, int* lo, int* hi) {
    if (!g_aegp_id) return false;
    bool ok = false;
    AEGP_EffectRefH meH = nullptr; AEGP_StreamRefH st = nullptr;
    try {
        AEGP_SuiteHandler s(in_data->pica_basicP);
        AEGP_LayerH me = nullptr; AEGP_CompH comp = nullptr;
        if (!s.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &me) && me &&
            !s.LayerSuite8()->AEGP_GetLayerParentComp(me, &comp) && comp &&
            !s.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(g_aegp_id, in_data->effect_ref, &meH) && meH &&
            !s.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(g_aegp_id, meH, P_SOURCE, &st) && st) {
            A_Time t0; t0.value = A_long(f0) * in_data->time_step; t0.scale = A_u_long(in_data->time_scale);
            AEGP_StreamValue2 v; AEFX_CLR_STRUCT(v);
            if (!s.StreamSuite5()->AEGP_GetNewStreamValue(g_aegp_id, st, AEGP_LTimeMode_LayerTime, &t0, FALSE, &v)) {
                const AEGP_LayerIDVal id = v.val.layer_id;
                s.StreamSuite5()->AEGP_DisposeStreamValue(&v);
                AEGP_LayerH src = nullptr; A_Time sin = {0, 1}, sdur = {0, 1};
                if (id && !s.LayerSuite8()->AEGP_GetLayerFromLayerID(comp, id, &src) && src &&
                    !s.LayerSuite8()->AEGP_GetLayerInPoint(src, AEGP_LTimeMode_CompTime, &sin) &&
                    !s.LayerSuite8()->AEGP_GetLayerDuration(src, AEGP_LTimeMode_CompTime, &sdur) && sin.scale && sdur.scale) {
                    const double cin = double(sin.value) / sin.scale, cout = cin + double(sdur.value) / sdur.scale, eps = 1e-6;
                    int a = -1, b = -1; bool bad = false;
                    for (int i = 0; i < n; ++i) {
                        A_Time lt, ct = {0, 1};
                        lt.value = A_long(f0 + i) * in_data->time_step; lt.scale = A_u_long(in_data->time_scale);
                        if (s.LayerSuite8()->AEGP_ConvertLayerToCompTime(me, &lt, &ct) || !ct.scale) { bad = true; break; }
                        const double c = double(ct.value) / ct.scale;
                        if (c >= cin - eps && c < cout - eps) { if (a < 0) a = i; b = i; }
                    }
                    if (!bad) { *lo = a; *hi = b; ok = true; }   // a = -1: no overlap at all
                }
            }
        }
    } catch (...) { ok = false; }
    try {
        AEGP_SuiteHandler s(in_data->pica_basicP);
        if (st) s.StreamSuite5()->AEGP_DisposeStream(st);
        if (meH) s.EffectSuite4()->AEGP_DisposeEffect(meH);
    } catch (...) {}
    return ok;
}

// A key of every analysis setting (not the layers). Stored in BlobHeader::pad0 by Analyze (0 = unknown, older
// analysis); the banner compares it with the current settings to say "needs re-analysis".
static uint32_t SettingsKey(PF_ParamDef* params[]) {
    uint32_t h = 2166136261u;
    auto mix = [&](double v) { const long long q = (long long)std::llround(v * 1000.0); for (int i = 0; i < 8; ++i) { h ^= uint32_t((q >> (8 * i)) & 0xFF); h *= 16777619u; } };
    mix(params[P_MODE]->u.pd.value); mix(params[P_RANGE]->u.sd.value); mix(params[P_AWIDTH]->u.sd.value);
    mix(params[P_AUTO]->u.bd.value); mix(params[P_LOCAL]->u.bd.value); mix(params[P_SMOOTH]->u.sd.value);
    mix(params[P_HOLD_ON]->u.bd.value); mix(params[P_HOLD_ON]->u.bd.value ? params[P_HOLD_FRAME]->u.sd.value : 0);
    mix(params[P_STIFF_P]->u.fs_d.value); mix(params[P_STIFF_L]->u.fs_d.value); mix(params[P_STIFF_MASKS]->u.bd.value);
    mix(params[P_AFF_ON]->u.bd.value);
    if (params[P_AFF_ON]->u.bd.value) { mix(params[P_AFF_FROM]->u.sd.value); mix(params[P_AFF_TO]->u.sd.value); }
    mix(params[P_MASK_GROW]->u.sd.value); mix(params[P_MASK_CHANNEL]->u.pd.value);
    mix(FIX_2_FLOAT(params[P_REGION_TL]->u.td.x_value)); mix(FIX_2_FLOAT(params[P_REGION_TL]->u.td.y_value));
    mix(FIX_2_FLOAT(params[P_REGION_BR]->u.td.x_value)); mix(FIX_2_FLOAT(params[P_REGION_BR]->u.td.y_value));
    return h | 1u;
}

// What to do after each failure reason (reasons are written by Analyze; old projects may hold others).
static const char* AdviceFor(const char* reason) {
    struct { const char* r; const char* a; } t[] = {
        {"no source layer", "pick a Source Layer, then Analyze"},
        {"size mismatch", "precompose the source to the target size"},
        {"source outside the shot", "the Source Layer must overlap this layer in time"},
        {"layer too short", "the layer needs 4 frames or more"},
        {"analysis region too small", "enlarge the Analysis Region (Advanced)"},
        {"could not read frames", "check the layers render, then Analyze"},
        {"out of memory", "lower Analysis Width (Advanced) or trim the layer"},
        {"no frame duration", "use footage with a frame rate"},
    };
    for (auto& e : t) if (std::strcmp(reason, e.r) == 0) return e.a;
    return "press Analyze again";
}

// ---------------------------------------------------------------- debug dump (0.3.3, for testers)
// Only when the folder %TEMP%\4Dwarper_dump exists (create it to switch the dump on, delete it to switch off):
// Analyze writes there what After Effects actually fed the solve, so it can be diffed against the PNG plates the
// CLI reads. Images at analysis size as PFM (grey float, exact values): T_<f>.pfm target, S_<f>.pfm source,
// MK_<f>.pfm / MKS_<f>.pfm the ignore masks after binarise + grow (what the solve sees). f = layer frame.
// fetch.txt: sizes, pixel format, frame ranges. solve.csv: per target frame the solved source time and held flag.
static std::string DumpDir() {
    const char* t = std::getenv("TEMP");
    const std::string d = std::string(t ? t : ".") + "\\jd_4Dwarper_dump";
    const DWORD a = GetFileAttributesA(d.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? d : std::string();
}
static void WritePFM(const std::string& path, const moco::Image& im) {
    if (im.w <= 0 || im.h <= 0) return;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "Pf\n%d %d\n-1.0\n", im.w, im.h);
    for (int y = im.h - 1; y >= 0; --y) std::fwrite(&im.p[size_t(y) * im.w], sizeof(float), size_t(im.w), f);   // PFM: bottom row first
    std::fclose(f);
}
static bool DumpFrame(int i, int n) {   // a few frames over the shot, and the whole end of it
    return i == 0 || i % 60 == 0 || i >= n - 13;
}

static PF_Err Analyze(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    PF_Err err = PF_Err_NONE;
    typedef std::chrono::steady_clock Clock;
    auto fail = [&](const char* reason, const char* msg) {
        if (msg) Message(out_data, msg);
        if (!std::strcmp(reason, "cancelled")) return PF_Err(PF_Err_NONE);   // previous analysis and status kept
        return StoreStatus(in_data, out_data, params, w4d::kFailed, reason);
    };
    if (in_data->time_step <= 0) return fail("no frame duration", "jd_4Dwarper: this layer has no frame duration.");
    // Range = the layer's in/out points in the comp (trim the layer to choose it); whole source if that fails.
    int f0 = 0, n = int(in_data->total_time / in_data->time_step);
    const double fdur = double(in_data->time_step) / in_data->time_scale;
    bool trimmed = false;
    try {
        AEGP_SuiteHandler s(in_data->pica_basicP);
        AEGP_LayerH layerH = nullptr; A_Time tin = {0, 1}, tdur = {0, 1};
        if (!s.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH) && layerH &&
            !s.LayerSuite8()->AEGP_GetLayerInPoint(layerH, AEGP_LTimeMode_LayerTime, &tin) &&
            !s.LayerSuite8()->AEGP_GetLayerDuration(layerH, AEGP_LTimeMode_LayerTime, &tdur) && tin.scale && tdur.scale) {
            int a = (int)std::lround(double(tin.value) / tin.scale / fdur), c = (int)std::lround(double(tdur.value) / tdur.scale / fdur);
            if (c >= 4) { f0 = a; n = c; trimmed = true; }
        }
    } catch (...) {}
    if (n < 4) return fail("layer too short", "jd_4Dwarper: the layer is too short to analyse.");
    n = std::min(n, kMaxFrames);
    const w4d::Settings st = SettingsFromParams(params, f0);
    // source frames: only those inside the source layer's in/out
    int s_lo = 0, s_hi = n - 1;
    const bool src_known = SourceRange(in_data, f0, n, &s_lo, &s_hi);
    if (src_known && (s_lo < 0 || s_hi - s_lo < 3))
        return fail("source outside the shot", "jd_4Dwarper: the Source Layer's in/out does not overlap this layer's range.");
    const int nS = s_hi + 1;
    const bool luma = params[P_MASK_CHANNEL]->u.pd.value != MASKCH_ALPHA;
    Progress prog{in_data};
    try {
        AEFX_SuiteScoper<PFAppSuite6> app(in_data, kPFAppSuite, kPFAppSuiteVersion6, out_data);
        prog.app = app.get();
    } catch (...) { prog.app = nullptr; }
    if (out_data->out_flags & PF_OutFlag_DISPLAY_ERROR_MESSAGE) { out_data->out_flags &= ~PF_OutFlag_DISPLAY_ERROR_MESSAGE; out_data->return_msg[0] = 0; }
    prog.open(L"jd_4Dwarper: Analyzing");

    // 1) main thread: fetch every frame of both layers and both ignore layers as reduced images (AE calls only here)
    std::vector<moco::Image> T(n), S(nS), MKr(n), MKSr(nS);   // MKr / MKSr: reduced mask channel before binarising
    bool use_t = true, use_s = true, tmask_size_bad = false, smask_size_bad = false;
    int k = 0, aw = 0, ah = 0, full_w = 0, full_h = 0;
    auto t0 = Clock::now();
    // which: 0 target, 1 source, 2 target ignore, 3 source ignore
    for (int i = 0; i < n && !err; ++i) {
        if (!prog.update(i, n)) return fail("cancelled", nullptr);             // previous analysis is kept
        for (int which = 0; which < 4 && !err; ++which) {
            if ((which == 1 || which == 3) && (i >= nS || i < s_lo)) continue;   // outside the source layer
            if ((which == 2 && !use_t) || (which == 3 && !use_s) || (which >= 2 && !k)) continue;
            const int pidx = which == 0 ? P_INPUT : which == 1 ? P_SOURCE : which == 2 ? P_MASK : P_SRC_MASK;
            PF_ParamDef def; AEFX_CLR_STRUCT(def);
            err = PF_CHECKOUT_PARAM(in_data, pidx, (f0 + i) * in_data->time_step, in_data->time_step, in_data->time_scale, &def);
            bool stop = false; const char* why = nullptr;
            if (!err) {
                PF_EffectWorld* w = &def.u.ld;
                if (!w->data) {
                    if (which == 1 && i == s_lo) { stop = true; why = "jd_4Dwarper: pick a Source Layer first."; }
                    if (which == 2 && i == 0) use_t = false;      // set to None
                    if (which == 3 && i == s_lo) use_s = false;
                } else if (which == 0 && !k) {   // first target frame decides the analysis size
                    full_w = w->width; full_h = w->height;
                    k = w4d::analysis_factor(full_w, st.analysis_width);
                    while (4.0 * n * double(full_w / k) * double(full_h / k) * 4.0 > kAnalysisMaxBytes) ++k;
                    aw = std::max(1, full_w / k); ah = std::max(1, full_h / k);
                    T[i] = world_to_grey(in_data, out_data, w, k);
                } else if (w->width != full_w || w->height != full_h) {
                    if (which == 1) { stop = true; why = "jd_4Dwarper: source and target layers have different sizes. Not supported yet: precompose the source to the target size."; }
                    if (which == 2) { use_t = false; tmask_size_bad = true; }
                    if (which == 3) { use_s = false; smask_size_bad = true; }
                } else {
                    moco::Image im = (which <= 1 || luma) ? world_to_grey(in_data, out_data, w, k) : world_to_alpha(in_data, out_data, w, k);
                    (which == 0 ? T : which == 1 ? S : which == 2 ? MKr : MKSr)[i] = std::move(im);
                }
            }
            PF_CHECKIN_PARAM(in_data, &def);
            if (stop) { prog.close(); return fail(std::strstr(why, "pick") ? "no source layer" : "size mismatch", why); }
        }
    }
    if (err) return err;
    if (!k || aw < 32 || ah < 32) { prog.close(); return fail("could not read frames", "jd_4Dwarper: could not read the layer's frames."); }
    for (int i = 0; i < n; ++i) if (T[i].w != aw || T[i].h != ah) T[i] = moco::Image(aw, ah, 0.f);   // missing: black
    for (int i = 0; i < nS; ++i) if (S[i].w != aw || S[i].h != ah) S[i] = moco::Image(aw, ah, 0.f);
    // ignore masks: binarised at half peak and grown, exactly as warper_cli (shared w4d::user_mask)
    const int grow = w4d::mask_grow_analysis_px(st, k);
    double rx0 = FIX_2_FLOAT(params[P_REGION_TL]->u.td.x_value) / k, ry0 = FIX_2_FLOAT(params[P_REGION_TL]->u.td.y_value) / k;
    double rx1 = FIX_2_FLOAT(params[P_REGION_BR]->u.td.x_value) / k, ry1 = FIX_2_FLOAT(params[P_REGION_BR]->u.td.y_value) / k;
    if (rx1 < rx0) std::swap(rx0, rx1);
    if (ry1 < ry0) std::swap(ry0, ry1);
    const bool region = rx0 > 1 || ry0 > 1 || rx1 < aw - 2 || ry1 < ah - 2;
    if (region && ((rx1 - rx0) < 32 || (ry1 - ry0) < 32)) { prog.close(); return fail("analysis region too small", "jd_4Dwarper: the Analysis Region is too small."); }
    std::vector<moco::Image> MK, MKS;
    if (use_t || region) {
        MK.resize(n);
        for (int i = 0; i < n; ++i) {
            MK[i] = use_t && MKr[i].w == aw && MKr[i].h == ah ? w4d::user_mask(MKr[i], grow) : moco::Image(aw, ah, 0.f);
            if (region) for (int y = 0; y < ah; ++y) for (int x = 0; x < aw; ++x) if (x < rx0 || x > rx1 || y < ry0 || y > ry1) MK[i].at(x, y) = 1.f;
        }
    }
    if (use_s) {
        MKS.resize(nS);
        for (int i = 0; i < nS; ++i) MKS[i] = MKSr[i].w == aw && MKSr[i].h == ah ? w4d::user_mask(MKSr[i], grow) : moco::Image(aw, ah, 0.f);
    }
    MKr.clear(); MKSr.clear();
    const std::string dump = DumpDir();
    if (!dump.empty()) {
        for (int i = 0; i < n; ++i) {
            if (!DumpFrame(i, n)) continue;
            const std::string f = std::to_string(f0 + i);
            WritePFM(dump + "\\T_" + f + ".pfm", T[i]);
            if (i < nS && i >= s_lo) WritePFM(dump + "\\S_" + f + ".pfm", S[i]);
            if (!MK.empty()) WritePFM(dump + "\\MK_" + f + ".pfm", MK[i]);
            if (!MKS.empty() && i < nS) WritePFM(dump + "\\MKS_" + f + ".pfm", MKS[i]);
        }
        PF_ParamDef d0; AEFX_CLR_STRUCT(d0);
        int fmt = -1;
        if (!PF_CHECKOUT_PARAM(in_data, P_INPUT, f0 * in_data->time_step, in_data->time_step, in_data->time_scale, &d0)) {
            if (d0.u.ld.data) fmt = int(FormatOf(in_data, out_data, &d0.u.ld));
            PF_CHECKIN_PARAM(in_data, &d0);
        }
        if (FILE* f = std::fopen((dump + "\\fetch.txt").c_str(), "w")) {
            std::fprintf(f, "jd_4Dwarper %d.%d.%d\nlayer %dx%d analysis %dx%d k %d\nfirst layer frame f0 %d frames n %d source frames 0..%d (s_lo %d, known %d)\n"
                            "time_step %d time_scale %u frame_dur %.9f s\npixel format of the target (PF_PixelFormat) %d\n"
                            "target ignore %d source ignore %d channel %s grow %d analysis px\n",
                         W4D_MAJOR_VERSION, W4D_MINOR_VERSION, W4D_BUG_VERSION, full_w, full_h, aw, ah, k, f0, n, nS - 1, s_lo, int(src_known),
                         int(in_data->time_step), unsigned(in_data->time_scale), fdur, fmt, int(use_t), int(use_s), luma ? "luma" : "alpha", grow);
            std::fclose(f);
        }
    }
    double fetch_s = std::chrono::duration<double>(Clock::now() - t0).count();

    // 2) worker threads: the fits (no After Effects calls); this thread only updates the progress bar and watches Esc
    // with an ignore layer the stiffness prior is not applied (judge loop round 11), unless the user asks for it
    moco::ShotOptions opt = w4d::to_options(st, aw, use_t || use_s);
    if (!MK.empty()) opt.ignore = [&](int t) -> const moco::Image* { return &MK[t]; };
    if (!MKS.empty()) opt.ignore_source = [&](int t) -> const moco::Image* { return t >= 0 && t < int(MKS.size()) ? &MKS[t] : nullptr; };
    prog.open(L"jd_4Dwarper: Effortlessly folding space and time");
    opt.progress = [&](int done, int total) { return prog.update(done, std::max(total, 1)); };
    moco::ShotResult R;
    auto t1 = Clock::now();
    try {
        R = moco::align_shot(n, nS, [&](int i) -> const moco::Image& { return T[i]; },
                             [&](int i) -> const moco::Image& { return S[i]; }, opt);
    } catch (...) {
        prog.close();
        return fail("out of memory", "jd_4Dwarper: analysis failed (out of memory?).");
    }
    prog.close();
    double solve_s = std::chrono::duration<double>(Clock::now() - t1).count();
    double off_min = 0, off_med = 0, off_max = 0;   // final source-minus-target time offset over the shot (frames)
    if (!R.cancelled && int(R.src_time.size()) == n) {
        std::vector<double> o(n);
        for (int t = 0; t < n; ++t) o[t] = R.src_time[t] - t;
        std::sort(o.begin(), o.end());
        off_min = o.front(); off_med = o[n / 2]; off_max = o.back();
    }
    if (FILE* f = std::fopen(LogPath().c_str(), "a")) {
        std::time_t now = std::time(nullptr); char stamp[32] = "";
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "%s  jd_4Dwarper %d.%d.%d  frames %d  layer %dx%d  analysis %dx%d (1/%d)  threads %d  "
                        "fetch %.2f s (%.3f s/frame)  solve %.2f s (%.3f s/frame)  total %.2f s  coarse offset %d  "
                        "final offset min %.3f median %.3f max %.3f fr  range layer frames %d..%d (%s)  "
                        "source frames %d..%d (%s)  target ignore %s  source ignore %s  channel %s  grow %d px  region %s  "
                        "auto ignore %s  mode %d  field %dx%d smooth %.2f  hold %d  stiffness %.2f/%.2f  affine %d..%d  held first/last source frame %d/%d  dump %s  %s\n",
                     stamp, W4D_MAJOR_VERSION, W4D_MINOR_VERSION, W4D_BUG_VERSION, n, full_w, full_h, aw, ah, k, opt.threads,
                     fetch_s, fetch_s / n, solve_s, solve_s / n, fetch_s + solve_s, R.coarse_offset,
                     off_min, off_med, off_max, f0, f0 + n - 1, trimmed ? "layer in/out" : "whole source",
                     f0 + s_lo, f0 + s_hi, src_known ? "source in/out" : "NOT clamped",
                     use_t ? "on" : (tmask_size_bad ? "SKIPPED (size differs)" : "none"),
                     use_s ? "on" : (smask_size_bad ? "SKIPPED (size differs)" : "none"), luma ? "luma" : "alpha",
                     st.mask_grow_px, region ? "on" : "full frame", st.auto_ignore ? "on" : "off", st.mode, st.grid_w, st.grid_h,
                     st.grid_smooth, st.time_trust_end >= 0 ? f0 + st.time_trust_end : -1, opt.prior_persp, opt.prior_lin,
                     st.affine_lo <= st.affine_hi ? f0 + st.affine_lo : -1, st.affine_lo <= st.affine_hi ? f0 + st.affine_hi : -1,
                     R.held_start, R.held_end, dump.empty() ? "off" : "on", R.cancelled ? "CANCELLED" : "done");
        std::fclose(f);
    }
    if (R.cancelled) return fail("cancelled", nullptr);   // previous analysis is kept
    if (!dump.empty() && int(R.src_time.size()) == n)
        if (FILE* f = std::fopen((dump + "\\solve.csv").c_str(), "w")) {
            std::fprintf(f, "layer_frame,src_time,offset,held\n");
            for (int t = 0; t < n; ++t)
                std::fprintf(f, "%d,%.5f,%.5f,%d\n", f0 + t, R.src_time[t], R.src_time[t] - t,
                             int(t < R.held_start || t >= n - R.held_end));
            std::fclose(f);
        }

    // 3) the record (shared writer), into a new handle
    const std::vector<unsigned char> blob = w4d::make_blob(R, n, nS, f0 * fdur, fdur, st.mode, s_lo);
    PF_Handle h = nullptr;
    ERR(NewBlob(in_data, blob.size(), &h));
    if (err) return err;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    void* hp = suites.HandleSuite1()->host_lock_handle(h);
    std::memcpy(hp, blob.data(), blob.size());
    ((w4d::BlobHeader*)hp)->pad0 = SettingsKey(params);   // for "needs re-analysis"
    if (R.held_start + R.held_end > 0) {   // shown by the banner: "held the last source frame for N frames"
        char why[32]; std::snprintf(why, sizeof(why), "held %d %d", R.held_start, R.held_end);
        w4d::set_status((w4d::BlobHeader*)hp, w4d::kSolved, why);
    }
    suites.HandleSuite1()->host_unlock_handle(h);
    params[P_DATA]->u.arb_d.value = h;                      // AE takes ownership of the new value
    params[P_DATA]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
    out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
    return err;
}

// ---------------------------------------------------------------- status banner (comp / layer view overlay)
static PF_Err DrawBanner(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* ev) {
    if (!params[P_BANNER]->u.bd.value) return PF_Err_NONE;
    const PF_WindowType wt = (*ev->contextH)->w_type;
    if (wt != PF_Window_COMP && wt != PF_Window_LAYER) return PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    uint32_t status = w4d::kNone; char reason[40] = "";
    int frames = 0; double omin = 0, omax = 0; bool stale = false, old = false;
    if (PF_Handle h = params[P_DATA]->u.arb_d.value) {
        size_t bytes = (size_t)suites.HandleSuite1()->host_get_handle_size(h);
        const void* p = suites.HandleSuite1()->host_lock_handle(h);
        if (w4d::valid(p, bytes)) {
            status = w4d::status_of((const w4d::BlobHeader*)p);
            const w4d::BlobHeader* hd = (const w4d::BlobHeader*)p;
            std::snprintf(reason, sizeof(reason), "%s", w4d::reason_of(hd));
            frames = int(hd->frame_count);
            for (int i = 0; i < frames; ++i) {
                const double o = w4d::record(p, uint32_t(i))->src_frame - i;
                if (i == 0 || o < omin) omin = o;
                if (i == 0 || o > omax) omax = o;
            }
            stale = frames > 0 && hd->pad0 && hd->pad0 != SettingsKey(params);
        } else if (w4d::old_version(p, bytes)) {   // analysed by 0.1/0.2: not read
            old = true;
        }
        suites.HandleSuite1()->host_unlock_handle(h);
    }
    // top edge of the layer in view coordinates
    PF_FixedPoint pt[2]; pt[0].x = 0; pt[0].y = 0; pt[1].x = INT2FIX(in_data->width); pt[1].y = 0;
    for (int i = 0; i < 2; ++i) {
        if (wt == PF_Window_COMP) ev->cbs.layer_to_comp(ev->cbs.refcon, ev->contextH, in_data->current_time, in_data->time_scale, &pt[i]);
        ev->cbs.source_to_frame(ev->cbs.refcon, ev->contextH, &pt[i]);
    }
    float x0 = (float)FIX_2_FLOAT(pt[0].x), x1 = (float)FIX_2_FLOAT(pt[1].x), y = (float)std::min(FIX_2_FLOAT(pt[0].y), FIX_2_FLOAT(pt[1].y));
    if (x1 < x0) std::swap(x0, x1);
    if (x1 - x0 < 60) return PF_Err_NONE;

    // Plain states (0.3.3): Not analysed / Ready / Needs re-analysis / Failed, each with what to do.
    // (Analysing n % shows in the progress dialog: the comp view cannot redraw while Analyze runs.)
    char text[160];
    DRAWBOT_ColorRGBA col; col.alpha = 0.92f;
    auto set = [&](float r, float g, float b) { col.red = r; col.green = g; col.blue = b; };
    if (old) { set(0.85f, 0.45f, 0.05f); std::snprintf(text, sizeof(text), "jd_4Dwarper: needs re-analysis (made by an older version). Press Analyze."); }
    else if (status == w4d::kSolved && stale) { set(0.85f, 0.45f, 0.05f); std::snprintf(text, sizeof(text), "jd_4Dwarper: needs re-analysis (Setup or Advanced settings changed). Press Analyze."); }
    else if (status == w4d::kSolved) {
        set(0.13f, 0.55f, 0.25f);
        int hs = 0, he = 0;
        if (std::sscanf(reason, "held %d %d", &hs, &he) == 2 && hs + he > 0)
            std::snprintf(text, sizeof(text), "jd_4Dwarper: ready. %d frames, time offset %+.1f to %+.1f frames. Held the %s source frame for %d frames.",
                          frames, omin, omax, he ? "last" : "first", he ? he : hs);
        else std::snprintf(text, sizeof(text), "jd_4Dwarper: ready. %d frames, time offset %+.1f to %+.1f frames.", frames, omin, omax);
    }
    else if (status == w4d::kFailed) { set(0.80f, 0.14f, 0.14f); std::snprintf(text, sizeof(text), "jd_4Dwarper: failed (%s): %s.", reason[0] ? reason : "unknown", AdviceFor(reason)); }
    else { set(0.30f, 0.30f, 0.33f); std::snprintf(text, sizeof(text), "jd_4Dwarper: not analysed. Pick the Source Layer, then press Analyze."); }

    PF_Err err = PF_Err_NONE;
    DRAWBOT_Suites db;
    ERR(AEFX_AcquireDrawbotSuites(in_data, out_data, &db));
    if (err) return PF_Err_NONE;
    DRAWBOT_DrawRef dr = nullptr;
    suites.EffectCustomUISuite1()->PF_GetDrawingReference(ev->contextH, &dr);
    if (dr) {
        DRAWBOT_SupplierRef sup = nullptr; DRAWBOT_SurfaceRef surf = nullptr;
        db.drawbot_suiteP->GetSupplier(dr, &sup);
        db.drawbot_suiteP->GetSurface(dr, &surf);
        if (sup && surf) {
            DRAWBOT_RectF32 r; r.left = x0; r.top = y; r.width = x1 - x0; r.height = 24.f;
            db.surface_suiteP->PaintRect(surf, &col, &r);
            float fs = 12.f; db.supplier_suiteP->GetDefaultFontSize(sup, &fs);
            DRAWBOT_FontRef font = nullptr; DRAWBOT_BrushRef brush = nullptr;
            db.supplier_suiteP->NewDefaultFont(sup, fs * 1.15f, &font);
            DRAWBOT_ColorRGBA white; white.red = white.green = white.blue = white.alpha = 1.f;
            db.supplier_suiteP->NewBrush(sup, &white, &brush);
            if (font && brush) {
                DRAWBOT_UTF16Char u[160]; size_t i = 0;
                for (; text[i] && i < 159; ++i) u[i] = (DRAWBOT_UTF16Char)(unsigned char)text[i];
                u[i] = 0;
                DRAWBOT_PointF32 o; o.x = x0 + 10.f; o.y = y + 17.f;
                db.surface_suiteP->DrawString(surf, brush, font, u, &o, kDRAWBOT_TextAlignment_Default, kDRAWBOT_TextTruncation_None, 0.f);
            }
            if (brush) db.supplier_suiteP->ReleaseObject(reinterpret_cast<DRAWBOT_ObjectRef>(brush));
            if (font) db.supplier_suiteP->ReleaseObject(reinterpret_cast<DRAWBOT_ObjectRef>(font));
        }
    }
    AEFX_ReleaseDrawbotSuites(in_data, out_data);
    ev->evt_out_flags = PF_EO_HANDLED_EVENT;
    return PF_Err_NONE;
}

static PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], const PF_UserChangedParamExtra* extra) {
    if (extra->param_index == P_ANALYZE) return Analyze(in_data, out_data, params);
    if (extra->param_index == P_CLEAR) {
        PF_Handle h = nullptr;
        PF_Err err = NewBlob(in_data, sizeof(w4d::BlobHeader), &h);
        if (!err) {
            params[P_DATA]->u.arb_d.value = h;
            params[P_DATA]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
            out_data->out_flags |= PF_OutFlag_FORCE_RERENDER | PF_OutFlag_REFRESH_UI;
        }
        return err;
    }
    if (extra->param_index == P_BANNER) out_data->out_flags |= PF_OutFlag_REFRESH_UI;
    if (extra->param_index == P_HELP) {   // version + quick start in AE's message box (0.3.4: no URL, no browser)
        PF_Err err = About(in_data, out_data, params, nullptr);
        out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
        return err;
    }
    if (extra->param_index == P_DOCS) {   // 0.3.4: the docs open only from their own button
        ShellExecuteA(NULL, "open", W4D_DOCS_URL, NULL, NULL, SW_SHOWNORMAL);
        return PF_Err_NONE;
    }
    return PF_Err_NONE;
}

// ---------------------------------------------------------------- Render (SmartFX; reads the blob only)
// 0.3.0: each source frame is warped with ITS OWN homography (Ha for floor(src), Hb for floor(src)+1) plus its own
// residual field. By default only the NEARER whole source frame is shown (sharpest, as in the judge loop); "Blend
// Neighbour Frames" mixes the two, each through its own warp.
struct PreData {
    bool have, useB, old, passthrough;
    double space;                          // Space Amount 0..1 (1 = full mapping)
    float f;
    int view, edges;
    double HA[9], HB[9];                   // normalised; HA for checkout A, HB for checkout B
    int gw, gh;                            // field size; the fields follow this struct (A, then B)
    double rx0, ry0, rx1, ry1;             // Analysis Region in (downsampled) layer coordinates
    bool useM; PF_LRect m_rect;            // Target Ignore Layer, checked out only for the Ignored Zones view
    PF_LRect in_rect, a_rect, b_rect;      // where each checked-out world sits in (downsampled) layer coordinates
    double tw, th, sw, sh;                 // target and source frame sizes at this render resolution
};
static void DeletePre(void* p) { std::free(p); }
static const float* PreFieldA(const PreData* pd) { return pd->gw > 1 ? (const float*)(pd + 1) : nullptr; }
static const float* PreFieldB(const PreData* pd) { return pd->gw > 1 ? (const float*)(pd + 1) + size_t(pd->gw) * pd->gh * 2 : nullptr; }

// Bilinear sample. clamp = extend the edge pixels outwards. Returns the coverage (0..1) of the sample by the world.
template <typename PIX>
static inline float sample(const PF_EffectWorld* w, double x, double y, bool clamp, float out[4]) {
    if (clamp) {
        if (!(x == x) || !(y == y)) { out[0] = out[1] = out[2] = out[3] = 0; return 0.f; }
        x = std::min(std::max(x, 0.0), double(w->width - 1)); y = std::min(std::max(y, 0.0), double(w->height - 1));
    }
    if (!(x > -1 && y > -1 && x < w->width && y < w->height)) { out[0] = out[1] = out[2] = out[3] = 0; return 0.f; }
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = float(x - x0), fy = float(y - y0);
    float acc[4] = {0, 0, 0, 0}, cov = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int xi = x0 + i, yi = y0 + j;
            if (xi < 0 || yi < 0 || xi >= w->width || yi >= w->height) continue;   // transparent outside
            const PIX* p = (const PIX*)((const char*)w->data + size_t(yi) * w->rowbytes) + xi;
            float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            cov += wt;
            acc[0] += wt * float(p->alpha); acc[1] += wt * float(p->red); acc[2] += wt * float(p->green); acc[3] += wt * float(p->blue);
        }
    out[0] = acc[0]; out[1] = acc[1]; out[2] = acc[2]; out[3] = acc[3];
    return cov;
}

// Residual field at target pixel (x, y) of a tw x th frame, bilinear between the nodes (node (i, j) at the centre of
// cell (i, j)); unit converts normalised units to source pixels. Same as the core's grid lookup.
static inline void FieldAt(const float* g, int gw, int gh, double tw, double th, double x, double y, double unit, double& dx, double& dy) {
    double u = (x + 0.5) / tw * gw - 0.5, v = (y + 0.5) / th * gh - 0.5;
    u = std::min(std::max(u, 0.0), double(gw - 1)); v = std::min(std::max(v, 0.0), double(gh - 1));
    int i = std::min(int(u), gw - 2), j = std::min(int(v), gh - 2);
    if (i < 0) i = 0; if (j < 0) j = 0;
    const int i1 = std::min(i + 1, gw - 1), j1 = std::min(j + 1, gh - 1);
    const double fu = u - i, fv = v - j;
    const float *a = g + (j * gw + i) * 2, *b = g + (j * gw + i1) * 2, *c = g + (j1 * gw + i) * 2, *d = g + (j1 * gw + i1) * 2;
    dx = unit * ((a[0] * (1 - fu) + b[0] * fu) * (1 - fv) + (c[0] * (1 - fu) + d[0] * fu) * fv);
    dy = unit * ((a[1] * (1 - fu) + b[1] * fu) * (1 - fv) + (c[1] * (1 - fu) + d[1] * fu) * fv);
}

// Exposure match for the Difference view: per channel, target ~ gain * warped source + offset, least squares on a
// sparse grid of fully covered pixels, second round without the outliers (what really differs between the takes).
template <typename PIX>
static void match_exposure(const PF_EffectWorld* in, const PF_EffectWorld* A, const PreData& pd,
                           const moco::H3& H, float gain[4], float off[4]) {
    for (int c = 0; c < 4; ++c) { gain[c] = 1; off[c] = 0; }
    if (!in) return;
    double sigma[4] = {0, 0, 0, 0};
    const int step = std::max(2, int(in->width) / 300);
    for (int round = 0; round < 2; ++round) {
        double n[4] = {0}, ss[4] = {0}, st[4] = {0}, sss[4] = {0}, sst[4] = {0}, stt[4] = {0};
        for (int oy = 0; oy < in->height; oy += step) {
            const PIX* t = (const PIX*)((const char*)in->data + size_t(oy) * in->rowbytes);
            for (int ox = 0; ox < in->width; ox += step) {
                double sx, sy; H.apply(ox + pd.in_rect.left, oy + pd.in_rect.top, sx, sy);
                float a[4];
                float cov = sample<PIX>(A, sx - pd.a_rect.left, sy - pd.a_rect.top, false, a);
                if (cov < 0.999f) continue;
                const float tv[4] = {float(t[ox].alpha), float(t[ox].red), float(t[ox].green), float(t[ox].blue)};
                for (int c = 1; c < 4; ++c) {
                    double s = a[c], v = tv[c];
                    if (!(s == s) || !(v == v)) continue;
                    if (round && std::fabs(gain[c] * s + off[c] - v) > 2.0 * sigma[c]) continue;
                    n[c] += 1; ss[c] += s; st[c] += v; sss[c] += s * s; sst[c] += s * v; stt[c] += v * v;
                }
            }
        }
        for (int c = 1; c < 4; ++c) {
            double den = n[c] * sss[c] - ss[c] * ss[c];
            if (n[c] < 50 || !(den > 1e-12 * std::max(1.0, sss[c] * n[c]))) continue;
            double g = (n[c] * sst[c] - ss[c] * st[c]) / den;
            g = std::min(std::max(g, 0.2), 5.0);
            double o = (st[c] - g * ss[c]) / n[c];
            double r2 = (stt[c] - 2 * g * sst[c] - 2 * o * st[c] + g * g * sss[c] + 2 * g * o * ss[c] + n[c] * o * o) / n[c];
            gain[c] = float(g); off[c] = float(o); sigma[c] = std::sqrt(std::max(r2, 0.0)) + 1e-9;
        }
    }
}

// maxv > 0: integer formats (rounded, clamped to 0..maxv). maxv == 0: float, values pass through unclamped.
template <typename PIX>
static void warp_rows(const PF_EffectWorld* in, const PF_EffectWorld* A, const PF_EffectWorld* B, const PF_EffectWorld* Mw,
                      const PreData& pd, const moco::H3& HA, const moco::H3& HB, float maxv, PF_EffectWorld* out) {
    float gain[4] = {1, 1, 1, 1}, off[4] = {0, 0, 0, 0};
    if (pd.view == VIEW_DIFF) match_exposure<PIX>(in, A, pd, HA, gain, off);
    typedef decltype(((PIX*)nullptr)->red) CH;
    const float* fA = PreFieldA(&pd); const float* fB = PreFieldB(&pd);
    const double unit = pd.sw * 0.5;
    for (int oy = 0; oy < out->height; ++oy) {
        PIX* o = (PIX*)((char*)out->data + size_t(oy) * out->rowbytes);
        const double y = oy + pd.in_rect.top, x0 = pd.in_rect.left;          // layer coordinates of this scanline
        const PIX* t = in && oy < in->height ? (const PIX*)((const char*)in->data + size_t(oy) * in->rowbytes) : nullptr;
        // incremental projective coordinates along the scanline
        double X = HA.m[0] * x0 + HA.m[1] * y + HA.m[2], Y = HA.m[3] * x0 + HA.m[4] * y + HA.m[5], W = HA.m[6] * x0 + HA.m[7] * y + HA.m[8];
        for (int ox = 0; ox < out->width; ++ox, X += HA.m[0], Y += HA.m[3], W += HA.m[6]) {
            float a[4], b[4];
            const double lx = ox + x0;
            double sx = X / W, sy = Y / W;
            if (fA) { double dx, dy; FieldAt(fA, pd.gw, pd.gh, pd.tw, pd.th, lx, y, unit, dx, dy); sx += dx; sy += dy; }
            if (pd.space != 1.0) { sx = lx + pd.space * (sx - lx); sy = y + pd.space * (sy - y); }   // Space Amount: blend the mapping
            const bool clamp = pd.edges == EDGES_EXTEND;
            float cov = sample<PIX>(A, sx - pd.a_rect.left, sy - pd.a_rect.top, clamp, a);
            if (B) {
                double bx, by; HB.apply(lx, y, bx, by);
                if (fB) { double dx, dy; FieldAt(fB, pd.gw, pd.gh, pd.tw, pd.th, lx, y, unit, dx, dy); bx += dx; by += dy; }
                if (pd.space != 1.0) { bx = lx + pd.space * (bx - lx); by = y + pd.space * (by - y); }
                float cb = sample<PIX>(B, bx - pd.b_rect.left, by - pd.b_rect.top, clamp, b);
                for (int c = 0; c < 4; ++c) a[c] += pd.f * (b[c] - a[c]);
                cov += pd.f * (cb - cov);
            }
            const bool have_t = t && ox < in->width;
            if (pd.edges == EDGES_TARGET && have_t && cov < 0.999f) {   // where the warped source ends, show the target
                a[0] += (1 - cov) * float(t[ox].alpha); a[1] += (1 - cov) * float(t[ox].red);
                a[2] += (1 - cov) * float(t[ox].green); a[3] += (1 - cov) * float(t[ox].blue);
            }
            if (pd.view != VIEW_RESULT && have_t) {
                float tv[4] = {float(t[ox].alpha), float(t[ox].red), float(t[ox].green), float(t[ox].blue)};
                if (pd.view == VIEW_DIFF || pd.view == VIEW_DIFF_PLAIN) {
                    for (int c = 1; c < 4; ++c) a[c] = std::fabs(gain[c] * a[c] + off[c] - tv[c]);
                    a[0] = std::max(a[0], tv[0]);
                } else if (pd.view == VIEW_IGNORED) {
                    // target, tinted red where Analyze does not measure: outside the Analysis Region or under the
                    // Target Ignore Layer (alpha drawn; the Source Ignore Layer and automatic zones are not drawn)
                    const int ix = ox + pd.in_rect.left, iy = oy + pd.in_rect.top;
                    bool out_region = ix < pd.rx0 || ix > pd.rx1 || iy < pd.ry0 || iy > pd.ry1;
                    if (Mw) {
                        const int mx = ix - pd.m_rect.left, my = iy - pd.m_rect.top;
                        if (mx >= 0 && my >= 0 && mx < Mw->width && my < Mw->height) {
                            const PIX* mp = (const PIX*)((const char*)Mw->data + size_t(my) * Mw->rowbytes) + mx;
                            const float full = maxv > 0 ? maxv : 1.f;
                            const float lum = 0.2126f * float(mp->red) + 0.7152f * float(mp->green) + 0.0722f * float(mp->blue);
                            if (std::min(float(mp->alpha), lum) > 0.5f * full) out_region = true;
                        }
                    }
                    for (int c = 0; c < 4; ++c) a[c] = tv[c];
                    if (out_region) { const float full = maxv > 0 ? maxv : 1.f; a[1] = 0.5f * a[1] + 0.5f * full; a[2] *= 0.5f; a[3] *= 0.5f; }
                } else for (int c = 0; c < 4; ++c) a[c] = 0.5f * (a[c] + tv[c]);
            }
            if (maxv > 0) for (int c = 0; c < 4; ++c) a[c] = a[c] <= 0 ? 0.f : (a[c] >= maxv ? maxv : a[c] + 0.5f);
            o[ox].alpha = (CH)a[0]; o[ox].red = (CH)a[1]; o[ox].green = (CH)a[2]; o[ox].blue = (CH)a[3];
        }
    }
}

// This frame's record from the blob handle, plus its two fields and the source range. *old = an analysis made by an
// older 4Dwarper (not read: the user must analyse again).
struct RecordView {
    w4d::FrameRecord rec; double first = 0, dur = 0; int src_first = 0, src_frames = 0;
    std::vector<float> fa, fb; bool old = false;
};
static bool ReadRecord(PF_InData* in_data, PF_Handle h, RecordView* rv) {
    if (!h) return false;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    bool have = false;
    size_t bytes = (size_t)suites.HandleSuite1()->host_get_handle_size(h);
    const void* p = suites.HandleSuite1()->host_lock_handle(h);
    if (w4d::valid(p, bytes)) {
        const w4d::BlobHeader* hd = (const w4d::BlobHeader*)p;
        if (hd->frame_count && hd->frame_dur_s > 0) {
            rv->dur = hd->frame_dur_s; rv->first = hd->first_time_s;
            rv->src_first = int(hd->source_first); rv->src_frames = int(hd->source_frames);
            long idx = std::lround((double(in_data->current_time) / in_data->time_scale - rv->first) / rv->dur);
            if (idx >= 0 && idx < long(hd->frame_count)) {
                const w4d::FrameRecord* r = w4d::record(p, uint32_t(idx));
                rv->rec = *r;
                const float* fa = w4d::field_a(hd, r); const float* fb = w4d::field_b(hd, r);
                const size_t nf = size_t(r->grid_w) * r->grid_h * 2;
                if (fa && fb && r->grid_w > 1 && r->grid_h > 1) { rv->fa.assign(fa, fa + nf); rv->fb.assign(fb, fb + nf); }
                else rv->rec.grid_w = rv->rec.grid_h = 0;
                have = true;
            }
        }
    } else rv->old = w4d::old_version(p, bytes);
    suites.HandleSuite1()->host_unlock_handle(h);
    return have;
}

static PF_Err PreRender(PF_InData* in_data, PF_OutData*, PF_PreRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    PF_RenderRequest req = extra->input->output_request;
    PF_CheckoutResult in_res; AEFX_CLR_STRUCT(in_res);
    ERR(extra->cb->checkout_layer(in_data->effect_ref, P_INPUT, CK_IN, &req, in_data->current_time, in_data->time_step, in_data->time_scale, &in_res));
    if (err) return err;
    RecordView rv; AEFX_CLR_STRUCT(rv.rec);
    int view = VIEW_RESULT, edges = EDGES_EXTEND; bool blend = false, local = true, have = false;
    double space_amt = 1.0, time_amt = 1.0;
    double rx0 = -1e9, ry0 = -1e9, rx1 = 1e9, ry1 = 1e9;
    {
        PF_ParamDef d;
        auto get = [&](int idx) { AEFX_CLR_STRUCT(d); return PF_CHECKOUT_PARAM(in_data, idx, in_data->current_time, in_data->time_step, in_data->time_scale, &d) == PF_Err_NONE; };
        if (get(P_VIEW)) view = d.u.pd.value;
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_REGION_TL)) { rx0 = FIX_2_FLOAT(d.u.td.x_value); ry0 = FIX_2_FLOAT(d.u.td.y_value); }
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_REGION_BR)) { rx1 = FIX_2_FLOAT(d.u.td.x_value); ry1 = FIX_2_FLOAT(d.u.td.y_value); }
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_EDGES)) edges = d.u.pd.value;
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_BLEND)) blend = d.u.bd.value != 0;
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_LOCAL)) local = d.u.bd.value != 0;
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_SPACE_AMT)) space_amt = d.u.fs_d.value / 100.0;
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_TIME_AMT)) time_amt = d.u.fs_d.value / 100.0;
        PF_CHECKIN_PARAM(in_data, &d);
        if (get(P_DATA)) have = ReadRecord(in_data, d.u.arb_d.value, &rv);
        PF_CHECKIN_PARAM(in_data, &d);
    }
    const int gw = have && local ? int(rv.rec.grid_w) : 0, gh = have && local ? int(rv.rec.grid_h) : 0;
    const size_t nf = gw > 1 && gh > 1 ? size_t(gw) * gh * 2 : 0;
    PreData* pd = (PreData*)std::calloc(1, sizeof(PreData) + 2 * nf * sizeof(float));
    if (!pd) return PF_Err_OUT_OF_MEMORY;
    pd->in_rect = in_res.result_rect;
    const double dsx = double(in_data->downsample_x.num) / in_data->downsample_x.den, dsy = double(in_data->downsample_y.num) / in_data->downsample_y.den;
    pd->tw = in_res.ref_width * dsx; pd->th = in_res.ref_height * dsy;
    pd->view = view; pd->edges = edges; pd->have = have; pd->old = rv.old;
    pd->rx0 = std::min(rx0, rx1); pd->rx1 = std::max(rx0, rx1); pd->ry0 = std::min(ry0, ry1); pd->ry1 = std::max(ry0, ry1);
    if (pd->view == VIEW_IGNORED) {   // the Target Ignore Layer at this time, only to draw it
        PF_CheckoutResult rm; AEFX_CLR_STRUCT(rm);
        if (!extra->cb->checkout_layer(in_data->effect_ref, P_MASK, CK_M, &req, in_data->current_time, in_data->time_step, in_data->time_scale, &rm) &&
            rm.result_rect.right > rm.result_rect.left) { pd->useM = true; pd->m_rect = rm.result_rect; }
    }
    pd->space = std::min(std::max(space_amt, 0.0), 1.0);
    if (pd->have) {
        // which source frames, with the Amounts, always inside the source layer's in/out as analysed (shared with
        // warper_cli: w4d::pick_source). Every checkout below uses a frame from this pick.
        const int idx = int(std::lround((double(in_data->current_time) / in_data->time_scale - rv.first) / rv.dur));
        const int lo = rv.src_first, hi = rv.src_frames > 0 ? rv.src_frames - 1 : std::max(idx, 0) + 1000000;
        const w4d::SourcePick pk = w4d::pick_source(idx, rv.rec.src_frame, lo, hi, time_amt, space_amt, blend);
        pd->passthrough = pk.passthrough;
        long a = pk.a;
        pd->f = pk.f;
        const bool useB = pk.use_b;
        const double* Ha = pk.a_is_hb ? rv.rec.Hb : rv.rec.Ha; const double* Hb = pk.b_is_hb ? rv.rec.Hb : rv.rec.Ha;
        const float* FA = nf ? (pk.a_is_hb ? rv.fb.data() : rv.fa.data()) : nullptr;
        const float* FB = nf ? (pk.b_is_hb ? rv.fb.data() : rv.fa.data()) : nullptr;
        for (int i = 0; i < 9; ++i) { pd->HA[i] = Ha[i]; pd->HB[i] = Hb[i]; }
        pd->gw = nf ? gw : 0; pd->gh = nf ? gh : 0;
        if (nf) {
            float* dst = (float*)(pd + 1);
            std::memcpy(dst, FA, nf * sizeof(float));
            std::memcpy(dst + nf, FB, nf * sizeof(float));
        }
        auto to_time = [&](long fr) { return (A_long)std::llround((rv.first + fr * rv.dur) * in_data->time_scale); };
        PF_RenderRequest sreq = req;                      // the warp can reach anywhere in the source: ask for all of it
        sreq.rect.left = -30000; sreq.rect.top = -30000; sreq.rect.right = 30000; sreq.rect.bottom = 30000;
        PF_CheckoutResult ra; AEFX_CLR_STRUCT(ra);
        // passthrough (both Amounts 0): the source layer at THIS time, untouched
        const A_long ta = pd->passthrough ? in_data->current_time : to_time(a);
        ERR(extra->cb->checkout_layer(in_data->effect_ref, P_SOURCE, CK_A, &sreq, ta, in_data->time_step, in_data->time_scale, &ra));
        pd->a_rect = ra.result_rect;
        pd->sw = ra.ref_width * dsx; pd->sh = ra.ref_height * dsy;
        if (err || ra.result_rect.right <= ra.result_rect.left || ra.ref_width <= 0) pd->have = false;   // source None or empty
        if (!err && pd->have && !pd->passthrough && useB) {
            PF_CheckoutResult rb; AEFX_CLR_STRUCT(rb);
            ERR(extra->cb->checkout_layer(in_data->effect_ref, P_SOURCE, CK_B, &sreq, to_time(a + 1), in_data->time_step, in_data->time_scale, &rb));
            pd->b_rect = rb.result_rect;
            pd->useB = !err && rb.result_rect.right > rb.result_rect.left;
        }
    }
    extra->output->result_rect = in_res.result_rect;
    extra->output->max_result_rect = in_res.max_result_rect;
    extra->output->pre_render_data = pd;
    extra->output->delete_pre_render_data_func = DeletePre;
    return err;
}

// Normalised <-> pixel matrix for a frame of (possibly fractional) size w x h.
static moco::H3 NormOf(double w, double h) {
    moco::H3 N; const double s = w * 0.5;
    N.m[0] = 1.0 / s; N.m[2] = -(w - 1) * 0.5 / s; N.m[4] = 1.0 / s; N.m[5] = -(h - 1) * 0.5 / s;
    return N;
}

static PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra) {
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    const PreData* pd = (const PreData*)extra->input->pre_render_data;
    PF_EffectWorld *in = nullptr, *A = nullptr, *B = nullptr, *M = nullptr, *out = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CK_IN, &in));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &out));
    if (!err && pd && pd->useM && extra->cb->checkout_layer_pixels(in_data->effect_ref, CK_M, &M)) M = nullptr;
    if (!err && pd && pd->have) {
        ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CK_A, &A));
        if (!err && pd->useB) ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, CK_B, &B));
    }
    if (!err && out) {
        if (pd && pd->have && pd->passthrough && A && A->data && A->width == out->width && A->height == out->height &&
            pd->a_rect.left == pd->in_rect.left && pd->a_rect.top == pd->in_rect.top) {
            err = suites.WorldTransformSuite1()->copy(in_data->effect_ref, A, out, NULL, NULL);   // Amounts 0 / 0: untouched source
        } else if (!pd || !pd->have || !A || !A->data) {
            if (in && in->data) err = suites.WorldTransformSuite1()->copy_hq(in_data->effect_ref, in, out, NULL, NULL);   // not analysed: pass through
        } else {
            moco::H3 Ha, Hb; for (int i = 0; i < 9; ++i) { Ha.m[i] = pd->HA[i]; Hb.m[i] = pd->HB[i]; }
            // target pixels -> normalised -> source pixels, each layer with its own size at this render resolution
            const moco::H3 Ns = NormOf(pd->sw, pd->sh).inverse(), Nt = NormOf(pd->tw, pd->th);
            const moco::H3 HA = Ns * Ha * Nt, HB = Ns * Hb * Nt;
            const PF_EffectWorld* Bw = B && B->data ? B : nullptr;
            const PF_EffectWorld* inw = in && in->data ? in : nullptr;
            const PF_EffectWorld* Mw = M && M->data ? M : nullptr;
            switch (FormatOf(in_data, out_data, out)) {
            case PF_PixelFormat_ARGB128: warp_rows<PF_PixelFloat>(inw, A, Bw, Mw, *pd, HA, HB, 0.f, out); break;
            case PF_PixelFormat_ARGB64: warp_rows<PF_Pixel16>(inw, A, Bw, Mw, *pd, HA, HB, 32768.f, out); break;
            default: warp_rows<PF_Pixel8>(inw, A, Bw, Mw, *pd, HA, HB, 255.f, out); break;
            }
        }
    }
    if (pd && pd->useM) extra->cb->checkin_layer_pixels(in_data->effect_ref, CK_M);
    extra->cb->checkin_layer_pixels(in_data->effect_ref, CK_IN);
    if (pd && pd->have) {
        extra->cb->checkin_layer_pixels(in_data->effect_ref, CK_A);
        if (pd->useB) extra->cb->checkin_layer_pixels(in_data->effect_ref, CK_B);
    }
    return err;
}

// ---------------------------------------------------------------- entry
extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite*, const char*, const char*) {
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr, W4D_NAME, W4D_MATCH_NAME, W4D_CATEGORY,
                                     8, "EffectMain", W4D_SUPPORT_URL);
    return result;
}

extern "C" DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                       PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
        case PF_Cmd_ABOUT: err = About(in_data, out_data, params, output); break;
        case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(in_data, out_data, params, output); break;
        case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data, params, output); break;
        case PF_Cmd_USER_CHANGED_PARAM: err = UserChangedParam(in_data, out_data, params, (const PF_UserChangedParamExtra*)extra); break;
        case PF_Cmd_ARBITRARY_CALLBACK: err = HandleArbitrary(in_data, out_data, (PF_ArbParamsExtra*)extra); break;
        case PF_Cmd_EVENT:
            if (((PF_EventExtra*)extra)->e_type == PF_Event_DRAW) err = DrawBanner(in_data, out_data, params, (PF_EventExtra*)extra);
            break;
        case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra); break;
        case PF_Cmd_SMART_RENDER: err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
        default: break;
        }
    } catch (PF_Err& thrown) {
        err = thrown;
    } catch (...) {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
