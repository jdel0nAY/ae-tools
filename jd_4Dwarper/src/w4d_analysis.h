// 4Dwarper analysis settings and record writing, shared by the After Effects effect, warper_cli and the
// verification test, so that the effect's Analyze and the offline loop run the SAME solve.
// Defaults = the judge loop's reference run r9, minus its shot-specific
// frame numbers (Hold Time After Frame and the affine range are off by default).
#pragma once
#include "align_shot.h"
#include "warp_record.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace w4d {

struct Settings {
  int mode = 1;                  // 1 = time + space, 2 = space only, 3 = time only
  int range = 8;                 // time search +- frames
  int analysis_width = 640;      // frames are box-reduced by a whole number to at most this width
  bool auto_ignore = false;      // automatic second pass without the zones that still differ (off: costs time)
  bool time_parallax = true;     // time from the residual after per-candidate homographies, with the wide search
  int grid_w = 48, grid_h = 27;  // residual field (Local Warp); 0 = off
  double grid_smooth = 0.25;     // field smoothness (spatial regularisation)
  int time_trust_end = -1;       // Hold Time After Frame (record index); -1 = off
  double prior_persp = 1.0;      // Perspective Stiffness
  double prior_lin = 0.5;        // Scale / Rotation Stiffness
  // Round 11 of the judge loop: once the talent is masked, the stiffness prior HURTS (r11_b, no prior: 6 bad
  // frames; with it: 10-12). So when any ignore mask is given the two stiffness values are not applied, unless
  // this is set. Without masks they apply as in 0.3.0.
  bool stiffness_with_masks = false;
  int affine_lo = 0, affine_hi = -1;   // affine-only frame range (record indices); off when lo > hi
  int mask_grow_px = 24;         // ignore masks are grown by this many FULL-resolution pixels
  // 0.3.5 speed: time candidates fitted on every 2nd frame (the frames between: interpolated scores + the 5
  // candidates around the best really fitted). Judge 3 (r19): median 0.212 / p95 1.92 / same 5 bad frames as r17.
  int cand_stride = 2;
  int threads = 1;
};

// Whole-number reduction factor for the analysis size (same formula as warper_cli --analysis-width).
inline int analysis_factor(int full_w, int analysis_width) {
  return std::max(1, (full_w + std::max(1, analysis_width) - 1) / std::max(1, analysis_width));
}

// Box mean with edge-clamped windows, separable (identical to warper_cli's, so masks match bit for bit).
inline moco::Image box_mean(const moco::Image& in, int r) {
  moco::Image t(in.w, in.h), o(in.w, in.h);
  for (int y = 0; y < in.h; ++y)
    for (int x = 0; x < in.w; ++x) { float s = 0; int c = 0; for (int i = std::max(0, x - r); i <= std::min(in.w - 1, x + r); ++i) { s += in.at(i, y); ++c; } t.at(x, y) = s / c; }
  for (int y = 0; y < in.h; ++y)
    for (int x = 0; x < in.w; ++x) { float s = 0; int c = 0; for (int j = std::max(0, y - r); j <= std::min(in.h - 1, y + r); ++j) { s += t.at(x, j); ++c; } o.at(x, y) = s / c; }
  return o;
}

// A user ignore mask at analysis size, from its reduced luma (or alpha) 0..1: binarised at half its peak, then
// grown by r analysis pixels. 1 = do not measure.
inline moco::Image user_mask(moco::Image m, int r) {
  float mx = 0.f;
  for (float v : m.p) mx = std::max(mx, v);
  for (float& v : m.p) v = mx > 0.f && v > 0.5f * mx ? 1.f : 0.f;
  if (r > 0) { m = box_mean(m, r); for (float& v : m.p) v = v > 0.02f ? 1.f : 0.f; }
  return m;
}
inline int mask_grow_analysis_px(const Settings& s, int k) { return std::max(0, s.mask_grow_px / std::max(1, k)); }

// ShotOptions for these settings. `aw` = analysis width. Masks are attached by the caller (ignore / ignore_source).
inline moco::ShotOptions to_options(const Settings& s, int aw, bool have_masks = false) {
  moco::ShotOptions o;
  o.range = s.mode == 2 ? 0 : s.range;     // space only: the source frame of the same time
  o.space.levels = aw >= 1024 ? 6 : 5;
  o.threads = std::max(1, s.threads);
  o.auto_ignore = s.auto_ignore;
  o.time_parallax = s.time_parallax;
  if (s.grid_w > 1 && s.grid_h > 1) { o.grid_w = s.grid_w; o.grid_h = s.grid_h; }
  o.grid_smooth = s.grid_smooth;
  o.cand_stride = std::max(1, s.cand_stride);
  o.time_trust_end = s.time_trust_end;
  const bool relax = have_masks && !s.stiffness_with_masks;
  o.prior_persp = relax ? 0.0 : s.prior_persp;
  o.prior_lin = relax ? 0.0 : s.prior_lin;
  if (s.affine_lo <= s.affine_hi) { o.dof_range_lo = s.affine_lo; o.dof_range_hi = s.affine_hi; o.dof_range_dof = 6; }
  return o;
}

