// Shared CPU render maths for 4Dwarper outside After Effects (warper_cli, tests): 8-bit RGB frames, homography
// warp with bilinear sampling, source edge extended, two-frame blend for sub-frame source times.
// Same formulas as ae/FourDWarper.cpp (sample / warp_rows, Edges = Extend Source Edge); the effect keeps its own
// copy for AE's 8/16/32 bpc worlds for now.
#pragma once
#include "align_shot.h"
#include "w4d_analysis.h"
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace moco {

struct RGB8 {                 // interleaved R,G,B, row-major
  int w = 0, h = 0;
  std::vector<uint8_t> p;
  RGB8() = default;
  RGB8(int W, int H) : w(W), h(H), p(size_t(W) * H * 3, 0) {}
};

// Rec.709 luma, box-reduced by an integer factor k, 0..1.
inline Image luma_reduced(const RGB8& im, int k) {
  Image o(std::max(1, im.w / k), std::max(1, im.h / k));
  const float inv = 1.f / (255.f * k * k);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x) {
      float s = 0;
      for (int j = 0; j < k; ++j) {
        const uint8_t* p = &im.p[(size_t(y * k + j) * im.w + x * k) * 3];
        for (int i = 0; i < k; ++i, p += 3) s += 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
      }
      o.at(x, y) = s * inv;
    }
  return o;
}

// extend = false: black where the source has no pixels (more than half a pixel outside it)
inline void sample_extend(const RGB8& s, double x, double y, float out[3], bool extend = true) {
  if (!(x == x) || !(y == y)) { out[0] = out[1] = out[2] = 0; return; }
  if (!extend && (x < -0.5 || y < -0.5 || x > s.w - 0.5 || y > s.h - 0.5)) { out[0] = out[1] = out[2] = 0; return; }
  x = std::min(std::max(x, 0.0), double(s.w - 1)); y = std::min(std::max(y, 0.0), double(s.h - 1));
  int x0 = std::min(int(x), s.w - 2), y0 = std::min(int(y), s.h - 2);
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
  const int x1 = std::min(x0 + 1, s.w - 1), y1 = std::min(y0 + 1, s.h - 1);
  const float fx = float(x - x0), fy = float(y - y0);
  const uint8_t *a = &s.p[(size_t(y0) * s.w + x0) * 3], *b = &s.p[(size_t(y0) * s.w + x1) * 3];
  const uint8_t *c = &s.p[(size_t(y1) * s.w + x0) * 3], *d = &s.p[(size_t(y1) * s.w + x1) * 3];
  for (int ch = 0; ch < 3; ++ch)
    out[ch] = (a[ch] * (1 - fx) + b[ch] * fx) * (1 - fy) + (c[ch] * (1 - fx) + d[ch] * fx) * fy;
}

// ---------------------------------------------------------------- per-pixel refinement (optional last stage)
// A dense residual flow in TARGET space, on a grid of small blocks: the target pixel p looks up the mapping at
// p + r(p) instead of p. Made by refine_flow() from the already aligned pair; off unless asked for.
struct PreFlow {
  int bw = 0, bh = 0;       // blocks
  double cell = 1;          // output pixels per block
  std::vector<float> d;     // bw*bh pairs (rx, ry), output pixels
};
inline void preflow_at(const PreFlow& f, double x, double y, double& rx, double& ry) {
  double u = (x + 0.5) / f.cell - 0.5, v = (y + 0.5) / f.cell - 0.5;
  u = std::min(std::max(u, 0.0), double(f.bw - 1)); v = std::min(std::max(v, 0.0), double(f.bh - 1));
  int i = std::min(int(u), f.bw - 2), j = std::min(int(v), f.bh - 2);
  if (i < 0) i = 0; if (j < 0) j = 0;
  const int i1 = std::min(i + 1, f.bw - 1), j1 = std::min(j + 1, f.bh - 1);
  const double fu = u - i, fv = v - j;
  const float *a = &f.d[(size_t(j) * f.bw + i) * 2], *b = &f.d[(size_t(j) * f.bw + i1) * 2];
  const float *c = &f.d[(size_t(j1) * f.bw + i) * 2], *d = &f.d[(size_t(j1) * f.bw + i1) * 2];
  rx = (a[0] * (1 - fu) + b[0] * fu) * (1 - fv) + (c[0] * (1 - fu) + d[0] * fu) * fv;
  ry = (a[1] * (1 - fu) + b[1] * fu) * (1 - fv) + (c[1] * (1 - fu) + d[1] * fu) * fv;
}

