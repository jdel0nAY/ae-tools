#include "align_core.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace moco {

float Image::bilinear(float x, float y, bool* inside) const {
  if (!(x == x) || !(y == y)) { if (inside) *inside = false; return 0.f; }
  bool in = x >= 0 && y >= 0 && x <= w - 1 && y <= h - 1;
  if (inside) *inside = in;
  x = std::min(std::max(x, 0.f), float(w - 1));
  y = std::min(std::max(y, 0.f), float(h - 1));
  int x0 = std::min(int(x), w - 2), y0 = std::min(int(y), h - 2);
  float fx = x - x0, fy = y - y0;
  const float* r0 = &p[size_t(y0) * w + x0];
  const float* r1 = r0 + w;
  return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy;
}

H3 H3::operator*(const H3& b) const {
  H3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += m[i * 3 + k] * b.m[k * 3 + j];
      r.m[i * 3 + j] = s;
    }
  return r;
}

H3 H3::inverse() const {
  const double* a = m;
  double c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
  double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
  H3 r;
  double id = 1.0 / det;
  r.m[0] = c00 * id; r.m[1] = (a[2] * a[7] - a[1] * a[8]) * id; r.m[2] = (a[1] * a[5] - a[2] * a[4]) * id;
  r.m[3] = c01 * id; r.m[4] = (a[0] * a[8] - a[2] * a[6]) * id; r.m[5] = (a[2] * a[3] - a[0] * a[5]) * id;
  r.m[6] = c02 * id; r.m[7] = (a[1] * a[6] - a[0] * a[7]) * id; r.m[8] = (a[0] * a[4] - a[1] * a[3]) * id;
  return r;
}

void H3::apply(double x, double y, double& ox, double& oy) const {
  double c = m[6] * x + m[7] * y + m[8];
  ox = (m[0] * x + m[1] * y + m[2]) / c;
  oy = (m[3] * x + m[4] * y + m[5]) / c;
}

static Image box_blur(const Image& in, int r) {
  Image t(in.w, in.h), o(in.w, in.h);
  for (int y = 0; y < in.h; ++y) {
    double s = 0; int n = 0;
    for (int x = -r; x < in.w + r; ++x) {
      if (x + r < in.w && x + r >= 0) { s += in.at(x + r, y); ++n; }
      if (x - r - 1 >= 0) { s -= in.at(x - r - 1, y); --n; }
      if (x >= 0 && x < in.w) t.at(x, y) = float(s / std::max(n, 1));
    }
  }
  for (int x = 0; x < in.w; ++x) {
    double s = 0; int n = 0;
    for (int y = -r; y < in.h + r; ++y) {
      if (y + r < in.h && y + r >= 0) { s += t.at(x, y + r); ++n; }
      if (y - r - 1 >= 0) { s -= t.at(x, y - r - 1); --n; }
      if (y >= 0 && y < in.h) o.at(x, y) = float(s / std::max(n, 1));
    }
  }
  return o;
}

Image normalize_local(const Image& in, int r) {
  Image mean = box_blur(in, r);
  Image sq(in.w, in.h);
  for (size_t i = 0; i < in.p.size(); ++i) { float d = in.p[i] - mean.p[i]; sq.p[i] = d * d; }
  Image var = box_blur(sq, r);
  Image o(in.w, in.h);
  for (size_t i = 0; i < in.p.size(); ++i) o.p[i] = (in.p[i] - mean.p[i]) / (std::sqrt(std::max(var.p[i], 0.f)) + 0.02f);
  return o;
}

Image downsample2(const Image& in) {
  Image o(std::max(in.w / 2, 1), std::max(in.h / 2, 1));
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x) {
      int x0 = std::min(2 * x, in.w - 1), x1 = std::min(2 * x + 1, in.w - 1);
      int y0 = std::min(2 * y, in.h - 1), y1 = std::min(2 * y + 1, in.h - 1);
      o.at(x, y) = 0.25f * (in.at(x0, y0) + in.at(x1, y0) + in.at(x0, y1) + in.at(x1, y1));
    }
  return o;
}

