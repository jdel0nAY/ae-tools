// warper_cli: 4Dwarper without After Effects. Same align_shot as the effect; render through src/render_core.h.
//
//   warper_cli align --target <dir> --source <dir> [--ignore <dir>] [--region x0,y0,x1,y1] [--range 8]
//                    [--auto-ignore 1] [--analysis-width 1536] [--mode both|space|time] [--threads N] --out <dir>
//   warper_cli selftest <dir>      writes a small synthetic target/source set into <dir> (for an end-to-end check)
//
// Frame folders hold 000000.png, 000001.png ... (8-bit RGB, as ffmpeg -start_number 0 %06d.png writes them).
// PNG is read and written with Windows' own GDI+ (no third-party code). --ignore mattes: white = do not measure.
// --region is in fractions of the frame (0..1). Output: <out>/aligned/%06d.png (source conformed to the target,
// edges extended), <out>/records.json, <out>/log.txt (appended, same fields as the effect's log).
#define NOMINMAX
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <windows.h>
#include <gdiplus.h>

#include "../src/align_shot.h"
#include "../src/render_core.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
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
    const uint8_t* s = (const uint8_t*)d.Scan0 + ptrdiff_t(y) * d.Stride;   // B,G,R
    uint8_t* q = &out.p[size_t(y) * w * 3];
    for (int x = 0; x < w; ++x, s += 3, q += 3) { q[0] = s[2]; q[1] = s[1]; q[2] = s[0]; }
  }
  bmp.UnlockBits(&d);
  return true;
}

static bool write_png(const fs::path& p, const RGB8& im) {
  static const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
  Gdiplus::Bitmap bmp(im.w, im.h, PixelFormat24bppRGB);
  Gdiplus::Rect r(0, 0, im.w, im.h);
  Gdiplus::BitmapData d;
  if (bmp.LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat24bppRGB, &d) != Gdiplus::Ok) return false;
  for (int y = 0; y < im.h; ++y) {
    uint8_t* s = (uint8_t*)d.Scan0 + ptrdiff_t(y) * d.Stride;
    const uint8_t* q = &im.p[size_t(y) * im.w * 3];
    for (int x = 0; x < im.w; ++x, s += 3, q += 3) { s[0] = q[2]; s[1] = q[1]; s[2] = q[0]; }
  }
  bmp.UnlockBits(&d);
  return bmp.Save(p.wstring().c_str(), &png, nullptr) == Gdiplus::Ok;
}

static fs::path frame_path(const fs::path& dir, int i) {
  char name[32]; std::snprintf(name, sizeof(name), "%06d.png", i);
  return dir / name;
}
// Mask sequences from AE renders use any name ending in the frame number ("mask_12.png", "000012.png"):
// index every .png in the folder by the digits at the end of its name.
static std::vector<fs::path> mask_list(const fs::path& dir, int n) {
  std::vector<fs::path> v(size_t(std::max(0, n)));
  if (dir.empty() || !fs::is_directory(dir)) return v;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const fs::path p = e.path();
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext != ".png") continue;
    const std::string s = p.stem().string();
    size_t j = s.size();
    while (j > 0 && std::isdigit((unsigned char)s[j - 1])) --j;
    if (j == s.size() || s.size() - j > 9) continue;
    const int i = std::stoi(s.substr(j));
    if (i >= 0 && i < n && v[size_t(i)].empty()) v[size_t(i)] = p;
  }
  return v;
}
static Image box(const Image& in, int r);
// a user mask at analysis size: binarised at half its peak, then grown by r analysis pixels
static Image user_mask(const RGB8& im, int k, int r) {
  Image m = luma_reduced(im, k);
  float mx = 0.f;
  for (float v : m.p) mx = std::max(mx, v);
  for (float& v : m.p) v = mx > 0.f && v > 0.5f * mx ? 1.f : 0.f;
  if (r > 0) { m = box(m, r); for (float& v : m.p) v = v > 0.02f ? 1.f : 0.f; }
  return m;
}
static int count_frames(const fs::path& dir) {
  int n = 0;
  while (fs::exists(frame_path(dir, n))) ++n;
  return n;
}

template <typename F>
static void parallel(int n, int threads, F fn) {
  std::atomic<int> next{0};
  std::vector<std::thread> pool;
  for (int k = 0; k < std::max(1, threads); ++k) pool.emplace_back([&]() { for (int i; (i = next++) < n;) fn(i); });
  for (auto& t : pool) t.join();
}

