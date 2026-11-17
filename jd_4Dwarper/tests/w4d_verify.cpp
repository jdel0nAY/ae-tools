// w4d_verify: the effect's Analyze path, offline. Loads the frames and Jean's masks the way the effect turns AE
// frames into analysis images (reduced luma, user masks binarised and grown), runs the shared settings
// (src/w4d_analysis.h) and the shared solve, writes the effect's record blob, reads it back like the effect's
// render does, and compares it with the CLI reference run r9 (records.json).
//
//   w4d_verify <target dir> <source dir> <target masks> <source masks> <reference records.json>
//              [--trust-end 128] [--affine 132,160]
// Exit 0 = every frame matches within float noise.
#define NOMINMAX
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <windows.h>
#include <gdiplus.h>

#include "../src/w4d_analysis.h"
#include "../src/render_core.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace moco;

static bool read_png(const fs::path& p, RGB8& out) {
  Gdiplus::Bitmap bmp(p.wstring().c_str());
  if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
  const int w = int(bmp.GetWidth()), h = int(bmp.GetHeight());
  if (w <= 0 || h <= 0) return false;
  Gdiplus::Rect r(0, 0, w, h);
  Gdiplus::BitmapData d;
  if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &d) != Gdiplus::Ok) return false;
  out = RGB8(w, h);
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = (const uint8_t*)d.Scan0 + ptrdiff_t(y) * d.Stride;
    uint8_t* q = &out.p[size_t(y) * w * 3];
    for (int x = 0; x < w; ++x, s += 3, q += 3) { q[0] = s[2]; q[1] = s[1]; q[2] = s[0]; }
  }
  bmp.UnlockBits(&d);
  return true;
}
static fs::path frame_path(const fs::path& dir, int i) { char n[32]; std::snprintf(n, sizeof(n), "%06d.png", i); return dir / n; }
static int count_frames(const fs::path& dir) { int n = 0; while (fs::exists(frame_path(dir, n))) ++n; return n; }
static std::vector<fs::path> mask_list(const fs::path& dir, int n) {   // any name ending in the frame number
  std::vector<fs::path> v(size_t(std::max(0, n)));
  if (dir.empty() || !fs::is_directory(dir)) return v;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    std::string ext = e.path().extension().string();
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext != ".png") continue;
    const std::string s = e.path().stem().string();
    size_t j = s.size();
    while (j > 0 && std::isdigit((unsigned char)s[j - 1])) --j;
    if (j == s.size() || s.size() - j > 9) continue;
    const int i = std::stoi(s.substr(j));
    if (i >= 0 && i < n && v[size_t(i)].empty()) v[size_t(i)] = e.path();
  }
  return v;
}

