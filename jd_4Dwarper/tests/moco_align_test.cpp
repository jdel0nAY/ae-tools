// Synthetic test: procedural plate sequence, source = known homography + known time offset + lighting change
// + an extra moving "talent" blob that only exists in the source. Checks align_shot recovers space and time,
// for a variable-speed pan and a constant-speed pan.
#include "../src/align_shot.h"
#include "../src/render_core.h"
#include <mutex>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

using namespace moco;

static int g_variant = 0;  // 0 = variable-speed pan, 1 = constant-speed pan
static bool g_auto = false;  // `warper_test auto`: run everything with the auto-ignore second pass on

static float hash2(int x, int y) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return float((h ^ (h >> 16)) & 0xffffff) / float(0xffffff);
}
static float vnoise(double x, double y) {
  int xi = int(std::floor(x)), yi = int(std::floor(y));
  double fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
  float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
  return float((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy);
}
// Scene seen by the camera at time t (continuous). Camera pans and slowly pushes in.
static float scene(double x, double y, double t) {
  double z = 1.0 + 0.002 * t;
  double pan = 2.0 * t + (g_variant == 0 ? 8.0 * std::sin(t * 0.35) : 0.0);
  double X = (x - 160) / z + 160 + pan, Y = (y - 90) / z + 90 + 0.6 * t + 0.02 * t * t;
  float v = 0, a = 0.5f; double f = 1.0 / 24;
  for (int o = 0; o < 4; ++o, a *= 0.5f, f *= 2) v += a * vnoise(X * f + o * 17, Y * f - o * 31);
  return v;
}

// The scene is defined in 320x180 coordinates. Rendered at 2x (pixel centres: x = (X + 0.5) / 2 - 0.5).
static Image render2x(const std::function<float(double, double)>& f, int W, int Hh) {
  Image o(2 * W, 2 * Hh);
  for (int Y = 0; Y < o.h; ++Y)
    for (int X = 0; X < o.w; ++X) o.at(X, Y) = f((X + 0.5) / 2 - 0.5, (Y + 0.5) / 2 - 0.5);
  return o;
}

// Analyse on the 320x180 proxy (box-downsampled 640x360 plates), apply the stored normalised result at 640x360.
static bool run_2x(const char* name) {
  g_variant = 0;
  const int W = 320, Hh = 180, N = 24;
  const double dt_true = 1.4;
  double ang = 0.6 * 3.14159265358979 / 180, sc = 1.015, cx = (W - 1) * 0.5, cy = (Hh - 1) * 0.5;
  H3 G;
  G.m[0] = sc * std::cos(ang); G.m[1] = -sc * std::sin(ang); G.m[3] = sc * std::sin(ang); G.m[4] = sc * std::cos(ang);
  G.m[2] = cx - G.m[0] * cx - G.m[1] * cy + 3.2; G.m[5] = cy - G.m[3] * cx - G.m[4] * cy - 2.1;
  G.m[6] = 2e-6;
  H3 Ginv = G.inverse();
  std::vector<Image> T(N), S(N);
  for (int i = 0; i < N; ++i) {
    Image th = render2x([&](double x, double y) { return scene(x, y, i); }, W, Hh);
    Image sh = render2x([&](double x, double y) {
      double sx, sy; Ginv.apply(x, y, sx, sy);
      float v = 0.65f * scene(sx, sy, i + dt_true) + 0.12f + 0.15f * float(x) / W;
      double bx = 60 + 8.0 * i, by = 110;
      if ((x - bx) * (x - bx) + (y - by) * (y - by) < 22 * 22) v = 0.95f;
      return v; }, W, Hh);
    T[i] = downsample2(th); S[i] = downsample2(sh);   // the proxy the analysis sees
  }
  ShotOptions so; so.threads = 8; so.auto_ignore = g_auto;
  ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
  // True mapping in 2x pixels: C maps 2x px -> 1x px.
  H3 C; C.m[0] = 0.5; C.m[2] = -0.25; C.m[4] = 0.5; C.m[5] = -0.25;
  H3 G2 = C.inverse() * G * C;
  double worst_rms = 0, worst_max = 0;
  for (int t = 0; t < N; ++t) {
    H3 H2 = from_normalized(R.Hf[t], 2 * W, 2 * Hh);
    double se = 0, mx = 0; int n = 0;
    for (int y = 0; y < 2 * Hh; y += 20)
      for (int x = 0; x < 2 * W; x += 20) {
        double ax, ay, bx, by; H2.apply(x, y, ax, ay); G2.apply(x, y, bx, by);
        double e = std::hypot(ax - bx, ay - by); se += e * e; mx = std::max(mx, e); ++n;
      }
    worst_rms = std::max(worst_rms, std::sqrt(se / n)); worst_max = std::max(worst_max, mx);
  }
  double temax = 0;
  for (int t = 3; t < N - 3; ++t) temax = std::max(temax, std::fabs(R.src_time[t] - (t - dt_true)));
  // Targets scale with resolution: 0.25 px RMS / 0.5 px max at 1x = 0.5 / 1.0 px at 2x.
  bool ok = worst_rms < 0.5 && worst_max < 1.0 && temax < 0.15;
  std::printf("[%s] per-frame H at 640x360: worst RMS %.3f max %.3f px (= %.3f / %.3f px at 1x) | time max %.3f fr | %s\n",
              name, worst_rms, worst_max, worst_rms / 2, worst_max / 2, temax, ok ? "PASS" : "FAIL");
  return ok;
}

static bool run(int variant, const char* name) {
  g_variant = variant;
  const int W = 320, Hh = 180, N = 24;
  const double dt_true = 1.4;          // source frame i shows scene time i + 1.4
  double ang = 0.6 * 3.14159265358979 / 180, sc = 1.015, cx = (W - 1) * 0.5, cy = (Hh - 1) * 0.5;
  H3 G;                                 // expected H (target px -> source px)
  G.m[0] = sc * std::cos(ang); G.m[1] = -sc * std::sin(ang); G.m[3] = sc * std::sin(ang); G.m[4] = sc * std::cos(ang);
  G.m[2] = cx - G.m[0] * cx - G.m[1] * cy + 3.2; G.m[5] = cy - G.m[3] * cx - G.m[4] * cy - 2.1;
  G.m[6] = 2e-6;  // a touch of perspective
  H3 Ginv = G.inverse();
  std::vector<Image> T(N), S(N);
  for (int i = 0; i < N; ++i) {
    T[i] = Image(W, Hh); S[i] = Image(W, Hh);
    for (int y = 0; y < Hh; ++y)
      for (int x = 0; x < W; ++x) {
        T[i].at(x, y) = scene(x, y, i);
        double sx, sy; Ginv.apply(x, y, sx, sy);
        float v = scene(sx, sy, i + dt_true);
        v = 0.65f * v + 0.12f + 0.15f * float(x) / W;            // lighting: gain, lift, horizontal ramp
        double bx = 60 + 8.0 * i, by = 110;                      // "talent" only in source
        if ((x - bx) * (x - bx) + (y - by) * (y - by) < 22 * 22) v = 0.95f;
        S[i].at(x, y) = v;
      }
  }
  auto t0 = std::chrono::steady_clock::now();
  ShotOptions so; so.threads = 8; so.auto_ignore = g_auto;
  ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  auto space_err = [&](const H3& H, double& rms, double& mx) {
    double se = 0; int n = 0; mx = 0;
    for (int y = 0; y < Hh; y += 10)
      for (int x = 0; x < W; x += 10) {
        double ax, ay, bx, by; H.apply(x, y, ax, ay); G.apply(x, y, bx, by);
        double e = std::hypot(ax - bx, ay - by); se += e * e; mx = std::max(mx, e); ++n;
      }
    rms = std::sqrt(se / n);
  };
  double rms, maxe; space_err(from_normalized(R.H, W, Hh), rms, maxe);
  double frms = 0, fmax = 0;   // worst per-frame H
  for (int t = 0; t < N; ++t) { double a, b; space_err(from_normalized(R.Hf[t], W, Hh), a, b); frms = std::max(frms, a); fmax = std::max(fmax, b); }
  // Time: expected source time for target t is t - dt_true. Skip the ends where the source runs out.
  double te = 0, temax = 0; int m = 0;
  for (int t = 3; t < N - 3; ++t) {
    double e = std::fabs(R.src_time[t] - (t - dt_true));
    te += e; temax = std::max(temax, e); ++m;
  }
  te /= m;
  bool ok = rms < 0.25 && maxe < 0.5 && frms < 0.25 && temax < 0.15;
  std::printf("[%s] coarse offset %d | space shot-H RMS %.3f max %.3f px | per-frame H worst RMS %.3f max %.3f px | "
              "time mean %.3f max %.3f fr | %.2f s | %s\n",
              name, R.coarse_offset, rms, maxe, frms, fmax, te, temax, secs, ok ? "PASS" : "FAIL");
  return ok;
}

// Time offset that varies over the shot (1.0 -> 1.8 frames) and a rig offset that drifts (1 px in x, 0.5 px in y).
// Checks the per-frame H against the true H of the matched source time, and the time curve.
static bool run_vary(const char* name) {
  g_variant = 0;
  const int W = 320, Hh = 180, N = 24;
  auto dt = [&](double i) { return 1.0 + 0.8 * i / (N - 1); };          // source frame i shows scene time i + dt(i)
  auto Gof = [&](double i) {                                            // true H (target px -> source px) at source frame i
    double ang = 0.6 * 3.14159265358979 / 180, sc = 1.015, cx = (W - 1) * 0.5, cy = (Hh - 1) * 0.5;
    H3 G;
    G.m[0] = sc * std::cos(ang); G.m[1] = -sc * std::sin(ang); G.m[3] = sc * std::sin(ang); G.m[4] = sc * std::cos(ang);
    G.m[2] = cx - G.m[0] * cx - G.m[1] * cy + 3.2 + 1.0 * i / (N - 1); G.m[5] = cy - G.m[3] * cx - G.m[4] * cy - 2.1 + 0.5 * i / (N - 1);
    G.m[6] = 2e-6;
    return G;
  };
  std::vector<Image> T(N), S(N);
  for (int i = 0; i < N; ++i) {
    T[i] = Image(W, Hh); S[i] = Image(W, Hh);
    H3 Ginv = Gof(i).inverse();
    for (int y = 0; y < Hh; ++y)
      for (int x = 0; x < W; ++x) {
        T[i].at(x, y) = scene(x, y, i);
        double sx, sy; Ginv.apply(x, y, sx, sy);
        float v = 0.65f * scene(sx, sy, i + dt(i)) + 0.12f + 0.15f * float(x) / W;
        double bx = 60 + 8.0 * i, by = 110;
        if ((x - bx) * (x - bx) + (y - by) * (y - by) < 22 * 22) v = 0.95f;
        S[i].at(x, y) = v;
      }
  }
  ShotOptions so; so.threads = 8; so.auto_ignore = g_auto;
  ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
  double worst_rms = 0, worst_max = 0, temax = 0, te = 0; int m = 0;
  for (int t = 3; t < N - 3; ++t) {
    double s_true = (t - 1.0) / (1.0 + 0.8 / (N - 1));                  // solves s + dt(s) = t
    double e = std::fabs(R.src_time[t] - s_true); te += e; temax = std::max(temax, e); ++m;
    H3 H = from_normalized(R.Hf[t], W, Hh), G = Gof(s_true);
    double se = 0, mx = 0; int n = 0;
    for (int y = 0; y < Hh; y += 10)
      for (int x = 0; x < W; x += 10) {
        double ax, ay, bx, by; H.apply(x, y, ax, ay); G.apply(x, y, bx, by);
        double d = std::hypot(ax - bx, ay - by); se += d * d; mx = std::max(mx, d); ++n;
      }
    worst_rms = std::max(worst_rms, std::sqrt(se / n)); worst_max = std::max(worst_max, mx);
  }
  te /= m;
  bool ok = worst_rms < 0.25 && worst_max < 0.5 && temax < 0.15;
  std::printf("[%s] per-frame H worst RMS %.3f max %.3f px | time mean %.3f max %.3f fr | %s\n",
              name, worst_rms, worst_max, te, temax, ok ? "PASS" : "FAIL");
  return ok;
}

// `warper_test bench`: solve speed at the analysis cap (1536x864), threads = cores - 2, as the AE effect does.
// Only the solve is timed (no After Effects frame fetch). Not a pass/fail test.
static int bench() {
  g_variant = 0;
  const int W = 1536, Hh = 864, N = 16; const double k = W / 320.0;
  std::vector<Image> T(N), S(N);
  for (int i = 0; i < N; ++i) {
    T[i] = Image(W, Hh); S[i] = Image(W, Hh);
    for (int y = 0; y < Hh; ++y)
      for (int x = 0; x < W; ++x) {
        T[i].at(x, y) = scene(x / k, y / k, i);
        S[i].at(x, y) = 0.65f * scene((x - 12.0) / k, (y + 7.0) / k, i + 1.4) + 0.12f;
      }
  }
  ShotOptions so; so.space.levels = 6;
  unsigned hc = std::thread::hardware_concurrency();
  so.threads = int(std::max(1u, hc > 2 ? hc - 2 : 1u));
  auto t0 = std::chrono::steady_clock::now();
  ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  double mid = R.src_time[N / 2] - N / 2;
  std::printf("[bench] %dx%d, %d frames, %d threads: solve %.2f s = %.3f s/frame (300 frames ~ %.0f s) | offset found %.2f fr (true -1.40)\n",
              W, Hh, N, so.threads, secs, secs / N, 300 * secs / N, mid);
  return 0;
}

// A "talent" that covers over half of the frame and moves WITH the camera (fixed in the frame, like an arm filling
// a close-up), at a different place in the two takes. Without a mask the solver locks onto it and the set slides;
// with an ignore mask over it the set must align. Errors are measured over the whole frame, masked zone included
// (there the warp is carried by the model fitted on the set around it).
static bool run_talent(const char* name) {
  g_variant = 0;
  const int W = 320, Hh = 180, N = 24;
  const double dt_true = 1.4;
  double ang = 0.6 * 3.14159265358979 / 180, sc = 1.015, cx = (W - 1) * 0.5, cy = (Hh - 1) * 0.5;
  H3 G;
  G.m[0] = sc * std::cos(ang); G.m[1] = -sc * std::sin(ang); G.m[3] = sc * std::sin(ang); G.m[4] = sc * std::cos(ang);
  G.m[2] = cx - G.m[0] * cx - G.m[1] * cy + 3.2; G.m[5] = cy - G.m[3] * cx - G.m[4] * cy - 2.1;
  H3 Ginv = G.inverse();
  // talent: x >= 100, y >= 40 in the target (53% of the frame); 14 px left and 9 px up in the source
  auto talent = [](double x, double y) { return 0.2f + 0.6f * vnoise(x / 7.0 + 300, y / 7.0 + 500) + 0.2f * vnoise(x / 2.5 + 40, y / 2.5 + 90); };
  std::vector<Image> T(N), S(N);
  Image mask(W, Hh, 0.f);
  int covered = 0;
  for (int y = 0; y < Hh; ++y)
    for (int x = 0; x < W; ++x) {
      if (x >= 100 && y >= 40) ++covered;
      if (x >= 80 && y >= 26) mask.at(x, y) = 1.f;       // rough roto: covers the talent in BOTH takes, with a margin
    }
  for (int i = 0; i < N; ++i) {
    T[i] = Image(W, Hh); S[i] = Image(W, Hh);
    for (int y = 0; y < Hh; ++y)
      for (int x = 0; x < W; ++x) {
        T[i].at(x, y) = (x >= 100 && y >= 40) ? talent(x, y) : scene(x, y, i);
        double sx, sy; Ginv.apply(x, y, sx, sy);
        float v = (x >= 86 && y >= 31) ? talent(x + 14, y + 9) : scene(sx, sy, i + dt_true);
        S[i].at(x, y) = 0.65f * v + 0.12f + 0.15f * float(x) / W;
      }
  }
  auto solve = [&](bool use_mask, double& rms, double& temax) {
    ShotOptions so; so.threads = 8; so.auto_ignore = g_auto;
    if (use_mask) so.ignore = [&](int) -> const Image* { return &mask; };
    ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
    rms = 0; temax = 0;
    for (int t = 3; t < N - 3; ++t) {
      H3 H = from_normalized(R.Hf[t], W, Hh);
      double se = 0; int n = 0;
      for (int y = 0; y < Hh; y += 10)
        for (int x = 0; x < W; x += 10) {
          double ax, ay, bx, by; H.apply(x, y, ax, ay); G.apply(x, y, bx, by);
          se += (ax - bx) * (ax - bx) + (ay - by) * (ay - by); ++n;
        }
      rms = std::max(rms, std::sqrt(se / n));
      temax = std::max(temax, std::fabs(R.src_time[t] - (t - dt_true)));
    }
  };
  double r0, t0, r1, t1;
  solve(false, r0, t0);
  solve(true, r1, t1);
  bool slides = r0 > 1.0, ok = r1 < 0.5 && t1 < 0.15;
  std::printf("[%s] talent covers %d%% | no mask: worst RMS %.2f px, time max %.2f fr (%s) | with mask: worst RMS %.3f px, "
              "time max %.3f fr | %s\n", name, 100 * covered / (W * Hh), r0, t0, slides ? "slides, as expected" : "did NOT slide",
              r1, t1, slides && ok ? "PASS" : "FAIL");
  return slides && ok;
}

// Parallax: two depth layers. The far set (top two thirds) follows the homography; the near "floor" (y > 120) is
// displaced by an extra (3.0, 0.5) px in the source. One homography cannot fit both; the residual grid must take
// up the difference on the floor without disturbing the far set.
static bool run_parallax(const char* name) {
  g_variant = 0;
  const int W = 320, Hh = 180, N = 24;
  const double dt_true = 1.4, ex = 3.0, ey = 0.5;
  double ang = 0.6 * 3.14159265358979 / 180, sc = 1.015, cx = (W - 1) * 0.5, cy = (Hh - 1) * 0.5;
  H3 G;
  G.m[0] = sc * std::cos(ang); G.m[1] = -sc * std::sin(ang); G.m[3] = sc * std::sin(ang); G.m[4] = sc * std::cos(ang);
  G.m[2] = cx - G.m[0] * cx - G.m[1] * cy + 3.2; G.m[5] = cy - G.m[3] * cx - G.m[4] * cy - 2.1;
  H3 Ginv = G.inverse();
  auto near_tex = [](double x, double y, double t) { return scene(x + 500, y + 300, t); };
  std::vector<Image> T(N), S(N);
  for (int i = 0; i < N; ++i) {
    T[i] = Image(W, Hh); S[i] = Image(W, Hh);
    for (int y = 0; y < Hh; ++y)
      for (int x = 0; x < W; ++x) {
        T[i].at(x, y) = y > 120 ? near_tex(x, y, i) : scene(x, y, i);
        double nx, ny; Ginv.apply(x - ex, y - ey, nx, ny);     // where this source pixel comes from if it is floor
        double fx, fy; Ginv.apply(x, y, fx, fy);
        float v = ny > 120 ? near_tex(nx, ny, i + dt_true) : scene(fx, fy, i + dt_true);
        S[i].at(x, y) = 0.65f * v + 0.12f + 0.15f * float(x) / W;
      }
  }
  ShotOptions so; so.threads = 8; so.auto_ignore = g_auto; so.grid_w = 16; so.grid_h = 9;
  ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
  double nearH = 0, nearG = 0, farG = 0; int nn = 0, nf = 0;
  for (int t = 3; t < N - 3; ++t) {
    H3 H = from_normalized(R.Hf[t], W, Hh);
    for (int y = 10; y < Hh; y += 10)
      for (int x = 20; x < W - 10; x += 20) {
        const bool is_near = y >= 140, is_far = y <= 100;
        if (!is_near && !is_far) continue;                    // skip the band around the depth edge
        double hx, hy, gx, gy, dx, dy;
        H.apply(x, y, hx, hy); G.apply(x, y, gx, gy);
        grid_at(R.grid[t], R.grid_w, R.grid_h, W, Hh, x, y, dx, dy);
        if (is_near) { gx += ex; gy += ey; nearH += (hx - gx) * (hx - gx) + (hy - gy) * (hy - gy); nearG += (hx + dx - gx) * (hx + dx - gx) + (hy + dy - gy) * (hy + dy - gy); ++nn; }
        else { farG += (hx + dx - gx) * (hx + dx - gx) + (hy + dy - gy) * (hy + dy - gy); ++nf; }
      }
  }
  nearH = std::sqrt(nearH / nn); nearG = std::sqrt(nearG / nn); farG = std::sqrt(farG / nf);
  // The 8-dof homography already absorbs part of the floor shift (it bends between the layers), so the check is
  // that the grid ends well under target on both layers and clearly improves the floor.
  bool ok = nearG < 0.25 && farG < 0.25 && nearG < 0.6 * nearH;
  std::printf("[%s] floor: homography only %.2f px RMS, with grid %.3f px | far set with grid %.3f px | %s\n",
              name, nearH, nearG, farG, ok ? "PASS" : "FAIL");
  return ok;
}

// Dolly-in with two depth layers and a time offset. The far set (y <= 110) grows 0.4 % per frame, the near floor
// (y > 110) 2 % per frame, about the frame centre; the source is 1.4 frames ahead and shifted (3.2, -2.1) px.
// A time error cannot be absorbed by one homography here (the layers scale differently), so the time must come out
// right, and at the right time both layers map by the same shift.
static bool run_dolly(const char* name) {
  const int W = 320, Hh = 180, N = 24;
  const double dt_true = 1.4, shx = 3.2, shy = -2.1, cx = (W - 1) * 0.5, cy = (Hh - 1) * 0.5;
  auto tex = [](double x, double y, int seed) {
    float v = 0, a = 0.5f; double f = 1.0 / 24;
    for (int o = 0; o < 4; ++o, a *= 0.5f, f *= 2) v += a * vnoise(x * f + o * 17 + seed * 101, y * f - o * 31 + seed * 57);
    return v;
  };
  auto view = [&](double x, double y, double t) {      // what the camera sees at pixel (x, y) at time t
    const bool near_layer = y > 110;
    const double z = 1.0 + (near_layer ? 0.02 : 0.004) * t;
    return tex((x - cx) / z + cx, (y - cy) / z + cy, near_layer ? 1 : 0);
  };
  std::vector<Image> T(N), S(N);
  for (int i = 0; i < N; ++i) {
    T[i] = Image(W, Hh); S[i] = Image(W, Hh);
    for (int y = 0; y < Hh; ++y)
      for (int x = 0; x < W; ++x) {
        T[i].at(x, y) = view(x, y, i);
        S[i].at(x, y) = 0.65f * view(x - shx, y - shy, i + dt_true) + 0.12f + 0.15f * float(x) / W;
      }
  }
  auto solve = [&](bool parallax, double& temax, double& farE, double& nearE) {
    ShotOptions so; so.threads = 8; so.auto_ignore = g_auto; so.grid_w = 16; so.grid_h = 9; so.time_parallax = parallax;
    ShotResult R = align_shot(N, N, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[i]; }, so);
    temax = 0; farE = 0; nearE = 0; int nf = 0, nn = 0;
    for (int t = 3; t < N - 3; ++t) {
      temax = std::max(temax, std::fabs(R.src_time[t] - (t - dt_true)));
      H3 H = from_normalized(R.Hf[t], W, Hh);
      for (int y = 10; y < Hh; y += 10)
        for (int x = 20; x < W - 10; x += 20) {
          const bool is_near = y >= 130, is_far = y <= 90;
          if (!is_near && !is_far) continue;
          double hx, hy, dx, dy; H.apply(x, y, hx, hy);
          grid_at(R.grid[t], R.grid_w, R.grid_h, W, Hh, x, y, dx, dy);
          const double e = (hx + dx - x - shx) * (hx + dx - x - shx) + (hy + dy - y - shy) * (hy + dy - y - shy);
          if (is_near) { nearE += e; ++nn; } else { farE += e; ++nf; }
        }
    }
    farE = std::sqrt(farE / nf); nearE = std::sqrt(nearE / nn);
  };
  double t0, f0, n0, t1, f1, n1;
  solve(false, t0, f0, n0);
  solve(true, t1, f1, n1);
  bool ok = t1 < 0.15 && f1 < 0.25 && n1 < 0.25;
  std::printf("[%s] motion model only: time max %.2f fr, far %.2f px, near %.2f px | with parallax time score: time max %.3f fr, "
              "far %.3f px, near %.3f px | %s\n", name, t0, f0, n0, t1, f1, n1, ok ? "PASS" : "FAIL");
  return ok;
}