// ---------------------------------------------------------------- synthetic set for an end-to-end check
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
static float scene(double x, double y, double t) {
  double X = x + 4.0 * t + 16.0 * std::sin(t * 0.35), Y = y + 1.2 * t + 0.04 * t * t;
  float v = 0, a = 0.5f; double f = 1.0 / 48;
  for (int o = 0; o < 4; ++o, a *= 0.5f, f *= 2) v += a * vnoise(X * f + o * 17, Y * f - o * 31);
  return v;
}
static int selftest(const fs::path& dir) {
  const int W = 640, H = 360, N = 24;
  fs::create_directories(dir / "target"); fs::create_directories(dir / "source");
  parallel(N, 8, [&](int i) {
    RGB8 t(W, H), s(W, H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        float v = scene(x, y, i);
        // source: 1.4 frames later in scene time, shifted (+6.4, -4.2) px, darker and tinted
        float u = scene(x - 6.4, y + 4.2, i + 1.4);
        uint8_t* q = &t.p[(size_t(y) * W + x) * 3]; uint8_t* r = &s.p[(size_t(y) * W + x) * 3];
        q[0] = q[1] = q[2] = uint8_t(255 * v);
        r[0] = uint8_t(255 * (0.70f * u + 0.10f)); r[1] = uint8_t(255 * (0.62f * u + 0.10f)); r[2] = uint8_t(255 * (0.55f * u + 0.14f));
      }
    write_png(frame_path(dir / "target", i), t); write_png(frame_path(dir / "source", i), s);
  });
  std::printf("selftest set written to %s: %d frames %dx%d; truth: source time = t - 1.40, shift (+6.4, -4.2) px\n",
              dir.string().c_str(), N, W, H);
  return 0;
}

// ---------------------------------------------------------------- rough static matte (white polygon = ignore)
// warper_cli matte <dir> <W> <H> <frames> <first> <last> "x,y x,y ..."   (pixels; frames outside first..last are black)
static int matte(int argc, char** argv) {
  if (argc < 9) { std::fprintf(stderr, "usage: warper_cli matte <dir> <W> <H> <frames> <first> <last> \"x,y x,y ...\"\n"); return 2; }
  const fs::path dir = argv[2];
  const int W = std::atoi(argv[3]), H = std::atoi(argv[4]), n = std::atoi(argv[5]), first = std::atoi(argv[6]), last = std::atoi(argv[7]);
  std::vector<std::pair<double, double>> poly;
  { const char* p = argv[8]; double x, y; int used = 0;
    while (std::sscanf(p, " %lf,%lf%n", &x, &y, &used) == 2) { poly.push_back({x, y}); p += used; } }
  if (poly.size() < 3 || W < 8 || H < 8 || n < 1) { std::fprintf(stderr, "matte: need a size, a frame count and at least 3 points\n"); return 2; }
  RGB8 black(W, H), white(W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      bool in = false;
      for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].second > y) != (poly[j].second > y) &&
            x < (poly[j].first - poly[i].first) * (y - poly[i].second) / (poly[j].second - poly[i].second) + poly[i].first) in = !in;
      if (in) { uint8_t* q = &white.p[(size_t(y) * W + x) * 3]; q[0] = q[1] = q[2] = 255; }
    }
  fs::create_directories(dir);
  parallel(n, 8, [&](int i) { write_png(frame_path(dir, i), i >= first && i <= last ? white : black); });
  std::printf("matte: %d frames %dx%d in %s, polygon on frames %d..%d\n", n, W, H, dir.string().c_str(), first, last);
  return 0;
}

// ---------------------------------------------------------------- automatic matte for blue-screen plates
static Image box(const Image& in, int r) {
  Image t(in.w, in.h), o(in.w, in.h);
  for (int y = 0; y < in.h; ++y)
    for (int x = 0; x < in.w; ++x) { float s = 0; int c = 0; for (int i = std::max(0, x - r); i <= std::min(in.w - 1, x + r); ++i) { s += in.at(i, y); ++c; } t.at(x, y) = s / c; }
  for (int y = 0; y < in.h; ++y)
    for (int x = 0; x < in.w; ++x) { float s = 0; int c = 0; for (int j = std::max(0, y - r); j <= std::min(in.h - 1, y + r); ++j) { s += t.at(x, j); ++c; } o.at(x, y) = s / c; }
  return o;
}
// 1 = do not measure. Made for a close-up of an arm over a blue-screen set: skin-coloured pixels, and, only when
// skin fills more than 3 % of the frame (a close-up), dark pixels too (sleeve, near-field clutter). Wide frames of
// the dark studio are left alone. Specks and thin things are dropped, then the zone is grown by a margin.
// (The first version masked everything that was not blue or a red marker; on the wide studio frames that was 90 %
// of the picture and wrecked the solve: run r3_a.)
static Image nonblue_mask(const RGB8& im, int k, int grow, bool with_dark = false) {
  Image skin(std::max(1, im.w / k), std::max(1, im.h / k)), dark(skin.w, skin.h);
  size_t nskin = 0;
  for (int y = 0; y < skin.h; ++y)
    for (int x = 0; x < skin.w; ++x) {
      int cs = 0, cd = 0;
      for (int j = 0; j < k; ++j) {
        const uint8_t* p = &im.p[(size_t(y * k + j) * im.w + x * k) * 3];
        for (int i = 0; i < k; ++i, p += 3) {
          const float r = p[0], g = p[1], b = p[2];
          const bool marker = r > 140.f && g < 0.6f * r && b < 0.65f * r;
          if (!marker && r > 110.f && r > 1.08f * g && g >= b && r - b > 35.f) ++cs;
          if (std::max(r, std::max(g, b)) < 75.f) ++cd;
        }
      }
      skin.at(x, y) = cs * 2 > k * k ? 1.f : 0.f; dark.at(x, y) = cd * 2 > k * k ? 1.f : 0.f;
      nskin += cs * 2 > k * k;
    }
  const bool closeup = nskin * 100 > skin.p.size() * 3;
  Image m = skin;
  // with_dark is off by default: while the arm enters, the dark studio is still in frame and masking dark pixels
  // removed most of the set (runs r3_b / r3_c blew up at frames 108, 152-160, 224-228)
  if (!closeup) return Image(skin.w, skin.h, 0.f);
  if (with_dark) for (size_t i = 0; i < m.p.size(); ++i) m.p[i] = std::max(m.p[i], dark.p[i]);
  Image o = box(m, 2);
  for (float& v : o.p) v = v > 0.6f ? 1.f : 0.f;      // opening: drop specks and thin lines
  o = box(o, grow);
  for (float& v : o.p) v = v > 0.02f ? 1.f : 0.f;     // grow by the margin
  return o;
}