struct RefRecord { double src = 0; double H[9] = {0}; std::vector<double> field; };
static std::vector<RefRecord> read_reference(const fs::path& p) {
  std::vector<RefRecord> out;
  std::ifstream f(p);
  std::string line;
  auto nums_after = [](const std::string& l, const char* key) {
    std::vector<double> v; size_t k = l.find(key); if (k == std::string::npos) return v;
    k = l.find('[', k); size_t e = l.find(']', k); if (k == std::string::npos || e == std::string::npos) return v;
    std::stringstream ss(l.substr(k + 1, e - k - 1)); std::string tok;
    while (std::getline(ss, tok, ',')) v.push_back(std::atof(tok.c_str()));
    return v;
  };
  while (std::getline(f, line)) {
    if (line.find("\"frame\":") == std::string::npos) continue;
    RefRecord r;
    size_t k = line.find("\"src_time\":"); if (k == std::string::npos) continue;
    r.src = std::atof(line.c_str() + k + 11);
    std::vector<double> H = nums_after(line, "\"H\":");
    if (H.size() != 9) continue;
    for (int i = 0; i < 9; ++i) r.H[i] = H[size_t(i)];
    r.field = nums_after(line, "\"field\":");
    out.push_back(r);
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: w4d_verify <target> <source> <target masks> <source masks> <records.json> [--trust-end N] [--affine a,b]\n"); return 2; }
  Gdiplus::GdiplusStartupInput gsi; ULONG_PTR tok = 0;
  Gdiplus::GdiplusStartup(&tok, &gsi, nullptr);
  {   // 0.3.5: opt out of Windows power throttling (EcoQoS). With nobody at the screen Windows runs background
      // processes on the efficiency cores at low clock (3.5x slower here), which makes timings meaningless. After
      // Effects' Analyze runs in the foreground and is not throttled.
    PROCESS_POWER_THROTTLING_STATE pt = {};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    pt.StateMask = 0;   // = never throttle execution speed
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt));
  }
  const fs::path tdir = argv[1], sdir = argv[2], tm = argv[3], sm = argv[4], ref = argv[5];
  w4d::Settings st;   // the effect's defaults
  int cstride = -1;   // --cand-stride
  double tsmooth = -1;   // --time-smooth: override ShotOptions::time_smooth
  int conf = -1, conf_refine = -1; double conf_set = -1, conf_sharp = -1;   // --confidence / --conf-refine / --conf-set / --conf-sharp
  std::string dump_csv;   // --dump <csv>: per frame src_time, H, mask shares and the time candidate scores
  double noise = 0;     // --noise L: add uniform noise of +-L/255 to the analysed luma (sensitivity to decode differences)
  int src_frames = 0;   // --source-frames N: only source frames 0..N-1 (a source out point, as the effect's in/out clamp)
  bool stiff = false;   // --stiffness 1: apply the stiffness prior although masks are given (0.3.0 behaviour)
  for (int i = 6; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--trust-end") st.time_trust_end = std::atoi(argv[i + 1]);
    if (k == "--affine") std::sscanf(argv[i + 1], "%d,%d", &st.affine_lo, &st.affine_hi);
    if (k == "--stiffness") stiff = std::atoi(argv[i + 1]) != 0;
    if (k == "--source-frames") src_frames = std::atoi(argv[i + 1]);
    if (k == "--noise") noise = std::atof(argv[i + 1]);
    if (k == "--dump") dump_csv = argv[i + 1];
    if (k == "--confidence") conf = std::atoi(argv[i + 1]);
    if (k == "--conf-refine") conf_refine = std::atoi(argv[i + 1]);
    if (k == "--conf-set") conf_set = std::atof(argv[i + 1]);
    if (k == "--conf-sharp") conf_sharp = std::atof(argv[i + 1]);
    if (k == "--time-smooth") tsmooth = std::atof(argv[i + 1]);
    if (k == "--cand-stride") cstride = std::atoi(argv[i + 1]);
  }
  unsigned hc = std::thread::hardware_concurrency();
  st.threads = int(std::max(1u, hc > 2 ? hc - 2 : 1u));
  const int n = std::min(count_frames(tdir), count_frames(sdir));
  const int nS = src_frames > 0 ? std::min(src_frames, n) : n;
  RGB8 first;
  if (n < 4 || !read_png(frame_path(tdir, 0), first)) { std::fprintf(stderr, "cannot read the frames\n"); return 2; }
  const int W = first.w, H = first.h, k = w4d::analysis_factor(W, st.analysis_width), aw = W / k, ah = H / k;
  const int grow = w4d::mask_grow_analysis_px(st, k);
  const auto mt = mask_list(tm, n), ms = mask_list(sm, n);
  std::vector<Image> T(n), S(n), MK(n), MKS(n);
  std::atomic<int> next{0}, bad{0};
  std::vector<std::thread> pool;
  for (int w = 0; w < st.threads; ++w)
    pool.emplace_back([&]() {
      for (int i; (i = next++) < n;) {
        RGB8 im;
        T[i] = read_png(frame_path(tdir, i), im) ? luma_reduced(im, k) : (++bad, Image(aw, ah, 0.f));
        S[i] = read_png(frame_path(sdir, i), im) ? luma_reduced(im, k) : (++bad, Image(aw, ah, 0.f));
        MK[i] = !mt[i].empty() && read_png(mt[i], im) ? w4d::user_mask(luma_reduced(im, k), grow) : Image(aw, ah, 0.f);
        MKS[i] = !ms[i].empty() && read_png(ms[i], im) ? w4d::user_mask(luma_reduced(im, k), grow) : Image(aw, ah, 0.f);
      }
    });
  for (auto& t : pool) t.join();
  if (noise > 0) {   // deterministic per frame (same result for any thread count)
    for (int i = 0; i < n; ++i)
      for (Image* im : {&T[i], &S[i]}) {
        uint32_t st = 2166136261u ^ uint32_t(i * 7919 + (im == &S[i]));
        for (float& v : im->p) { st = st * 1664525u + 1013904223u; v += float(noise / 255.0 * ((st >> 8) / 8388608.0 - 1.0)); }
      }
    std::printf("luma noise +-%.2f levels added\n", noise);
  }
  bool any_mask = false;
  for (const auto& p : mt) any_mask = any_mask || !p.empty();
  for (const auto& p : ms) any_mask = any_mask || !p.empty();
  if (stiff) st.stiffness_with_masks = true;
  ShotOptions o = w4d::to_options(st, aw, any_mask);   // as the effect: with masks the stiffness prior is not applied
  o.ignore = [&](int t) -> const Image* { return &MK[t]; };
  if (conf >= 0) o.confidence = conf != 0;
  if (tsmooth >= 0) o.time_smooth = tsmooth;
  if (cstride > 0) o.cand_stride = cstride;
  if (conf_refine >= 0) o.conf_refine = conf_refine != 0;
  if (conf_set >= 0) o.conf_min_set = conf_set;
  if (conf_sharp >= 0) o.conf_min_sharp = conf_sharp;
  o.ignore_source = [&](int t) -> const Image* { return &MKS[t]; };
  const auto t0 = std::chrono::steady_clock::now();
  o.ignore_source = [&](int t) -> const Image* { return t >= 0 && t < nS ? &MKS[t] : nullptr; };
  ShotResult R = align_shot(n, nS, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[std::min(i, nS - 1)]; }, o);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::vector<unsigned char> blob = w4d::make_blob(R, n, nS, 0.0, 1.0 / 25, st.mode);
  std::printf("confidence %s: %d frames interpolated\n", o.confidence ? "on" : "off", R.interpolated);
  {
    std::vector<double> o2(n); for (int t = 0; t < n; ++t) o2[t] = R.src_time[t] - t;
    std::vector<double> so = o2; std::sort(so.begin(), so.end());
    std::printf("source frames 0..%d  final offset min %.3f median %.3f max %.3f fr  last 12 src_time:", nS - 1, so.front(), so[n / 2], so.back());
    for (int t = std::max(0, n - 12); t < n; ++t) std::printf(" %.2f", R.src_time[t]);
    std::printf("\n");
  }

  // read the blob back the way the effect does, compare with the reference
  if (!w4d::valid(blob.data(), blob.size())) { std::printf("FAIL: blob not valid\n"); return 1; }
  const w4d::BlobHeader* hd = (const w4d::BlobHeader*)blob.data();
  const std::vector<RefRecord> rr = read_reference(ref);
  if (int(rr.size()) != n || int(hd->frame_count) != n) { std::printf("FAIL: %zu reference records, %u in the blob, %d frames\n", rr.size(), hd->frame_count, n); return 1; }
  double dt = 0, dh = 0, df = 0, dab = 0; int worst_t = -1;
  for (int t = 0; t < n; ++t) {
    const w4d::FrameRecord* r = w4d::record(blob.data(), uint32_t(t));
    const double e = std::fabs(r->src_frame - rr[size_t(t)].src);
    if (e > dt) { dt = e; worst_t = t; }
    // H: compare the mappings (rows may differ by scale), over the frame corners in normalised coordinates
    H3 a, b; for (int i = 0; i < 9; ++i) { a.m[i] = r->Hn[i]; b.m[i] = rr[size_t(t)].H[i]; }
    const double us[4] = {-1, 1, -1, 1}, vs[4] = {-0.5625, -0.5625, 0.5625, 0.5625};
    double dhf = 0, corner_x = 0, corner_y = 0;
    for (int c = 0; c < 4; ++c) {
      double ax, ay, bx, by; a.apply(us[c], vs[c], ax, ay); b.apply(us[c], vs[c], bx, by);
      dhf = std::max(dhf, std::hypot(ax - bx, ay - by) * W * 0.5);
      if (c == 3) { corner_x = ax; corner_y = ay; }
    }
    dh = std::max(dh, dhf);
    if (dhf > 5.0)   // list the frames that differ from the reference (with --source-frames: where the clamp acts)
      std::printf("  frame %d: src %.3f (ref %.3f), H differs by %.1f px, bottom-right corner maps to (%.3f, %.3f)\n", t, r->src_frame, rr[size_t(t)].src, dhf, corner_x, corner_y);
    // the blended field of the reference = what this solve's R.grid holds
    const std::vector<double>& rf = rr[size_t(t)].field;
    if (!rf.empty() && R.grid[size_t(t)].size() == rf.size())
      for (size_t i = 0; i < rf.size(); ++i) df = std::max(df, std::fabs(R.grid[size_t(t)][i] - rf[i]) * W * 0.5);
    // the blob's two fields = the solve's per-source-frame fields
    const float* fa = w4d::field_a(hd, r); const float* fb = w4d::field_b(hd, r);
    if (fa && fb && !R.grid_a.empty())
      for (size_t i = 0; i < R.grid_a[size_t(t)].size(); ++i)
        dab = std::max(dab, double(std::max(std::fabs(fa[i] - R.grid_a[size_t(t)][i]), std::fabs(fb[i] - R.grid_b[size_t(t)][i]))));
  }
  if (!dump_csv.empty())
    if (FILE* f = std::fopen(dump_csv.c_str(), "w")) {
      std::fprintf(f, "t,src,mk,mks");
      for (int i = 0; i < 9; ++i) std::fprintf(f, ",h%d", i);
      const int nc = R.time_scores.empty() ? 0 : int(R.time_scores[0].size());
      for (int i = 0; i < nc; ++i) std::fprintf(f, ",c%d", i);
      std::fprintf(f, "\n");
      for (int t = 0; t < n; ++t) {
        double a = 0, b = 0; for (float v : MK[t].p) a += v; for (float v : MKS[std::min(t, nS - 1)].p) b += v;
        std::fprintf(f, "%d,%.5f,%.4f,%.4f", t, R.src_time[t], a / MK[t].p.size(), b / MKS[0].p.size());
        const w4d::FrameRecord* r = w4d::record(blob.data(), uint32_t(t));
        for (int i = 0; i < 9; ++i) std::fprintf(f, ",%.9g", r->Hn[i]);
        for (int i = 0; i < nc; ++i) std::fprintf(f, ",%.6g", R.time_scores[t][i]);
        std::fprintf(f, "\n");
      }
      std::fclose(f);
    }
  const bool ok = !bad && dt < 2e-4 && dh < 0.01 && df < 0.01 && dab == 0.0;
  std::printf("w4d_verify: %d frames %dx%d, analysis %dx%d (1/%d), mask grow %d px at analysis size, solve %.1f s, threads %d\n",
              n, W, H, aw, ah, k, grow, secs, st.threads);
  std::printf("  vs reference: max |src_time| diff %.2e fr (frame %d), max H mapping diff %.2e px, max field diff %.2e px "
              "(1080p px), blob fields vs solve %.1e | %s\n", dt, worst_t, dh, df, dab, ok ? "PASS" : "FAIL");
  Gdiplus::GdiplusShutdown(tok);
  return ok ? 0 : 1;
}
