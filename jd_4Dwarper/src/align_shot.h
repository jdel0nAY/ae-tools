// Whole-shot solve (sprint 2). Breaks the pan/time ambiguity without blending frames:
//   P_t = measured homography target frame t -> source frame t+d0 (d0 = coarse integer offset)
//   M_t = measured homography target frame t -> target frame t+1 (the camera's own motion)
//   model: P_t = H * M_t^tau, with H the rig offset and tau the sub-frame time offset.
#pragma once
#include "align_core.h"

namespace moco {
struct ShotOptions {
  SpaceOptions space;          // settings of the per-frame homography fits
  int range = 3;               // time search +- frames
  double time_step = 0.05;     // DP state spacing (frames)
  double time_smooth = 5.0;    // DP penalty on (offset change)^2
  double time_sigma_px = 0.25; // expected fit noise of the homographies (px): scales data vs smoothness
  double max_slope = 0.3;      // max offset change per frame (frames)
  bool per_frame_H = true;     // per-frame H with a prior to the shot H, smoothed over time
  double h_prior = 0.5;        // MILD pull of each frame's own H to the shot H (Jean: drift is allowed); 0 = off
  int h_smooth_radius = 3;
  int window_radius = 6;       // frames each side used by the local time solve (H and tau constant inside it)
  // Per target frame: mask at the analysis size, > 0.5 = do NOT measure here (talent, crew, outside the analysis
  // region), or nullptr. Applies to the space AND the time solve. The warp itself still covers the whole frame:
  // under a masked zone it follows the model fitted on the rest. Called from worker threads.
  std::function<const Image*(int)> ignore;
  // Same for the SOURCE take, per source frame, in source space (e.g. the other talent's arm). Optional.
  std::function<const Image*(int)> ignore_source;
  // Second pass per frame: zones where the aligned source and the target still disagree (lighting-normalised,
  // blurred) are masked too and the frame is fitted again.
  bool auto_ignore = false;
  double auto_thresh = 3.0;    // x the median blurred residual
  // Time from parallax: score every candidate source frame by the residual left after its own homography fit
  // (reduced size). Needed for moves with depth (dolly); harmless but uninformative on a flat pan.
  bool time_parallax = false;
  double parallax_weight = 20.0;   // 1 unit of DP cost per 5 % rise of the residual over its minimum
  double motion_weight = 1.0;      // weight of the camera-motion time model (P = H * M^tau)
  double parallax_margin = 0.03;   // residual rises below this fraction are treated as noise
  // OFF by default: on the real shot (runs r4_a / r4_b) it flagged most frames after 100, because the per-frame
  // perspective terms jitter, and replacing them by the median made the result much worse.
  bool temporal_refit = false;     // re-fit frames whose H jumps away from the median of their neighbours
  int temporal_radius = 4;         // frames each side for that median
  double temporal_tol = 0.004;     // allowed RMS distance from it, as a fraction of the frame width
  // OFF by default: with radius 2 the real shot got worse (run r4_c: p95 4.0 px against 2.4 px without it).
  int grid_time_median = 0;        // temporal median (frames each side) on the per-source-frame fields; 0 = off
  // Time constraint: frames after time_trust_end stay within +-time_window frames of the offset the trusted part
  // ends with. -1 = off.
  int time_trust_end = -1;
  double time_window = 1.0;
  // Soft prior on the per-frame homography (second fit, with time_parallax): each frame is pulled towards the
  // temporally smoothed sequence, perspective terms hardest, then scale/rotation; translation is left free.
  // After time_trust_end the perspective prior is the median of the last 40 trusted frames. 0 = off.
  double prior_persp = 0.0, prior_lin = 0.0;
  // Fewer degrees of freedom for the direct fits of a frame range (e.g. 6 = affine while something enters the
  // frame). Off when lo > hi.
  int dof_range_lo = 0, dof_range_hi = -1, dof_range_dof = 6;
  // Tension pass: after the field is solved, cells whose residual, strain or temporal jump is above
  // median + tension_k * MAD get a wider local search and an edge-aware cohesion. Off by default.
  bool tension_pass = false;
  // 0.3.5 confidence: a frame is LOW confidence when less than conf_min_set of it is set (after the ignore masks)
  // or its time score has no clear minimum (best other candidate within conf_min_sharp of the best). Low frames
  // take their offset from their confident neighbours (linear) and their homography from the target-motion
  // prediction (like held frames); conf_refine also refines that prediction on the fine levels (with rejection).
  bool confidence = false;
  double conf_min_set = 0.55, conf_min_sharp = 0.05;
  bool conf_refine = false;
  // 0.3.5 speed: fit the time candidates only on every cand_stride-th frame (1 = all frames, as before).
  int cand_stride = 1;
  double tension_k = 3.0;
  int tension_radius = 6;          // local search radius, analysis pixels
  double tension_edge = 0.08;      // luma difference between cells at which cohesion falls to 1/e
  int tension_iters = 1;
  bool wide_search = true;         // coarse scale/translation search before the candidate fits (large take differences)
  double parallax_jump_cost = 0.3; // cost of changing the whole-frame base offset by one between frames
  // Parallax residual field (0 = off): a coarse grid of extra displacements on top of the per-frame homography.
  int grid_w = 0, grid_h = 0;
  double grid_smooth = 0.5;    // spatial regularisation (pull of each node to its neighbours vs its own measure)
  int grid_time_radius = 2;    // temporal smoothing, frames each side
  int threads = 1;             // worker threads for the per-frame fits (getT/getS are then called from workers)
  // Called on the CALLING thread while workers run; return false to cancel (ShotResult::cancelled).
  std::function<bool(int done, int total)> progress;
};
// Resolution-independent storage: u = (x - (w-1)/2) / (w/2), v = (y - (h-1)/2) / (w/2). An H analysed on a proxy
// applies to any other resolution of the same frame: from_normalized(to_normalized(H, wA, hA), wB, hB).
H3 to_normalized(const H3& Hpx, int w, int h);
H3 from_normalized(const H3& Hn, int w, int h);
struct ShotResult {
  // Both in NORMALISED coordinates (target -> source); use from_normalized(H, w, h) for pixels of any size.
  H3 H;                          // one homography for the shot
  std::vector<H3> Hf;            // per target frame (== H when per_frame_H is off)
  std::vector<double> src_time;  // per target frame, float source frame (may fall outside [0,nS-1]: hold)
  // With time_parallax: homographies fitted directly to source frames floor(src_time) and floor(src_time)+1
  // (normalised). A renderer should warp each of the two frames with its own one before blending. Empty otherwise.
  std::vector<H3> Ha, Hb;
  // With time_parallax: per target frame, the residual score of each candidate offset -range..+range (-1 = none).
  std::vector<std::vector<double>> time_scores;
  // Per target frame: 0 = fit kept, 1 = re-fitted from its neighbours, 2 = replaced by the temporal median.
  std::vector<int> temporal_fixed;
  // Tension pass: per target frame, per field cell (field of the floor source frame): tension score (> 1 =
  // flagged, -1 = no set pixels), and how many cells were flagged / improved by the local search.
  std::vector<std::vector<float>> tension;
  std::vector<int> tension_flagged, tension_fixed;
  double time_ref = 0;   // with time_trust_end: the offset the later frames are held near
  // 0.3.3: target frames whose source time ran past the source frames and is held on the first / last one
  // (their homographies are guarded: seeded from the neighbouring frame through the target motion).
  int held_start = 0, held_end = 0;
  // 0.3.5: per target frame 1 = low confidence (interpolated); count of them
  std::vector<char> low_conf;
  int interpolated = 0;
  int coarse_offset = 0;
  bool cancelled = false;
  // Residual field per target frame (empty when off): grid_w * grid_h pairs (du, dv), row-major, NORMALISED units
  // (fractions of the half width). Node (i, j) sits at the centre of cell (i, j) of the target frame.
  // source = H(target) + field(target). Same layout as the record field in warp_record.h.
  int grid_w = 0, grid_h = 0;
  std::vector<std::vector<float>> grid;
  // With time_parallax: the field of each of the two whole source frames (against Ha / Hb); grid is their blend.
  std::vector<std::vector<float>> grid_a, grid_b;
};
// Field displacement (pixels of a w x h frame) at target pixel (x, y), bilinear between the nodes.
void grid_at(const std::vector<float>& g, int gw, int gh, int w, int h, double x, double y, double& dx, double& dy);
ShotResult align_shot(int nT, int nS, const std::function<const Image&(int)>& getT,
                      const std::function<const Image&(int)>& getS, const ShotOptions& o = {});
}  // namespace moco
