// Included at the end of align_core.cpp. See align_shot.h for the model.
#include "align_shot.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace moco {

// Runs fn(0..n-1) on worker threads; the calling thread only polls progress (so it may talk to a host API).
// Returns false when progress() asked to cancel (workers stop after their current item).
static bool parallel_for(int n, int threads, const std::function<void(int)>& fn,
                         const std::function<bool(int, int)>& progress, int base, int total) {
  std::atomic<int> next{0}, done{0};
  std::atomic<bool> stop{false};
  auto work = [&]() { for (int i; !stop && (i = next++) < n;) { fn(i); ++done; } };
  if (threads <= 1 && !progress) { work(); return true; }
  std::vector<std::thread> pool;
  for (int k = 0; k < std::max(1, threads); ++k) pool.emplace_back(work);
  while (done < n && !stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (progress && !progress(base + done, total)) stop = true;
  }
  for (std::thread& t : pool) t.join();
  return !stop;
}

H3 to_normalized(const H3& Hpx, int w, int h) { H3 N = norm_of_level(w, h, 0); return N * Hpx * N.inverse(); }
H3 from_normalized(const H3& Hn, int w, int h) { H3 N = norm_of_level(w, h, 0); return N.inverse() * Hn * N; }

static H3 normed(H3 h) { double s = h.m[8]; for (double& v : h.m) v /= s; return h; }
// Fractional camera motion: M^tau ~ I + tau (M - I) (small inter-frame motion; tau may be negative or > 1).
static H3 frac(const H3& M, double tau) {
  H3 r;
  for (int i = 0; i < 9; ++i) r.m[i] = (i % 4 == 0 ? 1.0 : 0.0) + tau * (M.m[i] - (i % 4 == 0 ? 1.0 : 0.0));
  return r;
}
// Mean squared distance (px^2) between two mappings over a grid of the frame.
static double map_err(const H3& A, const H3& B, int w, int h) {
  double se = 0; int n = 0;
  for (int j = 0; j < 5; ++j)
    for (int i = 0; i < 8; ++i) {
      double x = (i + 0.5) * w / 8, y = (j + 0.5) * h / 5, ax, ay, bx, by;
      A.apply(x, y, ax, ay); B.apply(x, y, bx, by);
      se += (ax - bx) * (ax - bx) + (ay - by) * (ay - by); ++n;
    }
  return se / n;
}

// Mask (1 = ignore) of the zones where the aligned source still disagrees with the target, merged with `manual`.
static Image auto_mask(const Image& T, const Image& S, const H3& H, const Image* manual, const ShotOptions& o) {
  Image tn = downsample2(downsample2(normalize_local(T, o.space.norm_radius)));
  Image valid;
  Image sn = downsample2(downsample2(normalize_local(warp(S, H, &valid), o.space.norm_radius)));
  valid = downsample2(downsample2(valid));
  Image e(tn.w, tn.h);
  for (size_t i = 0; i < e.p.size(); ++i) e.p[i] = valid.p[i] > 0.99f ? std::fabs(tn.p[i] - sn.p[i]) : 0.f;
  e = box_blur(e, 3);
  std::vector<float> v(e.p);
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  const float thr = float(o.auto_thresh) * std::max(v[v.size() / 2], 1e-3f);
  Image m(T.w, T.h, 0.f);
  for (int y = 0; y < T.h; ++y)
    for (int x = 0; x < T.w; ++x) {
      bool ig = e.at(std::min(x / 4, e.w - 1), std::min(y / 4, e.h - 1)) > thr;
      if (manual && manual->at(x, y) > 0.5f) ig = true;
      m.at(x, y) = ig ? 1.f : 0.f;
    }
  return m;
}

// ---------------------------------------------------------------- parallax residual field
// Bilinear node lookup; g holds (dx, dy) per node in units of `unit` pixels.
static void grid_px(const float* g, int gw, int gh, int w, int h, double x, double y, double unit, double& dx, double& dy) {
  double u = (x + 0.5) / w * gw - 0.5, v = (y + 0.5) / h * gh - 0.5;
  u = std::min(std::max(u, 0.0), double(gw - 1)); v = std::min(std::max(v, 0.0), double(gh - 1));
  int i = std::min(int(u), gw - 2), j = std::min(int(v), gh - 2);
  if (i < 0) i = 0; if (j < 0) j = 0;
  const int i1 = std::min(i + 1, gw - 1), j1 = std::min(j + 1, gh - 1);
  const double fu = u - i, fv = v - j;
  const float *a = g + (j * gw + i) * 2, *b = g + (j * gw + i1) * 2, *c = g + (j1 * gw + i) * 2, *d = g + (j1 * gw + i1) * 2;
  dx = unit * ((a[0] * (1 - fu) + b[0] * fu) * (1 - fv) + (c[0] * (1 - fu) + d[0] * fu) * fv);
  dy = unit * ((a[1] * (1 - fu) + b[1] * fu) * (1 - fv) + (c[1] * (1 - fu) + d[1] * fu) * fv);
}
void grid_at(const std::vector<float>& g, int gw, int gh, int w, int h, double x, double y, double& dx, double& dy) {
  if (gw < 1 || gh < 1 || int(g.size()) < gw * gh * 2) { dx = dy = 0; return; }
  grid_px(g.data(), gw, gh, w, h, x, y, w * 0.5, dx, dy);
}