// ---------------------------------------------------------------- talent mask from a first solve (sprint 14)
// warper_cli talentmask --target <dir> --aligned <dir> --out <dir> [--grow 0.025] [--thresh 4] [--min 0.08]
// Per frame: exposure-matched difference between the target and the aligned source (a first solve). Where the takes
// differ (the talent of EITHER take shows up there, in target space) is masked: threshold, opening to drop specks,
// then grown by --grow of the frame width. White = ignore, full resolution, for `align --ignore`. Because it is in
// TARGET space it covers both people: the source's talent appears at its aligned position. No colour rules.
static int talentmask(std::map<std::string, std::string>& a) {
  const fs::path tdir = a["target"], adir = a["aligned"], out = a["out"];
  if (tdir.empty() || adir.empty() || out.empty()) { std::fprintf(stderr, "need --target, --aligned and --out\n"); return 2; }
  const double growf = a.count("grow") ? std::stod(a["grow"]) : 0.025, kth = a.count("thresh") ? std::stod(a["thresh"]) : 6.0;
  const double minth = a.count("min") ? std::stod(a["min"]) : 0.12;
  const int n = std::min(count_frames(tdir), count_frames(adir));
  if (n < 1) { std::fprintf(stderr, "no frames\n"); return 2; }
  fs::create_directories(out); fs::create_directories(out / "preview");
  std::atomic<int> bad{0}; std::vector<double> frac(n, 0.0);
  parallel(n, 16, [&](int i) {
    RGB8 T, A;
    if (!read_png(frame_path(tdir, i), T) || !read_png(frame_path(adir, i), A) || T.w != A.w || T.h != A.h) { ++bad; return; }
    const int k = std::max(1, T.w / 640);
    Image t = luma_reduced(T, k), s = luma_reduced(A, k);
    // robust gain/offset: least squares, then again without the worst residuals
    double g = 1, o = 0;
    for (int round = 0; round < 3; ++round) {
      std::vector<float> r;
      if (round) for (size_t q = 0; q < t.p.size(); ++q) r.push_back(std::fabs(float(g * s.p[q] + o - t.p[q])));
      float cut = 1e9f;
      if (round) { std::vector<float> rr(r); std::nth_element(rr.begin(), rr.begin() + rr.size() * 7 / 10, rr.end()); cut = 2.f * rr[rr.size() * 7 / 10] + 1e-4f; }
      double n0 = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
      for (size_t q = 0; q < t.p.size(); ++q) {
        if (s.p[q] <= 0.002f) continue;                    // black = no source coverage
        if (round && r[q] > cut) continue;
        n0 += 1; sx += s.p[q]; sy += t.p[q]; sxx += double(s.p[q]) * s.p[q]; sxy += double(s.p[q]) * t.p[q];
      }
      const double den = n0 * sxx - sx * sx;
      if (n0 < 100 || den <= 1e-12) break;
      g = std::min(std::max((n0 * sxy - sx * sy) / den, 0.3), 3.0); o = (sy - g * sx) / n0;
    }
    Image d(t.w, t.h);
    for (size_t q = 0; q < t.p.size(); ++q) d.p[q] = std::fabs(float(g * s.p[q] + o - t.p[q]));
    d = box(d, 1);
    std::vector<float> v(d.p); std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    const float thr = std::max(float(minth), float(kth) * v[v.size() / 2]);
    Image m(d.w, d.h);
    for (size_t q = 0; q < d.p.size(); ++q) m.p[q] = d.p[q] > thr ? 1.f : 0.f;
    // plus skin in EITHER take (target, and the aligned source, which is already in target space): the difference
    // misses talent where both takes have a person at nearly the same place (arms laid over each other at f220)
    auto skin_into = [&](const RGB8& im) {
      for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x) {
          int cs = 0;
          for (int j = 0; j < k; ++j) {
            const uint8_t* p = &im.p[(size_t(y * k + j) * im.w + x * k) * 3];
            for (int i2 = 0; i2 < k; ++i2, p += 3) {
              const float r = p[0], g2 = p[1], b = p[2];
              const bool marker = r > 140.f && g2 < 0.6f * r && b < 0.65f * r;
              if (!marker && r > 110.f && r > 1.08f * g2 && g2 >= b && r - b > 35.f) ++cs;
            }
          }
          if (cs * 2 > k * k) m.at(x, y) = 1.f;
        }
    };
    if (!a.count("skin") || a["skin"] != "0") { skin_into(T); skin_into(A); }
    const int orad = std::max(2, d.w / 160);              // opening: blobs thinner than about 1 % of the width go
    Image e = box(m, orad); for (float& x : e.p) x = x > 0.85f ? 1.f : 0.f;
    const int grow = std::max(orad, int(growf * d.w + 0.5)) + orad;
    e = box(e, grow); for (float& x : e.p) x = x > 0.01f ? 1.f : 0.f;
    size_t c = 0; for (float x : e.p) c += x > 0.5f; frac[i] = double(c) / e.p.size();
    RGB8 M(T.w, T.h);
    for (int y = 0; y < T.h; ++y)
      for (int x = 0; x < T.w; ++x) {
        const uint8_t v8 = e.at(std::min(x / k, e.w - 1), std::min(y / k, e.h - 1)) > 0.5f ? 255 : 0;
        uint8_t* q = &M.p[(size_t(y) * T.w + x) * 3]; q[0] = q[1] = q[2] = v8;
      }
    if (!write_png(frame_path(out, i), M)) ++bad;
    if (i % 20 == 0 || i == 116 || i == 160 || i == 220) {   // preview: target at half size, masked zone tinted red
      RGB8 P(T.w / 2, T.h / 2);
      for (int y = 0; y < P.h; ++y)
        for (int x = 0; x < P.w; ++x) {
          const uint8_t* src = &T.p[(size_t(2 * y) * T.w + 2 * x) * 3]; uint8_t* q = &P.p[(size_t(y) * P.w + x) * 3];
          const bool ig = e.at(std::min(2 * x / k, e.w - 1), std::min(2 * y / k, e.h - 1)) > 0.5f;
          q[0] = ig ? uint8_t(std::min(255, src[0] / 2 + 128)) : src[0]; q[1] = ig ? src[1] / 2 : src[1]; q[2] = ig ? src[2] / 2 : src[2];
        }
      write_png(out / "preview" / frame_path("", i).filename(), P);
    }
  });
  std::printf("talentmask: %d frames to %s; masked fraction every 20 frames:", n, out.string().c_str());
  for (int i = 0; i < n; i += 20) std::printf(" %.2f", frac[i]);
  std::printf("%s\n", bad ? "  (ERRORS)" : "");
  return bad ? 1 : 0;
}