// Residual flow between the target T and the source S warped by H (+ field), both at analysis size, k = output
// pixels per analysis pixel. Set pixels only: target mask mt, source mask ms and uncovered pixels are excluded.
//  1. block matching on lighting-normalised images: 4 px blocks, 8x8 window, search +-radius, sub-pixel by
//     parabola; a block keeps its vector only if it lowers its own matching error by 20 % (the solver's residual,
//     nothing else), with confidence = the relative gain;
//  2. edge-aware smoothing on the block grid: each block between its own vector (weighted by confidence) and its
//     4 neighbours, a neighbour counting less the more its mean target luma differs (no smoothing across strong
//     edges of the target); blocks that kept nothing are pulled to zero;
//  3. fade to zero near mask and coverage borders.
inline PreFlow refine_flow(const Image& T, const Image& S, const H3& H, const std::vector<float>* fld, int gw, int gh,
                           const Image* mt, const Image* ms, int k, int radius = 3, double edge_sigma = 0.06,
                           double gain = 0.8, double min_ratio = 0.0) {
  // gain: a block keeps its vector only if its error falls below gain x its error at zero.
  // min_ratio: and only if its error at zero is above min_ratio x the frame's median block error (0 = any block):
  // touch only the blocks that are clearly worse than the rest of the frame.
  const int w = T.w, h = T.h, B = 4, bw = w / B, bh = h / B;
  PreFlow F; F.bw = bw; F.bh = bh; F.cell = double(B) * k; F.d.assign(size_t(bw) * bh * 2, 0.f);
  if (bw < 4 || bh < 4) return F;
  Image sw(w, h, 0.f), val(w, h, 0.f);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double sx, sy; H.apply(x, y, sx, sy);
      if (fld && gw > 1 && gh > 1) { double dx, dy; grid_at(*fld, gw, gh, w, h, x, y, dx, dy); sx += dx; sy += dy; }
      const bool in = sx >= 0 && sy >= 0 && sx <= w - 1 && sy <= h - 1;
      sw.at(x, y) = S.bilinear(float(sx), float(sy));
      if (in && !(mt && mt->at(x, y) > 0.5f) && !(ms && ms->at(int(sx + 0.5), int(sy + 0.5)) > 0.5f)) val.at(x, y) = 1.f;
    }
  const Image tn = normalize_local(T, 6), sn = normalize_local(sw, 6);
  const int N = bw * bh, R = radius, D = 2 * R + 1;
  std::vector<float> r0(size_t(N) * 2, 0.f), conf(N, 0.f), g(N, 0.f), vb(N, 0.f), c0s(N, -1.f);
  std::vector<double> cost(size_t(D) * D);
  for (int j = 0; j < bh; ++j)
    for (int i = 0; i < bw; ++i) {
      const int kb = j * bw + i, cx = B * i + B / 2, cy = B * j + B / 2;
      const int x0 = std::max(R, cx - 4), x1 = std::min(w - 1 - R, cx + 3), y0 = std::max(R, cy - 4), y1 = std::min(h - 1 - R, cy + 3);
      double gs = 0, vs = 0; int gc = 0;
      for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) { gs += T.at(x, y); vs += val.at(x, y); ++gc; }
      if (gc <= 0) continue;
      g[kb] = float(gs / gc); vb[kb] = float(vs / gc);
      if (vs < 0.75 * gc) continue;
      bool any = false;
      for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
          double s = 0; int c = 0;
          for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
              if (val.at(x, y) < 0.5f || val.at(x + dx, y + dy) < 0.5f) continue;
              s += std::min(std::fabs(sn.at(x + dx, y + dy) - tn.at(x, y)), 2.f); ++c;
            }
          cost[size_t(dy + R) * D + dx + R] = c >= 32 ? s / c : 1e30;
          any = any || c >= 32;
        }
      const double c0 = cost[size_t(R) * D + R];
      if (!any || !(c0 < 1e29)) continue;
      c0s[kb] = float(c0);
      int bx = 0, by = 0; double best = c0;
      for (int dy = -R; dy <= R; ++dy) for (int dx = -R; dx <= R; ++dx) { const double c = cost[size_t(dy + R) * D + dx + R]; if (c < best) { best = c; bx = dx; by = dy; } }
      if (!(best < gain * c0) || (!bx && !by)) continue;
      double sxp = 0, syp = 0;
      if (bx > -R && bx < R) { const double a = cost[size_t(by + R) * D + bx + R - 1], b = cost[size_t(by + R) * D + bx + R + 1], den = a - 2 * best + b; if (a < 1e29 && b < 1e29 && den > 1e-12) sxp = std::min(std::max(0.5 * (a - b) / den, -0.5), 0.5); }
      if (by > -R && by < R) { const double a = cost[size_t(by + R - 1) * D + bx + R], b = cost[size_t(by + R + 1) * D + bx + R], den = a - 2 * best + b; if (a < 1e29 && b < 1e29 && den > 1e-12) syp = std::min(std::max(0.5 * (a - b) / den, -0.5), 0.5); }
      r0[kb * 2] = float(bx + sxp); r0[kb * 2 + 1] = float(by + syp);
      conf[kb] = float((c0 - best) / std::max(c0, 1e-6));
    }
  if (min_ratio > 0) {
    std::vector<float> v; for (float c : c0s) if (c >= 0) v.push_back(c);
    if (!v.empty()) {
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
      const float thr = float(min_ratio) * v[v.size() / 2];
      for (int kb = 0; kb < N; ++kb) if (c0s[kb] < thr) { conf[kb] = 0.f; r0[kb * 2] = r0[kb * 2 + 1] = 0.f; }
    }
  }
  std::vector<float> cur(r0), nxt(r0);
  for (int rep = 0; rep < 8; ++rep) {
    for (int j = 0; j < bh; ++j)
      for (int i = 0; i < bw; ++i) {
        const int kb = j * bw + i;
        const double cw = 4.0 * conf[kb];
        double nx = cw * r0[kb * 2], ny = cw * r0[kb * 2 + 1], den = cw + (conf[kb] > 0 ? 0.0 : 0.5);   // no gain: pulled to zero
        const int di[4] = {-1, 1, 0, 0}, dj[4] = {0, 0, -1, 1};
        for (int q = 0; q < 4; ++q) {
          const int ii = i + di[q], jj = j + dj[q]; if (ii < 0 || jj < 0 || ii >= bw || jj >= bh) continue;
          const int kk = jj * bw + ii;
          const double wq = std::exp(-std::fabs(double(g[kb]) - g[kk]) / edge_sigma);
          nx += wq * cur[kk * 2]; ny += wq * cur[kk * 2 + 1]; den += wq;
        }
        nxt[kb * 2] = float(nx / std::max(den, 1e-9)); nxt[kb * 2 + 1] = float(ny / std::max(den, 1e-9));
      }
    cur.swap(nxt);
  }
  // fade near mask / coverage borders: blurred validity of the blocks
  for (int j = 0; j < bh; ++j)
    for (int i = 0; i < bw; ++i) {
      double s = 0; int c = 0;
      for (int jj = std::max(0, j - 2); jj <= std::min(bh - 1, j + 2); ++jj) for (int ii = std::max(0, i - 2); ii <= std::min(bw - 1, i + 2); ++ii) { s += vb[jj * bw + ii]; ++c; }
      const double fade = std::min(std::max((s / c - 0.6) / 0.3, 0.0), 1.0);
      F.d[size_t(j * bw + i) * 2] = float(cur[(j * bw + i) * 2] * fade * k);
      F.d[size_t(j * bw + i) * 2 + 1] = float(cur[(j * bw + i) * 2 + 1] * fade * k);
    }
  return F;
}