// 0.3.2: the source is SHORTER than the target and the time offset (+1.4) needs source frames past its end, like
// a real take's out point. A recording source callback fails the test on any request outside 0..nS-1; the solved source
// times must stay inside, and w4d::pick_source must never return a frame outside the range, for any amount.
static bool run_fetch_bounds(const char* name) {
  g_variant = 0;
  const int W = 320, Hh = 180, nT = 30, nS = 22;
  std::vector<Image> T(nT), S(nS);
  for (int i = 0; i < nT; ++i) { T[i] = Image(W, Hh); for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x) T[i].at(x, y) = scene(x, y, i); }
  for (int i = 0; i < nS; ++i) { S[i] = Image(W, Hh); for (int y = 0; y < Hh; ++y) for (int x = 0; x < W; ++x) S[i].at(x, y) = 0.7f * scene(x - 3.0, y + 2.0, i + 1.4) + 0.1f; }
  std::mutex mu; std::set<int> bad;
  ShotOptions so; so.threads = 8; so.time_parallax = true; so.grid_w = 16; so.grid_h = 9; so.range = 8;
  ShotResult R = align_shot(nT, nS, [&](int i) -> const Image& { return T[i]; },
                            [&](int i) -> const Image& { if (i < 0 || i >= nS) { std::lock_guard<std::mutex> l(mu); bad.insert(i); return S[0]; } return S[i]; }, so);
  int out_of_range = 0;
  for (double s : R.src_time) if (!(s >= 0.0 && s <= nS - 1)) ++out_of_range;
  int pick_bad = 0;
  const double amts[5] = {0.0, 0.25, 0.5, 0.999, 1.0};
  for (int t = 0; t < nT; ++t)
    for (double ta : amts) for (double sa : amts) for (int bl = 0; bl < 2; ++bl)
      for (double solved : {R.src_time[size_t(t)], double(nS) + 3.7, -2.3, nS - 0.4}) {
        const w4d::SourcePick p = w4d::pick_source(t, solved, 0, nS - 1, ta, sa, bl != 0);
        if (p.passthrough) continue;   // passthrough shows the source layer at the target's own time, by design
        if (p.a < 0 || p.a > nS - 1 || (p.use_b && (p.b < 0 || p.b > nS - 1))) ++pick_bad;
      }
  const bool ok = bad.empty() && out_of_range == 0 && pick_bad == 0;
  std::printf("[%s] target %d frames, source %d: source requests outside 0..%d: %zu | solved times outside: %d | "
              "picks outside (all amounts, blend on/off, solved past the end): %d | last src_time %.2f | %s\n",
              name, nT, nS, nS - 1, bad.size(), out_of_range, pick_bad, R.src_time.back(), ok ? "PASS" : "FAIL");
  return ok;
}

