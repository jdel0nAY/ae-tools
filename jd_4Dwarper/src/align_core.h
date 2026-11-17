// MoCo Align core: CPU, plain C++17, no third-party libraries.
// Aligns a SOURCE plate to a TARGET plate in space (homography, later + residual grid) and time (sub-frame offset).
#pragma once
#include <vector>
#include <functional>

namespace moco {

struct Image {            // single-channel float, row-major
  int w = 0, h = 0;
  std::vector<float> p;
  Image() = default;
  Image(int W, int H, float v = 0.f) : w(W), h(H), p(size_t(W) * H, v) {}
  float& at(int x, int y) { return p[size_t(y) * w + x]; }
  float at(int x, int y) const { return p[size_t(y) * w + x]; }
  float bilinear(float x, float y, bool* inside = nullptr) const;
};

// 3x3 homography, row-major. Convention: source_px = H * target_px (pixel coords of the full-res frames).
struct H3 {
  double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  static H3 identity() { return H3(); }
  H3 operator*(const H3& b) const;
  H3 inverse() const;
  void apply(double x, double y, double& ox, double& oy) const;
};

// Lighting-robust representation: local contrast normalisation (zero mean / unit std over a box window).
Image normalize_local(const Image& in, int radius);
Image downsample2(const Image& in);
Image warp(const Image& src, const H3& H, Image* valid = nullptr);   // out(x) = src(H x)

struct SpaceOptions {
  int levels = 4;           // pyramid levels (coarse-to-fine)
  int iters = 30;           // Gauss-Newton iterations per level
  int dof = 8;              // 2 = translation, 6 = affine, 8 = homography
  int norm_radius = 6;      // local contrast window at full res
  const Image* ignore_mask = nullptr;  // target-space mask, >0.5 = ignore (talent/crew)
  const Image* source_ignore = nullptr;  // source-space mask, >0.5 = ignore (what differs in the source take)
  // Soft prior: pull the fit towards this homography (pixel coordinates), per parameter m[0..7], as a fraction of
  // the data term's own curvature for that parameter (1 = as strong as the data). nullptr = none.
  const H3* prior = nullptr;
  double prior_w[8] = {0, 0, 0, 0, 0, 0, 0, 0};
};
struct SpaceResult { H3 H; double residual = 0; double inlier_frac = 0; };

// Robust (Huber) forward-compositional direct alignment, coarse-to-fine.
SpaceResult align_space(const Image& target, const Image& source, const H3& init, const SpaceOptions& o = {});

struct TimeOptions {
  int range = 3;            // search +- frames
  int level = 2;            // pyramid level for scoring
  int smooth_radius = 2;    // moving-average radius on the offset curve
};
// Returns per-target-frame source time (float frames). getT/getS give grey frames by index.
std::vector<double> align_time(int nT, int nS, const std::function<const Image&(int)>& getT,
                               const std::function<const Image&(int)>& getS, const H3& H,
                               const TimeOptions& o = {}, std::vector<double>* raw = nullptr);

}  // namespace moco