// Residual displacement of S against T after the homography H, on a gw x gh grid, in PIXELS.
// Coarse-to-fine Lucas-Kanade per node on lighting-normalised images, robust weights, nothing measured where
// mask > 0.5; each node's confidence is the smaller eigenvalue of its structure tensor, and a confidence-weighted
// smoothing spreads the field into weak and ignored nodes from their neighbours.
static std::vector<float> measure_grid(const Image& T, const Image& S, const H3& H, const Image* mask, const ShotOptions& o,
                                       const Image* smask = nullptr) {
  const int gw = o.grid_w, gh = o.grid_h, N = gw * gh;
  std::vector<float> d(size_t(N) * 2, 0.f);
  std::vector<Image> tp{normalize_local(T, o.space.norm_radius)}, sp{normalize_local(S, o.space.norm_radius)};
  for (int L = 1; L < 3; ++L) { tp.push_back(downsample2(tp.back())); sp.push_back(downsample2(sp.back())); }
  const int w = T.w, h = T.h;
  for (int L = 2; L >= 0; --L) {
    const Image& t = tp[L]; const Image& s = sp[L];
    const double f = double(1 << L);
    for (int it = 0; it < 3; ++it) {
      std::vector<double> A(size_t(N) * 3, 0.0), b(size_t(N) * 2, 0.0);
      for (int y = 1; y < t.h - 1; ++y)
        for (int x = 1; x < t.w - 1; ++x) {
          const double X = (x + 0.5) * f - 0.5, Y = (y + 0.5) * f - 0.5;        // full-resolution target pixel
          if (mask && mask->at(std::min(int(X + 0.5), w - 1), std::min(int(Y + 0.5), h - 1)) > 0.5f) continue;
          double dx, dy; grid_px(d.data(), gw, gh, w, h, X, Y, 1.0, dx, dy);
          double sx, sy; H.apply(X, Y, sx, sy);
          const double qx = (sx + dx + 0.5) / f - 0.5, qy = (sy + dy + 0.5) / f - 0.5;
          if (!(qx >= 1 && qy >= 1 && qx <= s.w - 2 && qy <= s.h - 2)) continue;
          if (smask && smask->at(std::min(std::max(int(sx + dx + 0.5), 0), smask->w - 1), std::min(std::max(int(sy + dy + 0.5), 0), smask->h - 1)) > 0.5f) continue;
          const float e = s.bilinear(float(qx), float(qy)) - t.at(x, y);
          const double gx = 0.5 * (s.bilinear(float(qx + 1), float(qy)) - s.bilinear(float(qx - 1), float(qy))) / f;
          const double gy = 0.5 * (s.bilinear(float(qx), float(qy + 1)) - s.bilinear(float(qx), float(qy - 1))) / f;
          const double ae = std::fabs(e), wr = ae <= 1.0 ? 1.0 : 1.0 / ae;      // Huber on unit-variance images
          double u = (X + 0.5) / w * gw - 0.5, v = (Y + 0.5) / h * gh - 0.5;
          u = std::min(std::max(u, 0.0), double(gw - 1)); v = std::min(std::max(v, 0.0), double(gh - 1));
          const int i0 = std::min(int(u), gw - 1), j0 = std::min(int(v), gh - 1);
          const double fu = u - i0, fv = v - j0;
          for (int jj = 0; jj < 2; ++jj)
            for (int ii = 0; ii < 2; ++ii) {
              const int i = i0 + ii, j = j0 + jj;
              if (i >= gw || j >= gh) continue;
              const double wn = wr * (ii ? fu : 1 - fu) * (jj ? fv : 1 - fv);
              if (wn <= 0) continue;
              double* a = &A[size_t(j * gw + i) * 3]; double* bb = &b[size_t(j * gw + i) * 2];
              a[0] += wn * gx * gx; a[1] += wn * gx * gy; a[2] += wn * gy * gy;
              bb[0] += wn * gx * e; bb[1] += wn * gy * e;
            }
        }
      // per-node solve + confidence
      std::vector<double> conf(N, 0.0), meas(size_t(N) * 2, 0.0);
      double cmax = 0;
      for (int k = 0; k < N; ++k) {
        const double a = A[k * 3], bq = A[k * 3 + 1], c = A[k * 3 + 2];
        const double tr = a + c, det = a * c - bq * bq;
        const double lmin = 0.5 * (tr - std::sqrt(std::max(tr * tr - 4 * det, 0.0)));
        conf[k] = std::max(lmin, 0.0); cmax = std::max(cmax, conf[k]);
      }
      for (int k = 0; k < N; ++k) {
        const double a = A[k * 3] + 1e-9, bq = A[k * 3 + 1], c = A[k * 3 + 2] + 1e-9, det = a * c - bq * bq;
        double ux = 0, uy = 0;
        conf[k] = cmax > 0 ? conf[k] / cmax : 0.0;
        if (conf[k] > 0.02 && det > 1e-12) {
          ux = -(c * b[k * 2] - bq * b[k * 2 + 1]) / det; uy = -(a * b[k * 2 + 1] - bq * b[k * 2]) / det;
          const double lim = 1.5 * f, n2 = std::hypot(ux, uy);
          if (n2 > lim) { ux *= lim / n2; uy *= lim / n2; }
        } else conf[k] = 0;
        meas[k * 2] = d[k * 2] + ux; meas[k * 2 + 1] = d[k * 2 + 1] + uy;
      }
      // confidence-weighted smoothing: each node between its own measure and the mean of its neighbours
      std::vector<double> cur(meas), nxt(meas);
      for (int rep = 0; rep < 30; ++rep) {
        for (int j = 0; j < gh; ++j)
          for (int i = 0; i < gw; ++i) {
            double sxn = 0, syn = 0; int nb = 0;
            const int di[4] = {-1, 1, 0, 0}, dj[4] = {0, 0, -1, 1};
            for (int q = 0; q < 4; ++q) {
              const int ii = i + di[q], jj = j + dj[q];
              if (ii < 0 || jj < 0 || ii >= gw || jj >= gh) continue;
              sxn += cur[(jj * gw + ii) * 2]; syn += cur[(jj * gw + ii) * 2 + 1]; ++nb;
            }
            const int k = j * gw + i; const double c = conf[k], lam = o.grid_smooth;
            nxt[k * 2] = (c * meas[k * 2] + lam * sxn / std::max(nb, 1)) / (c + lam);
            nxt[k * 2 + 1] = (c * meas[k * 2 + 1] + lam * syn / std::max(nb, 1)) / (c + lam);
          }
        cur.swap(nxt);
      }
      for (size_t k = 0; k < d.size(); ++k) d[k] = float(cur[k]);
    }
  }
  return d;
}

// ---------------------------------------------------------------- tension pass (sprint 18, an idea of Jean)
// After the field is solved, find the cells where the deformation is under tension and fix only those.
// Tension of a cell (set pixels only, masks excluded), each against a robust threshold median + k * MAD over the
// cells of the frame:  (a) residual = the matching error left after the warp,  (b) strain = how far the
// displacement of the cell is from the mean of its neighbours,  (c) temporal jump = the cell against frames -1 / +1.
// Fix for a flagged cell: a wider local search of its displacement (whole steps, then half steps) on the residual
// of the solver, kept only if it lowers that residual clearly; then cohesion with its neighbours that does NOT cross
// strong edges of the target (neighbour weight falls with the difference of the mean target luma of the cells);
// cells flagged for a temporal jump are pulled towards the neighbouring frames. Uses the solver residual only.
static void tension_fix(const Image& T, const Image& S, const H3& H, const Image* mask, const Image* smask,
                        const std::vector<float>& d, const std::vector<float>* dprev, const std::vector<float>* dnext,
                        const ShotOptions& o, std::vector<float>& out, std::vector<float>* score, int* n_flag, int* n_fixed) {
  const int gw = o.grid_w, gh = o.grid_h, N = gw * gh, w = T.w, h = T.h;
  out = d;
  if (int(d.size()) < N * 2) return;
  const Image tn = downsample2(normalize_local(T, o.space.norm_radius)), sn = downsample2(normalize_local(S, o.space.norm_radius));
  const double cw = double(w) / gw, ch = double(h) / gh;
  // matching error of the window of node (i, j) when the displacement of that window is shifted by (ex, ey)
  auto cellres = [&](const std::vector<float>& fld, int i, int j, double ex, double ey, int* cnt) {
    const double cx = (i + 0.5) * cw - 0.5, cy = (j + 0.5) * ch - 0.5;
    const int x0 = std::max(1, int((cx - 0.75 * cw) / 2)), x1 = std::min(tn.w - 2, int((cx + 0.75 * cw) / 2));
    const int y0 = std::max(1, int((cy - 0.75 * ch) / 2)), y1 = std::min(tn.h - 2, int((cy + 0.75 * ch) / 2));
    double sum = 0; int c = 0;
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double X = 2 * x + 0.5, Y = 2 * y + 0.5;
        if (mask && mask->at(std::min(int(X + 0.5), w - 1), std::min(int(Y + 0.5), h - 1)) > 0.5f) continue;
        double dx, dy; grid_px(fld.data(), gw, gh, w, h, X, Y, 1.0, dx, dy);
        double sx, sy; H.apply(X, Y, sx, sy);
        sx += dx + ex; sy += dy + ey;
        if (!(sx >= 2 && sy >= 2 && sx <= w - 3 && sy <= h - 3)) continue;
        if (smask && smask->at(int(sx + 0.5), int(sy + 0.5)) > 0.5f) continue;
        const float e = std::fabs(sn.bilinear(float((sx - 0.5) / 2), float((sy - 0.5) / 2)) - tn.at(x, y));
        sum += std::min(e, 2.f); ++c;
      }
    if (cnt) *cnt = c;
    return c ? sum / c : 0.0;
  };
  std::vector<double> res(N, 0), strain(N, 0), tj(N, 0); std::vector<char> valid(N, 0);
  std::vector<float> lum(N, 0.f);
  const bool have_t = dprev && dnext && int(dprev->size()) >= N * 2 && int(dnext->size()) >= N * 2;
  for (int j = 0; j < gh; ++j)
    for (int i = 0; i < gw; ++i) {
      const int k = j * gw + i; int c = 0;
      res[k] = cellres(d, i, j, 0, 0, &c); valid[k] = c >= 12;
      double mx = 0, my = 0; int nb = 0;
      const int di[4] = {-1, 1, 0, 0}, dj[4] = {0, 0, -1, 1};
      for (int q = 0; q < 4; ++q) { const int ii = i + di[q], jj = j + dj[q]; if (ii < 0 || jj < 0 || ii >= gw || jj >= gh) continue; mx += d[(jj * gw + ii) * 2]; my += d[(jj * gw + ii) * 2 + 1]; ++nb; }
      strain[k] = nb ? std::hypot(d[k * 2] - mx / nb, d[k * 2 + 1] - my / nb) : 0;
      if (have_t)
        tj[k] = std::hypot(d[k * 2] - 0.5 * ((*dprev)[k * 2] + (*dnext)[k * 2]), d[k * 2 + 1] - 0.5 * ((*dprev)[k * 2 + 1] + (*dnext)[k * 2 + 1]));
      lum[k] = T.at(std::min(std::max(int((i + 0.5) * cw), 0), w - 1), std::min(std::max(int((j + 0.5) * ch), 0), h - 1));
    }
  auto thresh = [&](const std::vector<double>& v, double floor_) {
    std::vector<double> a; for (int k = 0; k < N; ++k) if (valid[k]) a.push_back(v[k]);
    if (a.size() < 8) return 1e30;
    std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end()); const double med = a[a.size() / 2];
    for (double& x : a) x = std::fabs(x - med);
    std::nth_element(a.begin(), a.begin() + a.size() / 2, a.end());
    return std::max(floor_, med + o.tension_k * 1.4826 * a[a.size() / 2]);
  };
  const double tr = thresh(res, 0.05), ts = thresh(strain, 0.5), tt = thresh(tj, 0.5);
  std::vector<char> flag(N, 0);
  int nf = 0, nx = 0;
  for (int k = 0; k < N; ++k) {
    const double sc = std::max(res[k] / tr, std::max(strain[k] / ts, tj[k] / tt));
    if (score) (*score)[k] = valid[k] ? float(sc) : -1.f;
    if (valid[k] && sc > 1.0) { flag[k] = 1; ++nf; }
  }
  // local fix of the flagged cells
  const int Rr = std::max(1, o.tension_radius);
  for (int j = 0; j < gh; ++j)
    for (int i = 0; i < gw; ++i) {
      const int k = j * gw + i; if (!flag[k]) continue;
      double best = res[k], bx = 0, by = 0;
      for (int ey = -Rr; ey <= Rr; ++ey)
        for (int ex = -Rr; ex <= Rr; ++ex) {
          if (!ex && !ey) continue;
          int c = 0; const double r = cellres(d, i, j, ex, ey, &c);
          if (c >= 12 && r < best) { best = r; bx = ex; by = ey; }
        }
      const double cx0 = bx, cy0 = by;
      for (int ey = -1; ey <= 1; ++ey)
        for (int ex = -1; ex <= 1; ++ex) {
          if (!ex && !ey) continue;
          int c = 0; const double r = cellres(d, i, j, cx0 + 0.5 * ex, cy0 + 0.5 * ey, &c);
          if (c >= 12 && r < best) { best = r; bx = cx0 + 0.5 * ex; by = cy0 + 0.5 * ey; }
        }
      if (best < 0.9 * res[k]) { out[k * 2] = float(d[k * 2] + bx); out[k * 2 + 1] = float(d[k * 2 + 1] + by); ++nx; }
    }
  // cohesion of the flagged cells with their neighbours, not across strong edges of the target
  for (int rep = 0; rep < 2; ++rep) {
    std::vector<float> nxt(out);
    for (int j = 0; j < gh; ++j)
      for (int i = 0; i < gw; ++i) {
        const int k = j * gw + i; if (!flag[k]) continue;
        double sx = 2.0 * out[k * 2], sy = 2.0 * out[k * 2 + 1], sw = 2.0;
        const int di[4] = {-1, 1, 0, 0}, dj[4] = {0, 0, -1, 1};
        for (int q = 0; q < 4; ++q) {
          const int ii = i + di[q], jj = j + dj[q]; if (ii < 0 || jj < 0 || ii >= gw || jj >= gh) continue;
          const int kk = jj * gw + ii;
          const double wq = std::exp(-std::fabs(double(lum[k]) - lum[kk]) / o.tension_edge);
          sx += wq * out[kk * 2]; sy += wq * out[kk * 2 + 1]; sw += wq;
        }
        nxt[k * 2] = float(sx / sw); nxt[k * 2 + 1] = float(sy / sw);
      }
    out.swap(nxt);
  }
  // temporal consistency for the cells that jumped
  if (have_t)
    for (int k = 0; k < N; ++k)
      if (flag[k] && tj[k] > tt) {
        out[k * 2] = 0.5f * out[k * 2] + 0.25f * ((*dprev)[k * 2] + (*dnext)[k * 2]);
        out[k * 2 + 1] = 0.5f * out[k * 2 + 1] + 0.25f * ((*dprev)[k * 2 + 1] + (*dnext)[k * 2 + 1]);
      }
  if (n_flag) *n_flag = nf;
  if (n_fixed) *n_fixed = nx;
}