// out(x) = (1-f) * A(H x + D(x)) + f * B(H x + D(x)); H maps output (target) pixels to source pixels, D is the
// optional residual field (grid, normalised units, see ShotResult::grid). B may be null.
// HB: optional own homography for frame B (motion-compensated blend); otherwise B uses H too.
inline RGB8 warp_blend(const RGB8& A, const RGB8* B, float f, const H3& H, int w, int h,
                       const std::vector<float>* grid = nullptr, int gw = 0, int gh = 0, const H3* HB = nullptr,
                       const std::vector<float>* gridB = nullptr, bool extend = true,
                       const RGB8* target = nullptr, RGB8* coverage = nullptr, double space = 1.0,
                       const PreFlow* pre = nullptr) {
  // space < 1 (Space Amount): the sample position moves only part of the way, p + space * (M(p) - p).
  // target: where the source has no pixels (source position outside the source frame) the output shows the
  // target instead. coverage: if given, receives white where the source had pixels, black elsewhere.
  if (coverage) *coverage = RGB8(w, h);
  RGB8 o(w, h);
  const bool use_grid = grid && gw > 1 && gh > 1 && int(grid->size()) >= gw * gh * 2;
  for (int y = 0; y < h; ++y) {
    double X = H.m[1] * y + H.m[2], Y = H.m[4] * y + H.m[5], W = H.m[7] * y + H.m[8];   // incremental along the row
    uint8_t* q = &o.p[size_t(y) * w * 3];
    for (int x = 0; x < w; ++x, X += H.m[0], Y += H.m[3], W += H.m[6], q += 3) {
      float a[3], b[3];
      double sx = X / W, sy = Y / W;
      double px = x, py = y;                       // refinement: look the mapping up at p + r(p)
      if (pre) { double rx, ry; preflow_at(*pre, x, y, rx, ry); px += rx; py += ry; H.apply(px, py, sx, sy); }
      double bx = sx, by = sy;
      if (B && HB) HB->apply(px, py, bx, by);
      if (use_grid) {
        double dx, dy; grid_at(*grid, gw, gh, w, h, px, py, dx, dy); sx += dx; sy += dy;
        if (B && gridB) grid_at(*gridB, gw, gh, w, h, px, py, dx, dy);   // frame B has its own field
        bx += dx; by += dy;
      }
      if (space != 1.0) { sx = x + space * (sx - x); sy = y + space * (sy - y); bx = x + space * (bx - x); by = y + space * (by - y); }
      const bool covered = sx >= -0.5 && sy >= -0.5 && sx <= A.w - 0.5 && sy <= A.h - 0.5;
      if (coverage) { uint8_t* cq = &coverage->p[(size_t(y) * w + x) * 3]; cq[0] = cq[1] = cq[2] = covered ? 255 : 0; }
      if (!covered && target && target->w == w && target->h == h) {
        const uint8_t* tq = &target->p[(size_t(y) * w + x) * 3]; q[0] = tq[0]; q[1] = tq[1]; q[2] = tq[2];
        continue;
      }
      sample_extend(A, sx, sy, a, extend);
      if (B) { sample_extend(*B, bx, by, b, extend); for (int c = 0; c < 3; ++c) a[c] += f * (b[c] - a[c]); }
      for (int c = 0; c < 3; ++c) q[c] = uint8_t(std::min(255.f, std::max(0.f, a[c] + 0.5f)));
    }
  }
  return o;
}