// 0.3.2 Amounts: both 0 -> the output is the source frame t, bit for bit; both 100 % with blend off -> the same
// pixels as the 0.3.1 render (nearest frame through its own homography + field).
static bool run_amounts(const char* name) {
  const int W = 160, Hh = 90, N = 6;
  std::vector<RGB8> F(N);
  for (int i = 0; i < N; ++i) { F[i] = RGB8(W, Hh); for (size_t q = 0; q < F[i].p.size(); ++q) F[i].p[q] = uint8_t((q * 2654435761u + i * 97u) >> 24); }
  H3 Ha, Hb; Ha.m[0] = 1.01; Ha.m[2] = 2.3; Ha.m[5] = -1.7; Hb = Ha; Hb.m[2] = 2.9;
  const int gw = 8, gh = 5; std::vector<float> fa(gw * gh * 2), fb(gw * gh * 2);
  for (size_t i = 0; i < fa.size(); ++i) { fa[i] = 0.002f * float(i % 7) - 0.006f; fb[i] = -fa[i]; }
  std::function<const RGB8&(int)> frames = [&](int i) -> const RGB8& { return F[size_t(i)]; };
  bool ok = true; int checked = 0;
  for (int t = 1; t < N - 2; ++t)
    for (double solved : {t + 0.3, t + 0.7, t + 1.0}) {
      // both 0: bit-identical source frame t
      const w4d::SourcePick p0 = w4d::pick_source(t, solved, 0, N - 1, 0.0, 0.0, false);
      RGB8 o0 = render_amounts(p0, frames, t, Ha, Hb, &fa, &fb, gw, gh, W, Hh, true, nullptr, nullptr);
      ok = ok && p0.passthrough && o0.p == F[size_t(t)].p;
      // both 100 %, blend off: the 0.3.1 render of the nearer frame (floor(solved) with Ha + A, or +1 with Hb + B)
      const w4d::SourcePick p1 = w4d::pick_source(t, solved, 0, N - 1, 1.0, 1.0, false);
      RGB8 o1 = render_amounts(p1, frames, t, Ha, Hb, &fa, &fb, gw, gh, W, Hh, true, nullptr, nullptr);
      const int a0 = int(std::floor(solved)); const bool second = solved - a0 >= 0.5;
      RGB8 ref = warp_blend(F[size_t(second ? a0 + 1 : a0)], nullptr, 0.f, second ? Hb : Ha, W, Hh, second ? &fb : &fa, gw, gh,
                            nullptr, nullptr, true, nullptr, nullptr);
      ok = ok && !p1.passthrough && o1.p == ref.p;
      // Space 0 % alone: the picked source frame unwarped (identity mapping)
      const w4d::SourcePick ps = w4d::pick_source(t, solved, 0, N - 1, 1.0, 0.0, false);
      RGB8 os = render_amounts(ps, frames, t, Ha, Hb, &fa, &fb, gw, gh, W, Hh, true, nullptr, nullptr);
      ok = ok && os.p == F[size_t(ps.a)].p;
      checked += 3;
    }
  std::printf("[%s] %d cases: amounts 0/0 = source frame t bit for bit, 100/100 blend off = 0.3.1 render, space 0 = "
              "unwarped picked frame | %s\n", name, checked, ok ? "PASS" : "FAIL");
  return ok;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "bench") return bench();
  g_auto = argc > 1 && std::string(argv[1]) == "auto";
  const bool fb_ok = run_fetch_bounds("source shorter than the shot: fetch bounds");
  const bool am_ok = run_amounts("Space / Time Amounts");
  bool p = run_parallax("parallax: far set + near floor");
  p = run_dolly("dolly-in, two depths, time offset") && p;
  bool e = run_talent("talent over half the frame, moving with the camera");
  bool d = run_vary("varying offset 1.0-1.8 fr + H drift");
  bool a = run(0, "variable-speed pan");
  bool b = run(1, "constant-speed pan");
  bool c = run_2x("analyse 1x, apply 2x");
  std::printf("%s\n", a && b && c && d && e && p && fb_ok && am_ok ? "PASS" : "FAIL");
  return a && b && c && d && e && p && fb_ok && am_ok ? 0 : 1;
}