// 0.3.5: phase timings to stderr when the environment variable W4D_TIMING is set (speed work).
#define W4D_PHASE(name) do { if (w4d_timing) { auto now_ = std::chrono::steady_clock::now(); \
  std::fprintf(stderr, "  phase %-18s %6.2f s\n", name, std::chrono::duration<double>(now_ - w4d_t0).count()); w4d_t0 = now_; } } while (0)
ShotResult align_shot(int nT, int nS, const std::function<const Image&(int)>& getT,
                      const std::function<const Image&(int)>& getS, const ShotOptions& o) {
  ShotResult R;
  const int w = getT(0).w, h = getT(0).h;
  const bool w4d_timing = std::getenv("W4D_TIMING") != nullptr;
  auto w4d_t0 = std::chrono::steady_clock::now();
  // 0) coarse integer offset: SSD of small lighting-normalised frames
  {
    const int Lc = 3;
    auto small = [&](const Image& im) { Image a = normalize_local(im, o.space.norm_radius); for (int L = 0; L < Lc; ++L) a = downsample2(a); return a; };
    std::vector<Image> ts(nT), ss(nS), ms(nT);
    if (!parallel_for(nT + nS, o.threads, [&](int i) {
          if (i < nT) {
            ts[i] = small(getT(i));
            if (const Image* mk = o.ignore ? o.ignore(i) : nullptr) { Image a = *mk; for (int L = 0; L < Lc; ++L) a = downsample2(a); ms[i] = a; }
          } else ss[i - nT] = small(getS(i - nT));
        }, o.progress, 0, 2 * nT + nS)) { R.cancelled = true; return R; }
    double bestc = 1e300;
    for (int dd = -o.range; dd <= o.range; ++dd) {
      double sum = 0; size_t cnt = 0;
      for (int t = 0; t < nT; ++t) {
        int s = t + dd; if (s < 0 || s >= nS) continue;
        const bool hm = ms[t].p.size() == ts[t].p.size();
        for (size_t i = 0; i < ts[t].p.size(); ++i) {
          if (hm && ms[t].p[i] > 0.25f) continue;   // ignored zones do not vote on the time offset either
          double e = ts[t].p[i] - ss[s].p[i]; sum += e * e; ++cnt;
        }
      }
      if (cnt && sum / cnt < bestc) { bestc = sum / cnt; R.coarse_offset = dd; }
    }
  }
  const int d0 = R.coarse_offset;
  W4D_PHASE("0 coarse");
  // 0b) Parallax-sensitive time score (for moves with depth, e.g. a dolly, and for offsets that drift by frames
  // over the shot): for every candidate source frame, fit a homography at reduced size and keep the residual it
  // leaves. At the right time one homography explains all depths; at a wrong time the near structure cannot
  // follow the far wall and the residual rises. It gives each frame its own whole-frame base offset d0v[t], so
  // the motion model below only has to bridge a fraction of a frame; for a flat pan the score is flat and the
  // base stays at the shot's coarse offset.
  std::vector<int> d0v(nT, d0);
  std::vector<std::vector<double>> cres;
  std::vector<std::vector<H3>> cresH;   // the homography of each candidate fit, normalised (starts for the full fits)
  if (o.time_parallax) {
    const int Rg = o.range, nc = 2 * Rg + 1;
    int q = 0; while ((w >> q) > 320) ++q;                      // work at about 240-320 px wide
    auto reduce = [&](const Image& im) { Image a = im; for (int L = 0; L < q; ++L) a = downsample2(a); return a; };
    std::vector<Image> tq(nT), sq(nS), mq(nT), mqs(nS);
    parallel_for(nT + nS, o.threads, [&](int i) {
      if (i < nT) { tq[i] = reduce(getT(i)); if (const Image* mk = o.ignore ? o.ignore(i) : nullptr) mq[i] = reduce(*mk); }
      else { sq[i - nT] = reduce(getS(i - nT)); if (const Image* mk = o.ignore_source ? o.ignore_source(i - nT) : nullptr) mqs[i - nT] = reduce(*mk); }
    }, nullptr, 0, 1);
    cres.assign(nT, std::vector<double>(nc, -1.0));
    // Robust start (sprint 9): the takes can differ by a large zoom and shift (16 % and 60 px at 1080p on the real
    // shot), far outside what a fit started at identity can reach. So per frame: a coarse search over scale and
    // translation at thumbnail size for one candidate, a refinement at reduced size, then the other candidate
    // source frames are chained outwards from it, each started from its neighbour's result.
    cresH.assign(nT, std::vector<H3>(nc));
    std::vector<Image> tt(nT), st(nS);
    parallel_for(nT + nS, o.threads, [&](int i) {
      if (i < nT) tt[i] = downsample2(downsample2(tq[i])); else st[i - nT] = downsample2(downsample2(sq[i - nT]));
    }, nullptr, 0, 1);
    const int cstride = std::max(1, o.cand_stride);   // 0.3.5 speed option: candidates on every cstride-th frame
    if (!parallel_for(nT, o.threads, [&](int t) {
          if (cstride > 1 && t % cstride != 0 && t != nT - 1) return;   // filled from the neighbours below
          SpaceOptions so = o.space; so.levels = 3; so.iters = 12; so.norm_radius = std::max(3, o.space.norm_radius >> (q > 1 ? 1 : 0));
          so.ignore_mask = mq[t].w ? &mq[t] : nullptr;
          const int wq = tq[t].w, hq = tq[t].h, wt = tt[t].w, ht = tt[t].h;
          int o0 = std::min(std::max(d0, -Rg), Rg);
          while (o0 < Rg && t + o0 < 0) ++o0;
          while (o0 > -Rg && t + o0 >= nS) --o0;
          if (t + o0 < 0 || t + o0 >= nS) return;
          H3 start;   // normalised
          if (o.wide_search) {
            SpaceOptions ss = o.space; ss.levels = 2; ss.iters = 8; ss.dof = 6; ss.norm_radius = 3;
            Image mt, mts;   // thumbnail masks
            if (mq[t].w) { mt = downsample2(downsample2(mq[t])); ss.ignore_mask = &mt; }
            if (mqs[t + o0].w) { mts = downsample2(downsample2(mqs[t + o0])); ss.source_ignore = &mts; }
            double bestr = 1e300;
            for (int is = -3; is <= 3; ++is)
              for (int ix = -1; ix <= 1; ++ix)
                for (int iy = -1; iy <= 1; ++iy) {
                  const double sc = 1.0 + 0.05 * is, cxs = (wt - 1) * 0.5, cys = (ht - 1) * 0.5;
                  H3 init; init.m[0] = sc; init.m[4] = sc;
                  init.m[2] = cxs - sc * cxs + 0.1 * ix * wt; init.m[5] = cys - sc * cys + 0.1 * iy * wt;
                  SpaceResult r = align_space(tt[t], st[t + o0], init, ss);
                  if (r.residual > 0 && r.inlier_frac > 0.5 && r.residual < bestr && std::fabs(r.H.m[0] * r.H.m[4] - r.H.m[1] * r.H.m[3]) > 0.4) { bestr = r.residual; start = to_normalized(r.H, wt, ht); }
                }
          }
          auto fit = [&](int oo, const H3& init_n) {
            SpaceOptions s1 = so; s1.source_ignore = mqs[t + oo].w ? &mqs[t + oo] : nullptr;
            SpaceResult r = align_space(tq[t], sq[t + oo], from_normalized(init_n, wq, hq), s1);
            cres[t][oo + Rg] = r.residual; cresH[t][oo + Rg] = to_normalized(r.H, wq, hq);
            return cresH[t][oo + Rg];
          };
          const H3 h0 = fit(o0, start);
          H3 prev = h0;
          for (int oo = o0 + 1; oo <= Rg && t + oo < nS; ++oo) prev = fit(oo, prev);
          prev = h0;
          for (int oo = o0 - 1; oo >= -Rg && t + oo >= 0; --oo) prev = fit(oo, prev);
        }, o.progress, 0, 2 * nT + nS)) { R.cancelled = true; return R; }
    if (cstride > 1)   // frames between measured ones: scores interpolated linearly, fit starts from the frame before
      for (int t = 0; t < nT; ++t) {
        if (t % cstride == 0 || t == nT - 1) continue;
        const int a = t - t % cstride, b = std::min(a + cstride, nT - 1);
        const double fb = double(t - a) / double(b - a);
        for (int i = 0; i < nc; ++i) {
          const double ca = cres[a][i], cb = cres[b][i];
          cres[t][i] = (ca >= 0 && cb >= 0) ? (1 - fb) * ca + fb * cb : (ca >= 0 ? ca : cb);
          cresH[t][i] = cres[a][i] >= 0 ? cresH[a][i] : cresH[b][i];
        }
        // a candidate past the source end for this frame does not exist, whatever the neighbours had
        for (int i = 0; i < nc; ++i) { const int sidx = t + i - Rg; if (sidx < 0 || sidx >= nS) cres[t][i] = -1.0; }
      }
    if (cstride > 1) {   // ...but the candidates near the neighbours' best are really fitted (5 fits instead of 17)
      std::vector<int> todo;
      for (int t = 0; t < nT; ++t) if (t % cstride != 0 && t != nT - 1) todo.push_back(t);
      parallel_for(int(todo.size()), o.threads, [&](int qi) {
        const int t = todo[qi];
        int ib = -1; for (int i = 0; i < nc; ++i) if (cres[t][i] >= 0 && (ib < 0 || cres[t][i] < cres[t][ib])) ib = i;
        if (ib < 0) return;
        SpaceOptions so = o.space; so.levels = 3; so.iters = 12; so.norm_radius = std::max(3, o.space.norm_radius >> (q > 1 ? 1 : 0));
        so.ignore_mask = mq[t].w ? &mq[t] : nullptr;
        const int wq = tq[t].w, hq = tq[t].h;
        H3 prev = cresH[t][ib];
        for (int d = 0; d <= 2; ++d)
          for (int sg : {1, -1}) {
            if (d == 0 && sg < 0) continue;
            const int i = ib + sg * d, oo = i - Rg;
            if (i < 0 || i >= nc || t + oo < 0 || t + oo >= nS) continue;
            SpaceOptions s1 = so; s1.source_ignore = mqs[t + oo].w ? &mqs[t + oo] : nullptr;
            SpaceResult r = align_space(tq[t], sq[t + oo], from_normalized(d == 0 ? prev : cresH[t][i], wq, hq), s1);
            cres[t][i] = r.residual; cresH[t][i] = to_normalized(r.H, wq, hq);
          }
      }, nullptr, 0, 1);
    }
    // base offset per frame: a path through the residual scores that may change by at most one frame per frame
    // and pays for every change (two motion-control passes drift slowly; single frames must not jump)
    {
      const double big = 1e3;
      std::vector<double> acc(size_t(nT) * nc, 0.0); std::vector<int> from(size_t(nT) * nc, 0);
      auto cn = [&](int t, int i) {   // relative rise over the frame's best candidate, flat inside the margin
        // 0.3.3: the last `range` frames see candidates past the end of the source take. Those do not exist, and
        // scoring them "big" forced the path down at the end (run 5: offset -1 at the last frame, the path moved
        // for 25 frames before it, and the fits there diverged). No fair evidence there: no measurement, the
        // path carries on (held), the same with or without the take's last frames.
        if (t + o.range >= nS) return 0.0;
        double cmin = 1e300; for (double c : cres[t]) if (c >= 0) cmin = std::min(cmin, c);
        if (!(cmin < 1e299) || cmin <= 0) return 0.0;        // no measurement: free
        if (cres[t][i] < 0) return big;
        return std::max(0.0, cres[t][i] / cmin - 1.0 - o.parallax_margin);
      };
      std::vector<double> c0(size_t(nT) * nc);
      for (int t = 0; t < nT; ++t) for (int i = 0; i < nc; ++i) c0[size_t(t) * nc + i] = cn(t, i);
      for (int i = 0; i < nc; ++i) acc[i] = c0[i];
      for (int t = 1; t < nT; ++t)
        for (int i = 0; i < nc; ++i) {
          double bestv = 1e300; int bi = i;
          for (int k = std::max(0, i - 1); k <= std::min(nc - 1, i + 1); ++k) {
            const double v = acc[size_t(t - 1) * nc + k] + (k != i ? o.parallax_jump_cost : 0.0);
            if (v < bestv) { bestv = v; bi = k; }
          }
          acc[size_t(t) * nc + i] = bestv + c0[size_t(t) * nc + i]; from[size_t(t) * nc + i] = bi;
        }
      int i = int(std::min_element(acc.begin() + size_t(nT - 1) * nc, acc.end()) - (acc.begin() + size_t(nT - 1) * nc));
      for (int t = nT - 1; t >= 0; --t) { d0v[t] = i - Rg; if (t > 0) i = from[size_t(t) * nc + i]; }
      // a flat score (pan of a flat scene) says nothing: keep the shot's coarse offset
      double spread = 0; for (double v : c0) if (v < big) spread = std::max(spread, v);
      if (spread <= 0) for (int& v : d0v) v = d0;
    }
  }
  auto ex = [&](int k, double tj) { return tj + (d0v[k] - d0); };   // exponent of M_k for frame k's own base offset
  W4D_PHASE("0b candidates");
  // 1) measurements: P_t (target t -> source t+d0v[t]) and M_t (target t -> target t+1)
  std::vector<H3> P(nT), M(nT);
  std::vector<char> ok(nT, 0);
  // (each frame is independent: identity start, so the result does not depend on the thread count)
  if (!parallel_for(nT, o.threads, [&](int t) {
        int s = t + d0v[t];
        const Image* mk = o.ignore ? o.ignore(t) : nullptr;
        SpaceOptions so = o.space; so.ignore_mask = mk;
        if (mk) {   // with much of the frame masked, perspective is not constrained by what is left: fit affine
          size_t ig = 0; for (float v : mk->p) ig += v > 0.5f;
          if (ig * 10 > mk->p.size() * 3) so.dof = std::min(so.dof, 6);
        }
        const int dof_used = so.dof;
        if (s >= 0 && s < nS) {
          H3 hstart = H3::identity();
          if (!cresH.empty() && d0v[t] + o.range >= 0 && d0v[t] + o.range < int(cresH[t].size()) && cres[t][d0v[t] + o.range] >= 0)
            hstart = from_normalized(cresH[t][d0v[t] + o.range], w, h);
          so.source_ignore = o.ignore_source ? o.ignore_source(s) : nullptr;
          H3 Hp = align_space(getT(t), getS(s), hstart, so).H;
          so.source_ignore = nullptr;
          if (o.auto_ignore) {   // second pass without the zones that still disagree
            Image am = auto_mask(getT(t), getS(s), Hp, mk, o);
            SpaceOptions s2 = o.space; s2.ignore_mask = &am; s2.dof = dof_used; s2.levels = std::min(s2.levels, 3); s2.iters = 10;
            Hp = align_space(getT(t), getS(s), Hp, s2).H;
          }
          P[t] = normed(Hp); ok[t] = 1;
        }
        if (t + 1 < nT) {
          // camera motion of the target: ignore what is masked in either frame
          const Image* mk1 = o.ignore ? o.ignore(t + 1) : nullptr;
          Image un;
          if (mk && mk1) { un = *mk; for (size_t i = 0; i < un.p.size(); ++i) un.p[i] = std::max(un.p[i], mk1->p[i]); so.ignore_mask = &un; }
          else so.ignore_mask = mk ? mk : mk1;
          H3 Hm = align_space(getT(t), getT(t + 1), H3::identity(), so).H;
          if (o.auto_ignore) {   // things moving on their own in the target do not follow the camera: mask and refit
            Image am = auto_mask(getT(t), getT(t + 1), Hm, so.ignore_mask, o);
            SpaceOptions s2 = o.space; s2.ignore_mask = &am; s2.dof = dof_used; s2.levels = std::min(s2.levels, 3); s2.iters = 10;
            Hm = align_space(getT(t), getT(t + 1), Hm, s2).H;
          }
          M[t] = normed(Hm);
        }
      }, o.progress, nT + nS, 2 * nT + nS)) { R.cancelled = true; return R; }
  if (nT > 1) M[nT - 1] = M[nT - 2];
  W4D_PHASE("1 P/M fits");
  // 2) one tau and one H for the shot: scan tau, H = mean of P_t * M_t^-tau, keep the best fit
  auto shotH = [&](double tau, double* err) {
    H3 a; for (double& v : a.m) v = 0; int c = 0;
    for (int t = 0; t < nT; ++t) if (ok[t]) { H3 q = normed(P[t] * frac(M[t], ex(t, tau)).inverse()); for (int i = 0; i < 9; ++i) a.m[i] += q.m[i]; ++c; }
    for (double& v : a.m) v /= std::max(c, 1);
    if (err) { *err = 0; for (int t = 0; t < nT; ++t) if (ok[t]) *err += map_err(P[t], a * frac(M[t], ex(t, tau)), w, h); }
    return a;
  };
  const double ts = 0.05; const int nt = int(1.5 / ts);
  std::vector<double> ce(2 * nt + 1);
  for (int j = -nt; j <= nt; ++j) shotH(j * ts, &ce[j + nt]);
  int bj = int(std::min_element(ce.begin(), ce.end()) - ce.begin());
  double tau = (bj - nt) * ts;
  if (bj > 0 && bj < 2 * nt) {
    double den = ce[bj - 1] - 2 * ce[bj] + ce[bj + 1];
    if (den > 1e-15) tau += ts * std::clamp(0.5 * (ce[bj - 1] - ce[bj + 1]) / den, -0.5, 0.5);
  }
  R.H = shotH(tau, nullptr);
  // 3) per-frame time curve: DP over offsets with smoothness and a slope limit, data = fit of P_t by H * M_t^tau
  const int J = int(std::round(o.range / o.time_step)), ns = 2 * J + 1;
  const double sig2 = o.time_sigma_px * o.time_sigma_px;
  std::vector<double> cost(size_t(nT) * ns, 0.0);   // frames without a measurement stay flat: DP carries the curve
  // Data term per frame and offset: a LOCAL solve over a window of frames (H and tau assumed constant only inside
  // the window, so both may drift over the shot), plus a mild pull to the shot H.
  const int wr = std::max(1, o.window_radius);
  for (int t = 0; t < nT; ++t)
    if (ok[t]) for (int j = 0; j < ns; ++j) {
      const double tj = d0 - (j - J) * o.time_step;
      H3 a; for (double& v : a.m) v = 0; double ws = 0;
      for (int k = std::max(0, t - wr); k <= std::min(nT - 1, t + wr); ++k) if (ok[k]) {
        double wk = std::exp(-2.0 * (k - t) * (k - t) / double(wr * wr));
        H3 q = normed(P[k] * frac(M[k], ex(k, tj)).inverse());
        for (int i = 0; i < 9; ++i) a.m[i] += wk * q.m[i];
        ws += wk;
      }
      for (double& v : a.m) v /= ws;
      double e = 0;
      for (int k = std::max(0, t - wr); k <= std::min(nT - 1, t + wr); ++k) if (ok[k])
        e += std::exp(-2.0 * (k - t) * (k - t) / double(wr * wr)) * map_err(P[k], a * frac(M[k], ex(k, tj)), w, h);
      e /= ws;
      cost[size_t(t) * ns + j] = o.motion_weight * (e + 0.02 * o.h_prior * map_err(P[t], R.H * frac(M[t], ex(t, tj)), w, h)) / sig2;
    }
  // Add the parallax time score (computed in 0b) to the per-frame cost, flat within parallax_margin of its minimum
  // so that the sub-frame part is left to the motion model.
  if (o.time_parallax) {
    const int Rg = o.range, nc = 2 * Rg + 1;
    for (int t = 0; t < nT; ++t) {
      double cmin = 1e300;
      for (double c : cres[t]) if (c >= 0) cmin = std::min(cmin, c);
      if (!(cmin < 1e299) || cmin <= 0) continue;
      if (t + Rg >= nS) continue;   // 0.3.3: candidates past the end of the take: no parallax evidence (as in 0b)
      auto at = [&](int i) {                                     // nearest valid candidate
        i = std::min(std::max(i, 0), nc - 1);
        for (int d = 0; d < nc; ++d) { if (i - d >= 0 && cres[t][i - d] >= 0) return cres[t][i - d]; if (i + d < nc && cres[t][i + d] >= 0) return cres[t][i + d]; }
        return cmin;
      };
      for (int j = 0; j < ns; ++j) {
        const double offv = (j - J) * o.time_step;
        const int i = int(std::lround(offv)); const double x = offv - i;
        const double c0 = at(i + Rg), cm = at(i + Rg - 1), cp = at(i + Rg + 1);
        const double val = std::max(c0 + 0.5 * (cp - cm) * x + 0.5 * (cp + cm - 2 * c0) * x * x, 0.0);   // local parabola
        cost[size_t(t) * ns + j] += o.parallax_weight * std::max(0.0, (val - cmin) / cmin - o.parallax_margin);
      }
    }
  }
  const int ms = std::max(1, int(std::round(o.max_slope / o.time_step)));
  std::vector<int> st(nT);
  std::vector<double> off(nT);
  for (int tpass = 0; tpass < 2; ++tpass) {
    std::vector<double> acc(size_t(nT) * ns); std::vector<int> from(size_t(nT) * ns, 0);
    for (int j = 0; j < ns; ++j) acc[j] = cost[j];
    for (int t = 1; t < nT; ++t)
      for (int j = 0; j < ns; ++j) {
        double best = 1e300; int bi = j;
        for (int i = std::max(0, j - ms); i <= std::min(ns - 1, j + ms); ++i) {
          double dd = (i - j) * o.time_step, c = acc[size_t(t - 1) * ns + i] + o.time_smooth * dd * dd;
          if (c < best) { best = c; bi = i; }
        }
        acc[size_t(t) * ns + j] = best + cost[size_t(t) * ns + j]; from[size_t(t) * ns + j] = bi;
      }
    st[nT - 1] = int(std::min_element(acc.begin() + size_t(nT - 1) * ns, acc.end()) - (acc.begin() + size_t(nT - 1) * ns));
    for (int t = nT - 1; t > 0; --t) st[t - 1] = from[size_t(t) * ns + st[t]];
    for (int t = 0; t < nT; ++t) {
      int j = st[t]; double sub = 0;
      if (ok[t] && j > 0 && j < ns - 1) {
        double c0 = cost[size_t(t) * ns + j - 1], c1 = cost[size_t(t) * ns + j], c2 = cost[size_t(t) * ns + j + 1];
        double den = c0 - 2 * c1 + c2;
        if (den > 1e-12 && c0 < 1e5 && c2 < 1e5) sub = std::clamp(0.5 * (c0 - c2) / den, -0.5, 0.5);
      }
      off[t] = (j - J + sub) * o.time_step;
    }
    // Time constraint (sprint 12): two motion-control passes do not reverse or jump in time. Frames after
    // time_trust_end (where something, e.g. an arm filling the frame, drags the fit) may only move within
    // +-time_window frames of the offset the trusted part ends with (median of its last 40 frames); then the
    // path is solved again.
    if (tpass == 1 || o.time_trust_end <= 0 || o.time_trust_end >= nT - 1) break;
    std::vector<double> v;
    for (int t = std::max(0, o.time_trust_end - 40); t <= o.time_trust_end; ++t) v.push_back(off[t]);
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    R.time_ref = v[v.size() / 2];
    for (int t = o.time_trust_end + 1; t < nT; ++t)
      for (int j = 0; j < ns; ++j)
        if (std::fabs((j - J) * o.time_step - R.time_ref) > o.time_window) cost[size_t(t) * ns + j] += 1e6;
  }
  R.src_time.resize(nT);
  for (int t = 0; t < nT; ++t) {
    double s = 0; int c = 0;
    for (int k = -2; k <= 2; ++k) if (t + k >= 0 && t + k < nT) { s += off[t + k]; ++c; }
    R.src_time[t] = t + s / c;
  }
  // 4) per-frame H: each frame's own estimate pulled to the shot H, then smoothed over time
  R.Hf.assign(nT, R.H);
  if (o.per_frame_H) {
    std::vector<H3> hf(nT, R.H);
    for (int t = 0; t < nT; ++t)
      if (ok[t]) {
        H3 q = normed(P[t] * frac(M[t], d0v[t] - (R.src_time[t] - t)).inverse());
        for (int i = 0; i < 9; ++i) hf[t].m[i] = (q.m[i] + o.h_prior * R.H.m[i]) / (1 + o.h_prior);
      }
    for (int t = 0; t < nT; ++t) {
      H3 a; for (double& v : a.m) v = 0; int c = 0;
      for (int k = -o.h_smooth_radius; k <= o.h_smooth_radius; ++k)
        if (t + k >= 0 && t + k < nT) { for (int i = 0; i < 9; ++i) a.m[i] += hf[t + k].m[i]; ++c; }
      for (double& v : a.m) v /= c;
      R.Hf[t] = a;
    }
  }
  // 0.3.5 confidence (o.confidence): low-confidence frames take their offset from their confident neighbours.
  R.low_conf.assign(nT, 0);
  if (o.confidence) {
    for (int t = 0; t < nT; ++t) {
      double setshare = 1.0;
      if (const Image* mk = o.ignore ? o.ignore(t) : nullptr) {
        double a = 0; for (float v : mk->p) a += v > 0.5f; setshare = 1.0 - a / std::max<size_t>(1, mk->p.size());
      }
      double sharp = 1e9;
      if (!cres.empty()) {
        const std::vector<double>& c = cres[t];
        int i0 = -1; for (int i = 0; i < int(c.size()); ++i) if (c[i] >= 0 && (i0 < 0 || c[i] < c[i0])) i0 = i;
        if (i0 >= 0 && c[i0] > 0) {
          double m = 1e300; for (int i = 0; i < int(c.size()); ++i) if (c[i] >= 0 && std::abs(i - i0) > 1) m = std::min(m, c[i]);
          if (m < 1e299) sharp = m / c[i0] - 1.0;
        }
      }
      R.low_conf[t] = setshare < o.conf_min_set || sharp < o.conf_min_sharp;
    }
    std::vector<double> off(nT); for (int t = 0; t < nT; ++t) off[t] = R.src_time[t] - t;
    int prev = -1;
    for (int t = 0; t <= nT; ++t) {
      if (t < nT && R.low_conf[t]) continue;
      // run (prev, t) exclusive are low frames
      for (int k = prev + 1; k < t; ++k) {
        double v;
        if (prev >= 0 && t < nT) v = off[prev] + (off[t] - off[prev]) * double(k - prev) / double(t - prev);
        else if (prev >= 0) v = off[prev];
        else if (t < nT) v = off[t];
        else v = off[k];
        R.src_time[k] = k + v;
      }
      prev = t;
    }
    for (char c : R.low_conf) R.interpolated += c;
  }
  // 0.3.2 (after the motion-model H of step 4, which works with unclamped times): where the solved source time runs past the last source frame (the shot needs source frames the take
  // does not have) the offset is HELD at the last in-range value instead of drifting towards the search limit,
  // and the source time is clamped to the source frames. Same at the start.
  std::vector<char> held(nT, 0);
  int held_first = -1, held_last = -1;
  {
    const double hi = double(nS - 1);
    int last = -1, first = -1;
    for (int t = 0; t < nT; ++t) if (R.src_time[t] >= 0.0 && R.src_time[t] <= hi) { if (first < 0) first = t; last = t; }
    if (last >= 0) {
      const double off_last = R.src_time[last] - last, off_first = R.src_time[first] - first;
      for (int t = last + 1; t < nT; ++t) R.src_time[t] = t + off_last;
      for (int t = 0; t < first; ++t) R.src_time[t] = t + off_first;
      for (int t = last + 1; t < nT; ++t) held[t] = 1;
      for (int t = 0; t < first; ++t) held[t] = 1;
      held_first = first; held_last = last;
      R.held_start = first; R.held_end = nT - 1 - last;
    }
    for (int t = 0; t < nT; ++t) R.src_time[t] = std::min(std::max(R.src_time[t], 0.0), hi);
  }
  W4D_PHASE("2-4 time path");
  // 4b) with the parallax time score: fit the homography DIRECTLY to the two source frames around the solved time
  //     (no extrapolation through the motion model, which is only valid for small steps and one plane).
  //     Ha / Hb let the renderer warp each of the two frames with its own homography before blending.
  if (o.time_parallax && nS > 1) {
    R.Ha.assign(nT, R.H); R.Hb.assign(nT, R.H);
    parallel_for(nT, o.threads, [&](int t) {
      const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
      const int a0 = std::min(int(std::floor(s)), nS - 2); const double f = s - a0;
      const Image* mk = o.ignore ? o.ignore(t) : nullptr;
      SpaceOptions so = o.space; so.ignore_mask = mk;
      if (mk) { size_t ig = 0; for (float v : mk->p) ig += v > 0.5f; if (ig * 10 > mk->p.size() * 3) so.dof = std::min(so.dof, 6); }
      if (t >= o.dof_range_lo && t <= o.dof_range_hi) so.dof = std::min(so.dof, o.dof_range_dof);
      H3 hstart = H3::identity();
      { const int i = a0 - t + o.range; if (!cresH.empty() && i >= 0 && i < int(cresH[t].size()) && cres[t][i] >= 0) hstart = from_normalized(cresH[t][i], w, h); }
      so.source_ignore = o.ignore_source ? o.ignore_source(a0) : nullptr;
      // 0.3.5 speed: when this is exactly the fit of step 1 (same target and source frame, same start, masks and dof,
      // no auto-ignore pass), reuse its result: identical inputs give the identical homography.
      const bool same_as_p = ok[t] && !o.auto_ignore && a0 == t + d0v[t] && !(t >= o.dof_range_lo && t <= o.dof_range_hi);
      H3 Pa = same_as_p ? P[t] : normed(align_space(getT(t), getS(a0), hstart, so).H);
      so.source_ignore = o.ignore_source ? o.ignore_source(a0 + 1) : nullptr;
      H3 Pb = normed(align_space(getT(t), getS(a0 + 1), Pa, so).H);
      R.Ha[t] = Pa; R.Hb[t] = Pb;
      for (int i = 0; i < 9; ++i) R.Hf[t].m[i] = (1 - f) * Pa.m[i] + f * Pb.m[i];
    }, nullptr, 0, 1);
    // 4b') soft prior (sprint 12): fit every frame again, started from and pulled towards the temporally smoothed
    //      sequence. Perspective terms hardest (median over +-12 frames inside the trusted part; after it, the
    //      median of the last 40 trusted frames), scale/rotation towards a median over +-4 frames, translation free.
    if (o.prior_persp > 0 || o.prior_lin > 0) {
      const int te = (o.time_trust_end > 0 && o.time_trust_end < nT - 1) ? o.time_trust_end : nT - 1;
      auto medpar = [&](int i, int lo, int hi) {
        std::vector<double> v;
        for (int k = std::max(0, lo); k <= std::min(nT - 1, hi); ++k) v.push_back(R.Hf[k].m[i] / R.Hf[k].m[8]);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
      };
      std::vector<H3> Hs(nT);
      for (int t = 0; t < nT; ++t)
        for (int i = 0; i < 8; ++i) {
          if (i >= 6) Hs[t].m[i] = t <= te ? medpar(i, t - 12, std::min(te, t + 12)) : medpar(i, te - 40, te);
          else Hs[t].m[i] = medpar(i, t - 4, t + 4);
        }
      std::vector<H3> nHa(R.Ha), nHb(R.Hb), nHf(R.Hf);
      parallel_for(nT, o.threads, [&](int t) {
        const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const int a0 = std::min(int(std::floor(s)), nS - 2); const double f = s - a0;
        const Image* mk = o.ignore ? o.ignore(t) : nullptr;
        SpaceOptions so = o.space; so.ignore_mask = mk;
        if (mk) { size_t ig = 0; for (float v : mk->p) ig += v > 0.5f; if (ig * 10 > mk->p.size() * 3) so.dof = std::min(so.dof, 6); }
        if (t >= o.dof_range_lo && t <= o.dof_range_hi) so.dof = std::min(so.dof, o.dof_range_dof);
        so.prior = &Hs[t];
        const double pw[8] = {o.prior_lin, o.prior_lin, 0, o.prior_lin, o.prior_lin, 0, o.prior_persp, o.prior_persp};
        for (int i = 0; i < 8; ++i) so.prior_w[i] = pw[i];
        so.source_ignore = o.ignore_source ? o.ignore_source(a0) : nullptr;
        H3 Pa = normed(align_space(getT(t), getS(a0), Hs[t], so).H);
        so.source_ignore = o.ignore_source ? o.ignore_source(a0 + 1) : nullptr;
        H3 Pb = normed(align_space(getT(t), getS(a0 + 1), Pa, so).H);
        nHa[t] = Pa; nHb[t] = Pb;
        for (int i = 0; i < 9; ++i) nHf[t].m[i] = (1 - f) * Pa.m[i] + f * Pb.m[i];
      }, nullptr, 0, 1);
      R.Ha = nHa; R.Hb = nHb; R.Hf = nHf;
    }
    // 4c) temporal consistency (sprint 11): the homography at the solved time is a smooth function of the frame.
    //     A frame whose H jumps away from the robust temporal reference (median of its neighbours, per parameter)
    //     is re-fitted from that reference; if it still does not agree, the reference replaces it.
    R.temporal_fixed.assign(nT, 0);
    if (o.temporal_refit) for (int pass = 0; pass < 2; ++pass) {
      std::vector<H3> med(nT);
      for (int t = 0; t < nT; ++t)
        for (int i = 0; i < 8; ++i) {
          std::vector<double> v;
          for (int k = -o.temporal_radius; k <= o.temporal_radius; ++k)
            if (t + k >= 0 && t + k < nT) v.push_back(R.Hf[t + k].m[i] / R.Hf[t + k].m[8]);
          std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
          med[t].m[i] = v[v.size() / 2];
        }
      const double thr2 = (o.temporal_tol * w) * (o.temporal_tol * w);
      std::vector<H3> nHa(R.Ha), nHb(R.Hb), nHf(R.Hf);
      std::vector<int> fixed(R.temporal_fixed);
      parallel_for(nT, o.threads, [&](int t) {
        const double e0 = map_err(normed(R.Hf[t]), med[t], w, h);
        if (!(e0 > thr2)) return;
        const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const int a0 = std::min(int(std::floor(s)), nS - 2); const double f = s - a0;
        const Image* mk = o.ignore ? o.ignore(t) : nullptr;
        SpaceOptions so = o.space; so.ignore_mask = mk;
        if (mk) { size_t ig = 0; for (float v : mk->p) ig += v > 0.5f; if (ig * 10 > mk->p.size() * 3) so.dof = std::min(so.dof, 6); }
        so.levels = std::min(so.levels, 3);   // start close: only the fine levels, so it cannot wander off again
        so.source_ignore = o.ignore_source ? o.ignore_source(a0) : nullptr;
        H3 Pa = normed(align_space(getT(t), getS(a0), med[t], so).H);
        so.source_ignore = o.ignore_source ? o.ignore_source(a0 + 1) : nullptr;
        H3 Pb = normed(align_space(getT(t), getS(a0 + 1), Pa, so).H);
        H3 Hn; for (int i = 0; i < 9; ++i) Hn.m[i] = (1 - f) * Pa.m[i] + f * Pb.m[i];
        const double e1 = map_err(normed(Hn), med[t], w, h);
        if (e1 < e0) { nHa[t] = Pa; nHb[t] = Pb; nHf[t] = Hn; fixed[t] = 1; }
        if (pass == 1 && std::min(e0, e1) > thr2) { nHa[t] = med[t]; nHb[t] = med[t]; nHf[t] = med[t]; fixed[t] = 2; }
      }, nullptr, 0, 1);
      R.Ha = nHa; R.Hb = nHb; R.Hf = nHf; R.temporal_fixed = fixed;
    }
    // 4c') 0.3.5 confidence: low-confidence frames (not held) take the target-motion prediction from their confident
    //      neighbour (forward from the frame before a run; backward from the frame after it when the run starts the
    //      shot); with conf_refine also a fine-level refinement that is rejected if it leaves the prediction.
    if (o.confidence && R.interpolated > 0) {
      auto lowfit = [&](int t, const H3& predA, const H3& predB) {
        if (!o.conf_refine) { R.Ha[t] = predA; R.Hb[t] = predB; }
        else {
          const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
          const int a0 = std::min(int(std::floor(s)), nS - 2);
          const Image* mk = o.ignore ? o.ignore(t) : nullptr;
          SpaceOptions so = o.space; so.ignore_mask = mk; so.levels = std::min(so.levels, 2);
          so.source_ignore = o.ignore_source ? o.ignore_source(a0) : nullptr;
          H3 Pa = normed(align_space(getT(t), getS(a0), predA, so).H);
          so.source_ignore = o.ignore_source ? o.ignore_source(a0 + 1) : nullptr;
          H3 Pb = normed(align_space(getT(t), getS(a0 + 1), predB, so).H);
          const double tol2 = (0.005 * w) * (0.005 * w);
          R.Ha[t] = map_err(Pa, predA, w, h) < tol2 ? Pa : predA;
          R.Hb[t] = map_err(Pb, predB, w, h) < tol2 ? Pb : predB;
        }
        const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const double f = s - std::min(int(std::floor(s)), nS - 2);
        for (int i = 0; i < 9; ++i) R.Hf[t].m[i] = (1 - f) * R.Ha[t].m[i] + f * R.Hb[t].m[i];
      };
      // a frame's source pair can change between neighbours (a0 differs): the prediction then maps to the other
      // source frame; step it through the source motion is not available, so the neighbour's pair is reused as is.
      int t = 0;
      while (t < nT) {
        if (!R.low_conf[t] || held[t]) { ++t; continue; }
        int e = t; while (e + 1 < nT && R.low_conf[e + 1] && !held[e + 1]) ++e;
        if (t > 0) for (int k = t; k <= e; ++k) lowfit(k, normed(R.Ha[k - 1] * M[k - 1].inverse()), normed(R.Hb[k - 1] * M[k - 1].inverse()));
        else if (e + 1 < nT) for (int k = e; k >= t; --k) lowfit(k, normed(R.Ha[k + 1] * M[k]), normed(R.Hb[k + 1] * M[k]));
        t = e + 1;
      }
    }
    // 4d) 0.3.3: held frames (source time held on the first / last source frame). The target keeps moving while the
    //     source frame stays, so the fit is against content up to several frames away and can diverge (run 5:
    //     f241 / f242 off by 1780 / 28181 px). Each one is predicted from its neighbour through the target motion
    //     (H_t = H_t-1 * M_t-1^-1 at the end, H_t = H_t+1 * M_t at the start), refined from that prediction on
    //     the fine levels only, and the refinement is kept only if it stays near the prediction.
    if (held_last >= 0 && (R.held_start > 0 || R.held_end > 0)) {
      const double tol2 = (0.01 * w) * (0.01 * w);
      auto guard = [&](int t, const H3& predA, const H3& predB) {
        const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const int a0 = std::min(int(std::floor(s)), nS - 2); const double f = s - a0;
        const Image* mk = o.ignore ? o.ignore(t) : nullptr;
        SpaceOptions so = o.space; so.ignore_mask = mk; so.levels = std::min(so.levels, 2);
        if (mk) { size_t ig = 0; for (float v : mk->p) ig += v > 0.5f; if (ig * 10 > mk->p.size() * 3) so.dof = std::min(so.dof, 6); }
        so.source_ignore = o.ignore_source ? o.ignore_source(a0) : nullptr;
        H3 Pa = normed(align_space(getT(t), getS(a0), predA, so).H);
        so.source_ignore = o.ignore_source ? o.ignore_source(a0 + 1) : nullptr;
        H3 Pb = normed(align_space(getT(t), getS(a0 + 1), predB, so).H);
        if (!(map_err(Pa, predA, w, h) < tol2)) Pa = predA;
        if (!(map_err(Pb, predB, w, h) < tol2)) Pb = predB;
        R.Ha[t] = Pa; R.Hb[t] = Pb;
        for (int i = 0; i < 9; ++i) R.Hf[t].m[i] = (1 - f) * Pa.m[i] + f * Pb.m[i];
      };
      for (int t = held_last + 1; t < nT; ++t)
        guard(t, normed(R.Ha[t - 1] * M[t - 1].inverse()), normed(R.Hb[t - 1] * M[t - 1].inverse()));
      for (int t = held_first - 1; t >= 0; --t)
        guard(t, normed(R.Ha[t + 1] * M[t]), normed(R.Hb[t + 1] * M[t]));
    }
  }
  W4D_PHASE("4b-4d direct fits");
  // 5) parallax residual field: measured against the nearest whole source frame (no blending), which the model
  //    reaches through Hf * M^(a - src_time); then smoothed over time and stored in normalised units.
  if (o.grid_w > 1 && o.grid_h > 1) {
    std::vector<std::vector<float>> g(nT), GA(nT), GB(nT);
    parallel_for(nT, o.threads, [&](int t) {
      const Image* mk = o.ignore ? o.ignore(t) : nullptr;
      if (!R.Ha.empty()) {
        // one field per whole source frame (each against its own homography); the stored field is their blend
        const double s = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const int a0 = std::min(int(std::floor(s)), nS - 2); const float f = float(s - a0);
        GA[t] = measure_grid(getT(t), getS(a0), R.Ha[t], mk, o, o.ignore_source ? o.ignore_source(a0) : nullptr);
        GB[t] = measure_grid(getT(t), getS(a0 + 1), R.Hb[t], mk, o, o.ignore_source ? o.ignore_source(a0 + 1) : nullptr);
        g[t].resize(GA[t].size());
        for (size_t i = 0; i < g[t].size(); ++i) g[t][i] = (1 - f) * GA[t][i] + f * GB[t][i];
      } else {
        int a = int(std::lround(R.src_time[t]));
        a = std::min(std::max(a, 0), nS - 1);
        g[t] = measure_grid(getT(t), getS(a), R.Hf[t] * frac(M[t], a - R.src_time[t]), mk, o);
      }
    }, nullptr, 0, 1);
    if (!R.Ha.empty() && o.grid_time_median > 0) {
      // the per-source-frame fields are smooth over the shot too: a median over neighbouring frames removes
      // single-frame outliers without blurring; then the blended field is rebuilt from them
      auto tmed = [&](std::vector<std::vector<float>>& G) {
        std::vector<std::vector<float>> out(G);
        for (int t = 0; t < nT; ++t)
          for (size_t i = 0; i < G[t].size(); ++i) {
            float v[16]; int c = 0;
            for (int k = -o.grid_time_median; k <= o.grid_time_median && c < 16; ++k)
              if (t + k >= 0 && t + k < nT && G[t + k].size() == G[t].size()) v[c++] = G[t + k][i];
            std::nth_element(v, v + c / 2, v + c);
            out[t][i] = v[c / 2];
          }
        G.swap(out);
      };
      tmed(GA); tmed(GB);
      for (int t = 0; t < nT; ++t) {
        const double sv = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const int a0 = std::min(int(std::floor(sv)), nS - 2); const float f = float(sv - a0);
        for (size_t i = 0; i < g[t].size(); ++i) g[t][i] = (1 - f) * GA[t][i] + f * GB[t][i];
      }
    }
    if (o.tension_pass && !R.Ha.empty()) {
      R.tension.assign(nT, std::vector<float>(size_t(o.grid_w) * o.grid_h, 0.f));
      std::vector<int> nfl(nT, 0), nfx(nT, 0);
      for (int iter = 0; iter < std::max(1, o.tension_iters); ++iter) {
        std::vector<std::vector<float>> NA(GA), NB(GB);
        parallel_for(nT, o.threads, [&](int t) {
          const Image* mk = o.ignore ? o.ignore(t) : nullptr;
          const double sv = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
          const int a0 = std::min(int(std::floor(sv)), nS - 2);
          int f1 = 0, x1 = 0, f2 = 0, x2 = 0;
          tension_fix(getT(t), getS(a0), R.Ha[t], mk, o.ignore_source ? o.ignore_source(a0) : nullptr, GA[t],
                      t > 0 ? &GA[t - 1] : nullptr, t + 1 < nT ? &GA[t + 1] : nullptr, o, NA[t], iter == 0 ? &R.tension[t] : nullptr, &f1, &x1);
          tension_fix(getT(t), getS(a0 + 1), R.Hb[t], mk, o.ignore_source ? o.ignore_source(a0 + 1) : nullptr, GB[t],
                      t > 0 ? &GB[t - 1] : nullptr, t + 1 < nT ? &GB[t + 1] : nullptr, o, NB[t], nullptr, &f2, &x2);
          if (iter == 0) { nfl[t] = f1; nfx[t] = x1; }
        }, nullptr, 0, 1);
        GA.swap(NA); GB.swap(NB);
      }
      R.tension_flagged = nfl; R.tension_fixed = nfx;
      for (int t = 0; t < nT; ++t) {
        const double sv = std::min(std::max(R.src_time[t], 0.0), double(nS - 1));
        const int a0 = std::min(int(std::floor(sv)), nS - 2); const float f = float(sv - a0);
        for (size_t i = 0; i < g[t].size(); ++i) g[t][i] = (1 - f) * GA[t][i] + f * GB[t][i];
      }
    }
    R.grid_w = o.grid_w; R.grid_h = o.grid_h; R.grid.assign(nT, std::vector<float>(size_t(o.grid_w) * o.grid_h * 2, 0.f));
    const float to_norm = 1.f / (w * 0.5f);
    for (int t = 0; t < nT; ++t) {
      int c = 0;
      for (int k = -o.grid_time_radius; k <= o.grid_time_radius; ++k) {
        if (t + k < 0 || t + k >= nT) continue;
        for (size_t i = 0; i < R.grid[t].size(); ++i) R.grid[t][i] += g[t + k][i];
        ++c;
      }
      for (float& v : R.grid[t]) v *= to_norm / c;
    }
    if (!R.Ha.empty()) {   // per-source-frame fields for a renderer that warps the two frames separately
      for (auto& v : GA) for (float& q : v) q *= to_norm;
      for (auto& v : GB) for (float& q : v) q *= to_norm;
      R.grid_a = GA; R.grid_b = GB;
    }
  }
  // results leave in normalised coordinates (resolution independent)
  R.H = to_normalized(R.H, w, h);
  for (H3& q : R.Hf) q = to_normalized(q, w, h);
  R.time_scores = cres;
  for (H3& q : R.Ha) q = to_normalized(q, w, h);
  for (H3& q : R.Hb) q = to_normalized(q, w, h);
  W4D_PHASE("5 fields");
  if (w4d_timing) std::fprintf(stderr, "  align_space: %lld calls, %.1f s total (thread time), prep %.1f s\n", (long long)g_fit_calls, g_fit_ns * 1e-9, g_prep_ns * 1e-9);
  return R;
}

}  // namespace moco