// A target frame with the Time / Space Amounts (0.3.2), frames picked by w4d::pick_source. frames(i) gives
// source frame i at full resolution. Ha / Hb in PIXELS, fa / fb the record's fields (normalised), may be null.
// passthrough: the source frame t itself, untouched (bit-identical), and full coverage.
inline RGB8 render_amounts(const w4d::SourcePick& pk, const std::function<const RGB8&(int)>& frames, int t,
                           const H3& Ha, const H3& Hb, const std::vector<float>* fa, const std::vector<float>* fb,
                           int gw, int gh, int w, int h, bool extend, const RGB8* target, RGB8* coverage,
                           const PreFlow* pre = nullptr) {
  if (pk.passthrough) {
    if (coverage) { *coverage = RGB8(w, h); std::fill(coverage->p.begin(), coverage->p.end(), uint8_t(255)); }
    return frames(t);
  }
  const RGB8& A = frames(pk.a);
  const RGB8* B = pk.use_b ? &frames(pk.b) : nullptr;
  const H3& HA = pk.a_is_hb ? Hb : Ha; const H3& HB = pk.b_is_hb ? Hb : Ha;
  return warp_blend(A, B, pk.f, HA, w, h, pk.a_is_hb ? fb : fa, gw, gh, B ? &HB : nullptr, pk.b_is_hb ? fb : fa,
                    extend, target, coverage, pk.space, pre);
}

}  // namespace moco