Image warp(const Image& src, const H3& H, Image* valid) {
  Image o(src.w, src.h);
  if (valid) *valid = Image(src.w, src.h);
  for (int y = 0; y < src.h; ++y)
    for (int x = 0; x < src.w; ++x) {
      double sx, sy; H.apply(x, y, sx, sy);
      bool in; o.at(x, y) = src.bilinear(float(sx), float(sy), &in);
      if (valid) valid->at(x, y) = in ? 1.f : 0.f;
    }
  return o;
}

// Level-L pixel -> normalised coords (resolution independent), as an affine H3.
static H3 norm_of_level(int w0, int h0, int L) {
  double f = double(1 << L), cx = (w0 - 1) * 0.5, cy = (h0 - 1) * 0.5, s = w0 * 0.5;
  H3 N;
  N.m[0] = f / s; N.m[2] = (0.5 * f - 0.5 - cx) / s;
  N.m[4] = f / s; N.m[5] = (0.5 * f - 0.5 - cy) / s;
  return N;
}

static bool solve(std::vector<double>& A, std::vector<double>& b, int n) {  // Gauss-Jordan, partial pivot
  for (int c = 0; c < n; ++c) {
    int piv = c;
    for (int r = c + 1; r < n; ++r) if (std::fabs(A[r * n + c]) > std::fabs(A[piv * n + c])) piv = r;
    if (std::fabs(A[piv * n + c]) < 1e-12) return false;
    for (int k = 0; k < n; ++k) std::swap(A[c * n + k], A[piv * n + k]);
    std::swap(b[c], b[piv]);
    for (int r = 0; r < n; ++r) {
      if (r == c) continue;
      double f = A[r * n + c] / A[c * n + c];
      for (int k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
      b[r] -= f * b[c];
    }
  }
  for (int c = 0; c < n; ++c) b[c] /= A[c * n + c];
  return true;
}

std::atomic<long long> g_prep_ns{0}, g_fit_ns{0}, g_fit_calls{0};   // 0.3.5 speed probe (W4D_TIMING)
SpaceResult align_space(const Image& target, const Image& source, const H3& init, const SpaceOptions& o) {
  const auto t_in = std::chrono::steady_clock::now();
  struct Probe { std::chrono::steady_clock::time_point t; ~Probe() { g_fit_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count(); ++g_fit_calls; } } probe{t_in};
  const int w0 = target.w, h0 = target.h;
  std::vector<Image> T{normalize_local(target, o.norm_radius)}, S{normalize_local(source, o.norm_radius)};
  std::vector<Image> M;
  if (o.ignore_mask) M.push_back(*o.ignore_mask);
  std::vector<Image> MS;
  if (o.source_ignore) MS.push_back(*o.source_ignore);
  for (int L = 1; L < o.levels; ++L) {
    T.push_back(downsample2(T.back())); S.push_back(downsample2(S.back()));
    if (o.ignore_mask) M.push_back(downsample2(M.back()));
    if (o.source_ignore) MS.push_back(downsample2(MS.back()));
  }
  g_prep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_in).count();
  // Work on Hn: normalised target -> normalised source.
  H3 N0 = norm_of_level(w0, h0, 0);
  H3 Hn = N0 * init * N0.inverse();
  H3 Pn;   // prior in the same normalised parametrisation (m[8] = 1)
  if (o.prior) { Pn = N0 * (*o.prior) * N0.inverse(); const double p8 = Pn.m[8]; for (double& v : Pn.m) v /= p8; }
  { const double h8 = Hn.m[8]; for (double& v : Hn.m) v /= h8; }
  // active parameter indices into m[0..7]
  std::vector<int> idx;
  if (o.dof == 2) idx = {2, 5};
  else if (o.dof == 6) idx = {0, 1, 2, 3, 4, 5};
  else idx = {0, 1, 2, 3, 4, 5, 6, 7};
  const int n = int(idx.size());
  SpaceResult res;
  for (int L = o.levels - 1; L >= 0; --L) {
    const Image& t = T[L]; const Image& s = S[L];
    H3 N = norm_of_level(w0, h0, L), Ni = N.inverse();
    double gscale = 1.0 / N.m[0];  // d(level px)/d(normalised)
    for (int it = 0; it < o.iters; ++it) {
      // robust scale from previous residuals: first pass computes errors
      std::vector<double> A(n * n, 0.0), b(n, 0.0);
      // 0.3.5 speed: per-thread buffers reused across iterations and calls (a fresh 15 MB buffer per iteration cost
      // page faults); same values, same order, so the results are bit-identical.
      struct Px { float e, gx, gy; double u, v, vx, vy, C; };
      static thread_local std::vector<float> errs; errs.clear(); errs.reserve(size_t(t.w) * t.h);
      static thread_local std::vector<Px> px; px.clear(); px.reserve(size_t(t.w) * t.h);
      for (int y = 1; y < t.h - 1; ++y)
        for (int x = 1; x < t.w - 1; ++x) {
          if (o.ignore_mask && M[L].at(x, y) > 0.5f) continue;
          double u, v; N.apply(x, y, u, v);
          double C = Hn.m[6] * u + Hn.m[7] * v + 1.0;
          double vx = (Hn.m[0] * u + Hn.m[1] * v + Hn.m[2]) / C;
          double vy = (Hn.m[3] * u + Hn.m[4] * v + Hn.m[5]) / C;
          double qx, qy; Ni.apply(vx, vy, qx, qy);
          if (!(qx >= 1 && qy >= 1 && qx <= s.w - 2 && qy <= s.h - 2)) continue;
          if (o.source_ignore && MS[L].at(std::min(int(qx + 0.5), MS[L].w - 1), std::min(int(qy + 0.5), MS[L].h - 1)) > 0.5f) continue;
          if (qx < 1 || qy < 1 || qx > s.w - 2 || qy > s.h - 2) continue;
          float val = s.bilinear(float(qx), float(qy));
          float gx = 0.5f * (s.bilinear(float(qx + 1), float(qy)) - s.bilinear(float(qx - 1), float(qy)));
          float gy = 0.5f * (s.bilinear(float(qx), float(qy + 1)) - s.bilinear(float(qx), float(qy - 1)));
          float e = val - t.at(x, y);
          px.push_back({e, float(gx * gscale), float(gy * gscale), u, v, vx, vy, C});
          errs.push_back(std::fabs(e));
        }
      if (px.size() < 50) break;
      std::nth_element(errs.begin(), errs.begin() + errs.size() / 2, errs.end());
      double sigma = std::max(1.4826 * errs[errs.size() / 2], 1e-4);
      double k = 1.345 * sigma;
      double sse = 0; size_t inl = 0;
      double J[8];
      for (const Px& q : px) {
        double ae = std::fabs(q.e), wgt = ae <= k ? 1.0 : k / ae;
        if (ae <= 3 * k) ++inl;
        sse += wgt * q.e * q.e;
        double full[8] = {q.gx * q.u / q.C, q.gx * q.v / q.C, q.gx / q.C,
                          q.gy * q.u / q.C, q.gy * q.v / q.C, q.gy / q.C,
                          -(q.gx * q.vx + q.gy * q.vy) * q.u / q.C, -(q.gx * q.vx + q.gy * q.vy) * q.v / q.C};
        for (int i = 0; i < n; ++i) J[i] = full[idx[i]];
        for (int i = 0; i < n; ++i) {
          b[i] -= wgt * J[i] * q.e;
          for (int j = i; j < n; ++j) A[i * n + j] += wgt * J[i] * J[j];
        }
      }
      for (int i = 0; i < n; ++i) for (int j = 0; j < i; ++j) A[i * n + j] = A[j * n + i];
      if (o.prior)
        for (int i = 0; i < n; ++i) {
          const double wp = o.prior_w[idx[i]];
          if (wp <= 0) continue;
          const double D = A[i * n + i];
          A[i * n + i] += wp * D; b[i] -= wp * D * (Hn.m[idx[i]] - Pn.m[idx[i]]);
        }
      for (int i = 0; i < n; ++i) A[i * n + i] *= 1.0 + 1e-6;
      res.residual = sse / px.size();
      res.inlier_frac = double(inl) / px.size();
      if (!solve(A, b, n)) break;
      double step = 0;
      for (int i = 0; i < n; ++i) { Hn.m[idx[i]] += b[i]; step += b[i] * b[i]; }
      if (std::sqrt(step) < 1e-6) break;
    }
  }
  res.H = N0.inverse() * Hn * N0;
  return res;
}