// Which source frames a target frame shows (shared by the effect's render and warper_cli, 0.3.2).
// t = record index, solved = the record's source time, lo / hi = first / last source frame the analysis could
// use (source layer in/out), amounts 0..1. Every frame returned is inside [lo, hi].
//  - both amounts 0: passthrough = show the source layer's own frame t, untouched (bit-identical copy);
//  - source time = t + time_amount * (solved - t), clamped to [lo, hi];
//  - time amount 1 and blend off: the nearer whole frame (0.3.0 / 0.3.1 behaviour); otherwise the two
//    neighbouring frames blended by the fraction, so a Time Amount ramp does not step;
//  - a_is_hb / b_is_hb: each frame uses the record's Hb + field B if it is past floor(solved), else Ha + field A.
struct SourcePick { bool passthrough = false; int a = 0, b = 0; float f = 0.f; bool use_b = false; bool a_is_hb = false, b_is_hb = false; double space = 1.0; };
inline SourcePick pick_source(int t, double solved, int lo, int hi, double time_amount, double space_amount, bool blend) {
  SourcePick p;
  const double ta = std::min(std::max(time_amount, 0.0), 1.0), sa = std::min(std::max(space_amount, 0.0), 1.0);
  p.space = sa;
  if (ta <= 0.0 && sa <= 0.0) { p.passthrough = true; p.a = t; return p; }
  if (hi < lo) hi = lo;
  const double sc = std::min(std::max(solved, double(lo)), double(hi));
  const int a0 = std::max(lo, std::min(int(std::floor(sc)), hi - 1));   // the record's Ha frame (Hb = a0 + 1)
  double st = ta >= 1.0 ? sc : t + ta * (sc - t);
  st = std::min(std::max(st, double(lo)), double(hi));
  int a = int(std::floor(st)); double f = st - a;
  if (a >= hi) { a = hi; f = 0.0; }
  if (!blend && ta >= 1.0) { if (f >= 0.5) a = std::min(a + 1, hi); f = 0.0; }
  p.a = a; p.b = std::min(a + 1, hi); p.f = float(f); p.use_b = f > 0.002 && p.b != a;
  p.a_is_hb = a > a0; p.b_is_hb = p.b > a0;
  return p;
}

// The blob for a solved shot. n = target frames, nS = source frames the solve could use.
inline std::vector<unsigned char> make_blob(const moco::ShotResult& R, int n, int nS, double first_s, double dur_s, int mode,
                                             int source_first = 0) {
  const int gw = R.grid_w > 1 ? R.grid_w : 0, gh = R.grid_h > 1 ? R.grid_h : 0;
  std::vector<unsigned char> buf(blob_size(uint32_t(n), uint32_t(gw), uint32_t(gh)), 0);
  BlobHeader* h = (BlobHeader*)buf.data();
  init_header(h, uint32_t(n), uint32_t(gw), uint32_t(gh), first_s, dur_s);
  h->source_frames = uint32_t(std::max(0, nS));
  h->source_first = uint32_t(std::max(0, source_first));
  h->mode = uint32_t(mode);
  set_status(h, kSolved, "");
  const size_t nf = size_t(gw) * gh * 2;
  for (int t = 0; t < n; ++t) {
    FrameRecord* r = record(buf.data(), uint32_t(t));
    r->src_frame = mode == 2 ? double(t) : R.src_time[size_t(t)];
    const bool ident = mode == 3;
    for (int i = 0; i < 9; ++i) {
      const double I = (i % 4 == 0) ? 1.0 : 0.0;
      r->Hn[i] = ident ? I : R.Hf[size_t(t)].m[i];
      r->Ha[i] = ident ? I : (R.Ha.empty() ? R.Hf[size_t(t)].m[i] : R.Ha[size_t(t)].m[i]);
      r->Hb[i] = ident ? I : (R.Hb.empty() ? R.Hf[size_t(t)].m[i] : R.Hb[size_t(t)].m[i]);
    }
    r->quality = 1.f;
    r->grid_w = ident ? 0 : uint32_t(gw); r->grid_h = ident ? 0 : uint32_t(gh);
    if (!ident && nf) {
      float* fa = (float*)(r + 1);
      float* fb = fa + nf;
      const std::vector<float>& A = R.grid_a.empty() ? R.grid[size_t(t)] : R.grid_a[size_t(t)];
      const std::vector<float>& B = R.grid_b.empty() ? R.grid[size_t(t)] : R.grid_b[size_t(t)];
      if (A.size() >= nf) std::memcpy(fa, A.data(), nf * sizeof(float));
      if (B.size() >= nf) std::memcpy(fb, B.data(), nf * sizeof(float));
    }
  }
  return buf;
}

}  // namespace w4d