// ---------------------------------------------------------------- align
static int align(std::map<std::string, std::string>& a) {
  typedef std::chrono::steady_clock Clock;
  auto secs = [](Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); };
  const fs::path tdir = a["target"], sdir = a["source"], out = a["out"];
  if (tdir.empty() || sdir.empty() || out.empty()) { std::fprintf(stderr, "need --target, --source and --out\n"); return 2; }
  const int nT = count_frames(tdir), nS = count_frames(sdir);
  if (nT < 4 || nS < 4) { std::fprintf(stderr, "need at least 4 frames named 000000.png ... (target %d, source %d)\n", nT, nS); return 2; }
  const int n = std::min(nT, nS);
  // --source-frames N (0.3.3): only source frames 0..N-1 are used, like a Source Layer out point in After Effects
  // (the target keeps all its frames; where it needs later source frames the solve holds the last one).
  const int nSu = a.count("source-frames") ? std::max(4, std::min(n, std::stoi(a["source-frames"]))) : n;
  const int range = a.count("range") ? std::stoi(a["range"]) : 3;
  const bool auto_ignore = !a.count("auto-ignore") || std::stoi(a["auto-ignore"]) != 0;
  const int awidth = a.count("analysis-width") ? std::stoi(a["analysis-width"]) : 1536;
  const std::string mode = a.count("mode") ? a["mode"] : "both";
  unsigned hc = std::thread::hardware_concurrency();
  const int threads = a.count("threads") ? std::stoi(a["threads"]) : int(std::max(1u, hc > 2 ? hc - 2 : 1u));
  const bool have_ignore = a.count("ignore") != 0;
  double rg[4] = {0, 0, 1, 1};
  const bool region = a.count("region") && std::sscanf(a["region"].c_str(), "%lf,%lf,%lf,%lf", &rg[0], &rg[1], &rg[2], &rg[3]) == 4;

  // 1) load: reduced luma of every frame (the full-resolution frames are read again at render time)
  RGB8 first;
  if (!read_png(frame_path(tdir, 0), first)) { std::fprintf(stderr, "cannot read %s\n", frame_path(tdir, 0).string().c_str()); return 2; }
  const int W = first.w, H = first.h;
  const int k = std::max(1, (W + awidth - 1) / awidth), aw = std::max(1, W / k), ah = std::max(1, H / k);
  // --auto-matte 1: per-frame matte of everything that is not blue screen or a red marker, for BOTH takes
  const bool auto_matte = a.count("auto-matte") && std::stoi(a["auto-matte"]) != 0;
  const int grow = a.count("matte-grow") ? std::stoi(a["matte-grow"]) : std::max(3, W / k / 80);
  // --ignore / --ignore-source <dir>: user mask sequences (white = talent, do not measure), any name ending in the
  // frame number; grown by --mask-grow full-resolution pixels (default 24)
  const bool have_ignore_src = a.count("ignore-source") != 0;
  const auto ign_t = mask_list(have_ignore ? fs::path(a["ignore"]) : fs::path(), n);
  const auto ign_s = mask_list(have_ignore_src ? fs::path(a["ignore-source"]) : fs::path(), n);
  const int mgrow = std::max(0, (a.count("mask-grow") ? std::stoi(a["mask-grow"]) : 24) / k);
  int n_ign_t = 0, n_ign_s = 0;
  for (const auto& p : ign_t) n_ign_t += !p.empty();
  for (const auto& p : ign_s) n_ign_s += !p.empty();
  if (have_ignore || have_ignore_src) std::printf("user masks: target %d / source %d of %d frames\n", n_ign_t, n_ign_s, n);
  std::vector<Image> T(n), S(n), MK(have_ignore || region || auto_matte ? n : 0), MKS(auto_matte || have_ignore_src ? n : 0);
  std::atomic<int> bad{0};
  auto t0 = Clock::now();
  parallel(n, threads, [&](int i) {
    RGB8 im;
    Image am;
    if (read_png(frame_path(tdir, i), im) && im.w == W && im.h == H) { T[i] = luma_reduced(im, k); if (auto_matte) am = nonblue_mask(im, k, grow); } else { T[i] = Image(aw, ah, 0.f); ++bad; }
    if (read_png(frame_path(sdir, i), im) && im.w == W && im.h == H) { S[i] = luma_reduced(im, k); if (auto_matte) MKS[i] = nonblue_mask(im, k, grow); } else { S[i] = Image(aw, ah, 0.f); ++bad; if (auto_matte) MKS[i] = Image(aw, ah, 0.f); }
    if (!MKS.empty()) {
      if (MKS[i].w != aw || MKS[i].h != ah) MKS[i] = Image(aw, ah, 0.f);
      if (!ign_s[i].empty() && read_png(ign_s[i], im) && im.w == W && im.h == H) {
        Image mm = user_mask(im, k, mgrow);
        for (size_t q = 0; q < mm.p.size(); ++q) MKS[i].p[q] = std::max(MKS[i].p[q], mm.p[q]);
      }
    }
    if (!MK.empty()) {
      MK[i] = am.w == aw && am.h == ah ? am : Image(aw, ah, 0.f);
      if (!ign_t[i].empty() && read_png(ign_t[i], im) && im.w == W && im.h == H) {   // white = ignore
        Image mm = user_mask(im, k, mgrow);
        for (size_t q = 0; q < mm.p.size(); ++q) MK[i].p[q] = std::max(MK[i].p[q], mm.p[q]);
      }
      if (region)
        for (int y = 0; y < ah; ++y)
          for (int x = 0; x < aw; ++x)
            if (x < rg[0] * aw || x > rg[2] * aw || y < rg[1] * ah || y > rg[3] * ah) MK[i].at(x, y) = 1.f;
    }
  });
  const double load_s = secs(t0);
  if (bad) std::fprintf(stderr, "warning: %d frames could not be read or had another size (treated as black)\n", int(bad));

  // 2) solve: exactly what the effect's Analyze calls
  ShotOptions opt;
  opt.range = range;
  opt.space.levels = aw >= 1024 ? 6 : 5;
  opt.threads = threads;
  opt.auto_ignore = auto_ignore;
  if (!MK.empty()) opt.ignore = [&](int t) -> const Image* { return &MK[t]; };
  if (!MKS.empty()) opt.ignore_source = [&](int t) -> const Image* { return &MKS[t]; };
  if (a.count("grid-smooth")) opt.grid_smooth = std::stod(a["grid-smooth"]);
  if (a.count("tension")) opt.tension_pass = std::stoi(a["tension"]) != 0;            // tension pass on the field
  if (a.count("tension-k")) opt.tension_k = std::stod(a["tension-k"]);
  if (a.count("tension-radius")) opt.tension_radius = std::stoi(a["tension-radius"]);
  if (a.count("tension-iters")) opt.tension_iters = std::stoi(a["tension-iters"]);
  if (a.count("dof-range")) std::sscanf(a["dof-range"].c_str(), "%d,%d,%d", &opt.dof_range_lo, &opt.dof_range_hi, &opt.dof_range_dof);   // first,last,dof
  if (a.count("time-trust-end")) opt.time_trust_end = std::stoi(a["time-trust-end"]);   // frames after it are held near the trusted offset
  if (a.count("time-window")) opt.time_window = std::stod(a["time-window"]);
  if (a.count("prior-persp")) opt.prior_persp = std::stod(a["prior-persp"]);           // soft prior on the perspective terms
  if (a.count("prior-lin")) opt.prior_lin = std::stod(a["prior-lin"]);                 // and on scale / rotation
  if (a.count("dof")) opt.space.dof = std::stoi(a["dof"]);   // 8 = homography (default), 6 = affine, 2 = translation
  if (a.count("temporal-refit")) opt.temporal_refit = std::stoi(a["temporal-refit"]) != 0;
  if (a.count("temporal-tol")) opt.temporal_tol = std::stod(a["temporal-tol"]);
  if (a.count("temporal-radius")) opt.temporal_radius = std::stoi(a["temporal-radius"]);
  if (a.count("grid-time-median")) opt.grid_time_median = std::stoi(a["grid-time-median"]);
  if (a.count("max-slope")) opt.max_slope = std::stod(a["max-slope"]);
  // --edges extend (default: repeat the source's border pixels) | black (nothing where the source has no pixels)
  // | target (the target frame where the source has no pixels). A coverage mask per frame (white = the source had
  // pixels there) is always written to <out>/coverage/.
  const bool edges_extend = !a.count("edges") || a["edges"] != "black";
  const bool edges_target = a.count("edges") && a["edges"] == "target";
  // --space-amount / --time-amount 0..100 (%), default 100: how far the source is conformed (0.3.2)
  const double space_amt = a.count("space-amount") ? std::stod(a["space-amount"]) / 100.0 : 1.0;
  const double time_amt = a.count("time-amount") ? std::stod(a["time-amount"]) / 100.0 : 1.0;
  const bool blend = !a.count("blend") || std::stoi(a["blend"]) != 0;   // --blend 0: nearest whole source frame only (sharper)
  // --time-parallax 1 (default): time from the residual after per-candidate homographies (moves with depth)
  opt.time_parallax = !a.count("time-parallax") || std::stoi(a["time-parallax"]) != 0;
  if (a.count("parallax-weight")) opt.parallax_weight = std::stod(a["parallax-weight"]);
  if (a.count("motion-weight")) opt.motion_weight = std::stod(a["motion-weight"]);
  if (a.count("jump-cost")) opt.parallax_jump_cost = std::stod(a["jump-cost"]);
  if (a.count("wide-search")) opt.wide_search = std::stoi(a["wide-search"]) != 0;
  if (a.count("time-smooth")) opt.time_smooth = std::stod(a["time-smooth"]);
  int gw = 0, gh = 0;   // --grid 32x18: parallax residual field (local warp); off by default
  if (a.count("grid") && std::sscanf(a["grid"].c_str(), "%dx%d", &gw, &gh) == 2 && gw > 1 && gh > 1) { opt.grid_w = gw; opt.grid_h = gh; }
  else gw = gh = 0;
  auto t1 = Clock::now();
  ShotResult R = align_shot(n, nSu, [&](int i) -> const Image& { return T[i]; }, [&](int i) -> const Image& { return S[std::min(i, nSu - 1)]; }, opt);
  if (R.held_start + R.held_end > 0) std::printf("held the first source frame for %d frames, the last for %d frames\n", R.held_start, R.held_end);
  const double solve_s = secs(t1);
  if (mode == "space") for (int t = 0; t < n; ++t) R.src_time[t] = t;
  if (mode == "time") for (int t = 0; t < n; ++t) R.Hf[t] = H3::identity();

  // 3) render: source conformed to the target, edges extended, two-frame blend at sub-frame times
  // --refine 1: per-pixel refinement after the field (dense residual flow, edge-aware, gated by the residual)
  std::vector<PreFlow> RF;
  double refine_s = 0;
  if (a.count("refine") && std::stoi(a["refine"]) != 0 && !R.Ha.empty() && mode == "both") {
    const auto tr = Clock::now();
    RF.resize(size_t(n));
    const int rad = a.count("refine-radius") ? std::stoi(a["refine-radius"]) : 3;
    const double rgain = a.count("refine-gain") ? std::stod(a["refine-gain"]) : 0.8;
    const double rmin = a.count("refine-min") ? std::stod(a["refine-min"]) : 0.0;
    parallel(n, threads, [&](int t) {
      const w4d::SourcePick pk = w4d::pick_source(t, R.src_time[t], 0, nSu - 1, 1.0, 1.0, false);
      const bool g2 = gw && !R.grid_a.empty();
      RF[size_t(t)] = refine_flow(T[t], S[pk.a], from_normalized(pk.a_is_hb ? R.Hb[t] : R.Ha[t], aw, ah),
                                  g2 ? (pk.a_is_hb ? &R.grid_b[t] : &R.grid_a[t]) : nullptr, gw, gh,
                                  MK.empty() ? nullptr : &MK[t], MKS.empty() ? nullptr : &MKS[pk.a], k, rad, 0.06, rgain, rmin);
    });
    refine_s = secs(tr);
    double mx = 0, sum = 0; size_t cnt = 0, nz = 0;
    for (const PreFlow& f : RF) for (size_t i = 0; i + 1 < f.d.size(); i += 2) { const double m = std::hypot(f.d[i], f.d[i + 1]); mx = std::max(mx, m); sum += m; ++cnt; nz += m > 0.25; }
    std::printf("refine: %.1f s; flow over %.1f %% of the blocks (> 0.25 px), mean %.3f px, max %.1f px (output pixels)\n", refine_s, 100.0 * nz / std::max<size_t>(cnt, 1), sum / std::max<size_t>(cnt, 1), mx);
  }
  fs::create_directories(out / "aligned"); fs::create_directories(out / "coverage");
  auto t2 = Clock::now();
  std::atomic<int> wbad{0};
  parallel(n, threads, [&](int t) {
    if (!R.Ha.empty() && mode == "both") {   // 0.3.2 path: shared frame pick + Amounts (same as the effect)
      std::map<int, RGB8> cache;
      std::function<const RGB8&(int)> frames = [&](int i) -> const RGB8& {
        auto it = cache.find(i);
        if (it == cache.end()) { RGB8 im; if (!read_png(frame_path(sdir, i), im)) { ++wbad; im = RGB8(W, H); } it = cache.emplace(i, std::move(im)).first; }
        return it->second;
      };
      const w4d::SourcePick pk = w4d::pick_source(t, R.src_time[t], 0, nSu - 1, time_amt, space_amt, blend);
      RGB8 Tfull, cov; const bool haveT = edges_target && read_png(frame_path(tdir, t), Tfull);
      const bool g2 = gw && !R.grid_a.empty();
      RGB8 o = render_amounts(pk, frames, t, from_normalized(R.Ha[t], W, H), from_normalized(R.Hb[t], W, H),
                              g2 ? &R.grid_a[t] : nullptr, g2 ? &R.grid_b[t] : nullptr, gw, gh, W, H, edges_extend,
                              edges_target && haveT ? &Tfull : nullptr, &cov, RF.empty() || blend || time_amt < 1.0 ? nullptr : &RF[size_t(t)]);
      if (!write_png(frame_path(out / "aligned", t), o)) ++wbad;
      if (!write_png(frame_path(out / "coverage", t), cov)) ++wbad;
      return;
    }
    double s = std::min(std::max(R.src_time[t], 0.0), double(n - 1));
    int a0 = std::min(int(std::floor(s)), n - 1); float f = float(s - a0);
    RGB8 A, B; bool useB = f > 0.002f && a0 + 1 < n;
    bool second = false;                       // --blend 0: take only the nearer of the two frames, with its own H and field
    if (!blend) { second = f >= 0.5f && a0 + 1 < n; useB = false; if (second) ++a0; }
    if (!read_png(frame_path(sdir, a0), A)) { ++wbad; return; }
    if (useB && !read_png(frame_path(sdir, a0 + 1), B)) useB = false;
    const bool two = !R.Ha.empty() && mode == "both";   // each source frame through its own homography
    H3 HA = from_normalized(two ? (second ? R.Hb[t] : R.Ha[t]) : R.Hf[t], W, H), HBm = from_normalized(two ? R.Hb[t] : R.Hf[t], W, H);
    const bool twog = two && gw && !R.grid_a.empty();
    RGB8 Tfull, cov; const bool haveT = edges_target && read_png(frame_path(tdir, t), Tfull);
    RGB8 o = warp_blend(A, useB ? &B : nullptr, f, HA, W, H, gw && mode != "time" ? (twog ? (second ? &R.grid_b[t] : &R.grid_a[t]) : &R.grid[t]) : nullptr, gw, gh,
                        two ? &HBm : nullptr, twog ? &R.grid_b[t] : nullptr, edges_extend, edges_target && haveT ? &Tfull : nullptr, &cov);
    if (!write_png(frame_path(out / "aligned", t), o)) ++wbad;
    if (!write_png(frame_path(out / "coverage", t), cov)) ++wbad;
  });
  const double render_s = secs(t2);
  if (!R.tension.empty() && gw) {   // tension maps to look at: one 20 px block per cell; red = flagged, grey = score, blue = no set pixels
    fs::create_directories(out / "tension");
    long tf = 0, tx = 0; for (int v : R.tension_flagged) tf += v; for (int v : R.tension_fixed) tx += v;
    std::printf("tension pass: %ld cells flagged over the shot (%.1f %% of %d x %d x %d), %ld improved by the local search\n",
                tf, 100.0 * tf / (double(n) * gw * gh), n, gw, gh, tx);
    for (int t = 0; t < n; ++t) {
      if (!(t % 20 == 0 || t == 116 || t == 132 || t == 148 || t == 160)) continue;
      const int B = 20; RGB8 img(gw * B, gh * B);
      for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
          const float sc = R.tension[t][size_t(y / B) * gw + x / B]; uint8_t* q = &img.p[(size_t(y) * img.w + x) * 3];
          if (sc < 0) { q[0] = 20; q[1] = 30; q[2] = 90; }
          else if (sc > 1) { q[0] = 255; q[1] = uint8_t(std::max(0.f, 200 - 60 * (sc - 1))); q[2] = 40; }
          else { q[0] = q[1] = q[2] = uint8_t(40 + 160 * sc); }
        }
      write_png(frame_path(out / "tension", t), img);
    }
  }
  if (auto_matte) {   // a few mattes to look at: target and source, analysis size
    fs::create_directories(out / "matte_debug");
    for (int t = 0; t < n; t += 20)
      for (int which = 0; which < 2; ++which) {
        const Image& m = which ? MKS[t] : MK[t]; RGB8 g(m.w, m.h);
        for (size_t q = 0; q < m.p.size(); ++q) g.p[q * 3] = g.p[q * 3 + 1] = g.p[q * 3 + 2] = m.p[q] > 0.5f ? 255 : 0;
        char nm[40]; std::snprintf(nm, sizeof(nm), "%s_%06d.png", which ? "source" : "target", t);
        write_png(out / "matte_debug" / nm, g);
      }
  }

  // 4) records.json and log.txt
  std::vector<double> off(n);
  for (int t = 0; t < n; ++t) off[t] = R.src_time[t] - t;
  std::vector<double> so = off; std::sort(so.begin(), so.end());
  if (FILE* f = std::fopen((out / "records.json").string().c_str(), "w")) {
    std::fprintf(f, "{\n  \"version\": 1,\n  \"frames\": %d,\n  \"size\": [%d, %d],\n  \"analysis_size\": [%d, %d],\n  \"coarse_offset\": %d,\n"
                    "  \"note\": \"H maps target to source in normalised coordinates u=(x-(w-1)/2)/(w/2), v=(y-(h-1)/2)/(w/2); src_time in frames\",\n  \"records\": [\n",
                 n, W, H, aw, ah, R.coarse_offset);
    for (int t = 0; t < n; ++t) {
      double ig = 0;
      if (!MK.empty()) { size_t c = 0; for (float v : MK[t].p) c += v > 0.5f; ig = double(c) / MK[t].p.size(); }
      const double* m = R.Hf[t].m;
      std::fprintf(f, "    {\"frame\": %d, \"temporal_fixed\": %d, \"src_time\": %.5f, \"offset\": %.5f, \"ignored_fraction\": %.4f, \"H\": [%.10g, %.10g, %.10g, %.10g, %.10g, %.10g, %.10g, %.10g, %.10g]",
                   t, t < int(R.temporal_fixed.size()) ? R.temporal_fixed[t] : 0, R.src_time[t], off[t], ig, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
      if (gw) {   // residual field: gw*gh pairs (du, dv), row-major, normalised units; plus its largest length in pixels
        double mx = 0;
        for (size_t i = 0; i + 1 < R.grid[t].size(); i += 2) mx = std::max(mx, std::hypot(R.grid[t][i], R.grid[t][i + 1]) * W * 0.5);
        std::fprintf(f, ", \"grid\": [%d, %d], \"field_max_px\": %.3f, \"field\": [", gw, gh, mx);
        for (size_t i = 0; i < R.grid[t].size(); ++i) std::fprintf(f, "%s%.6g", i ? "," : "", R.grid[t][i]);
        std::fprintf(f, "]");
      }
      std::fprintf(f, "}%s\n", t + 1 < n ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
  }
  if (!R.time_scores.empty())   // scores.csv: residual score of every candidate offset per frame (lower = better)
    if (FILE* f = std::fopen((out / "scores.csv").string().c_str(), "w")) {
      std::fprintf(f, "frame,offset_final");
      for (int oo = -range; oo <= range; ++oo) std::fprintf(f, ",o%+d", oo);
      std::fputc('\n', f);
      for (int t = 0; t < n; ++t) {
        std::fprintf(f, "%d,%.3f", t, off[t]);
        for (double v : R.time_scores[t]) std::fprintf(f, ",%.5f", v);
        std::fputc('\n', f);
      }
      std::fclose(f);
    }
  char line[1024];
  std::time_t now = std::time(nullptr); char stamp[32] = "";
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  std::snprintf(line, sizeof(line),
                "%s  warper_cli  frames %d  layer %dx%d  analysis %dx%d (1/%d)  threads %d  load %.2f s (%.3f s/frame)  "
                "solve %.2f s (%.3f s/frame)  render %.2f s (%.3f s/frame)  total %.2f s  coarse offset %d  "
                "final offset min %.3f median %.3f max %.3f fr  range +-%d  ignore mattes %s  region %s  auto ignore %s  mode %s  grid %dx%d  time-parallax %d (w %.3g, motion w %.3g, smooth %.3g)  %s",
                stamp, n, W, H, aw, ah, k, threads, load_s, load_s / n, solve_s, solve_s / n, render_s, render_s / n,
                load_s + solve_s + render_s, R.coarse_offset, so.front(), so[n / 2], so.back(), range,
                have_ignore ? (auto_matte ? "on+auto" : "on") : (auto_matte ? "auto (close-up skin)" : "none"), region ? "on" : "full frame", auto_ignore ? "on" : "off", mode.c_str(), gw, gh,
                int(opt.time_parallax), opt.parallax_weight, opt.motion_weight, opt.time_smooth,
                wbad ? "WRITE ERRORS" : "done");
  std::printf("%s\n", line);
  if (FILE* f = std::fopen((out / "log.txt").string().c_str(), "a")) { std::fprintf(f, "%s\n", line); std::fclose(f); }
  return wbad ? 1 : 0;
}

int main(int argc, char** argv) {
  Gdiplus::GdiplusStartupInput gsi; ULONG_PTR tok = 0;
  if (Gdiplus::GdiplusStartup(&tok, &gsi, nullptr) != Gdiplus::Ok) { std::fprintf(stderr, "GDI+ failed to start\n"); return 2; }
  int rc = 2;
  if (argc >= 3 && std::string(argv[1]) == "selftest") rc = selftest(argv[2]);
  else if (argc >= 2 && std::string(argv[1]) == "matte") rc = matte(argc, argv);
  else if (argc >= 2 && std::string(argv[1]) == "talentmask") {
    std::map<std::string, std::string> a;
    for (int i = 2; i + 1 < argc; i += 2) { std::string key = argv[i]; if (key.rfind("--", 0) == 0) a[key.substr(2)] = argv[i + 1]; }
    rc = talentmask(a);
  }
  else if (argc >= 2 && std::string(argv[1]) == "align") {
    std::map<std::string, std::string> a;
    for (int i = 2; i + 1 < argc; i += 2) { std::string key = argv[i]; if (key.rfind("--", 0) == 0) a[key.substr(2)] = argv[i + 1]; }
    rc = align(a);
  } else
    std::fprintf(stderr, "usage:\n  warper_cli align --target <dir> --source <dir> [--ignore <dir>] [--region x0,y0,x1,y1] [--range 8]\n"
                         "                   [--auto-ignore 1] [--analysis-width 1536] [--mode both|space|time] [--threads N] --out <dir>\n"
                         "  warper_cli selftest <dir>\n");
  Gdiplus::GdiplusShutdown(tok);
  return rc;
}