std::vector<double> align_time(int nT, int nS, const std::function<const Image&(int)>& getT,
                               const std::function<const Image&(int)>& getS, const H3& H,
                               const TimeOptions& o, std::vector<double>* raw) {
  auto prep = [&](const Image& im) {
    Image a = normalize_local(im, 6);
    for (int L = 0; L < o.level; ++L) a = downsample2(a);
    return a;
  };
  // Warp each source frame once into target space (at full res, then reduce).
  std::vector<Image> Tl(nT), Sl(nS), Vl(nS);
  for (int i = 0; i < nT; ++i) Tl[i] = prep(getT(i));
  for (int i = 0; i < nS; ++i) {
    Image valid;
    Image w = warp(getS(i), H, &valid);
    Sl[i] = prep(w);
    for (int L = 0; L < o.level; ++L) valid = downsample2(valid);
    Vl[i] = valid;
  }
  std::vector<double> off(nT, 0.0);
  for (int t = 0; t < nT; ++t) {
    std::vector<double> cost(2 * o.range + 1, 1e30);
    for (int d = -o.range; d <= o.range; ++d) {
      int s = t + d;
      if (s < 0 || s >= nS) continue;
      double sum = 0; size_t cnt = 0;
      const Image& a = Tl[t]; const Image& b = Sl[s]; const Image& v = Vl[s];
      for (size_t i = 0; i < a.p.size(); ++i)
        if (v.p[i] > 0.99f) { double e = a.p[i] - b.p[i]; sum += e * e; ++cnt; }
      if (cnt > 20) cost[d + o.range] = sum / cnt;
    }
    int best = int(std::min_element(cost.begin(), cost.end()) - cost.begin());
    double sub = 0;
    if (best > 0 && best < int(cost.size()) - 1 && cost[best - 1] < 1e29 && cost[best + 1] < 1e29) {
      double c0 = cost[best - 1], c1 = cost[best], c2 = cost[best + 1];
      double den = c0 - 2 * c1 + c2;
      if (den > 1e-12) sub = std::clamp(0.5 * (c0 - c2) / den, -0.5, 0.5);
    }
    off[t] = best - o.range + sub;
  }
  if (raw) *raw = off;
  // Smooth (moving average), then enforce monotonic source time (slope >= 0).
  std::vector<double> sm(nT);
  for (int t = 0; t < nT; ++t) {
    double s = 0; int c = 0;
    for (int k = -o.smooth_radius; k <= o.smooth_radius; ++k)
      if (t + k >= 0 && t + k < nT) { s += off[t + k]; ++c; }
    sm[t] = s / c;
  }
  std::vector<double> ts(nT);
  for (int t = 0; t < nT; ++t) ts[t] = t + sm[t];
  for (int t = 1; t < nT; ++t) ts[t] = std::max(ts[t], ts[t - 1]);
  return ts;
}

}  // namespace moco

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "align_shot.inc"
