// jd_exrc image input (EXR single/multi-part, PNG 8/16-bit), sequences, logging.
#include "exrc.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>

#include <half.h>
#include <tiffio.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <regex>
#include <set>

namespace fs = std::filesystem;

namespace jdexrc {

// ---------------------------------------------------------------- logging
static bool g_json = false;
static std::mutex g_log_mutex;
static std::FILE* g_log_file = nullptr;  // --log-file: every line also goes here (flushed per line)
void log_set_json(bool on) { g_json = on; }
bool log_is_json() { return g_json; }
bool log_set_file(const std::string& path) {
    if (g_log_file) std::fclose(g_log_file);
#ifdef _WIN32
    g_log_file = _wfopen(fs::u8path(path).wstring().c_str(), L"wb");  // UTF-8 path, truncate
#else
    g_log_file = std::fopen(path.c_str(), "wb");
#endif
    return g_log_file != nullptr;
}
void log_raw(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
    if (g_log_file) { std::fputs(text.c_str(), g_log_file); std::fflush(g_log_file); }
}
void log_line(const std::string& event, const std::string& msg, int frame, const std::string& file) {
    std::string s;
    if (g_json) {
        s = "{\"event\":\"" + json_escape(event) + "\"";
        if (frame != INT32_MIN) s += ",\"frame\":" + std::to_string(frame);
        if (!file.empty()) s += ",\"file\":\"" + json_escape(file) + "\"";
        if (!msg.empty()) s += ",\"msg\":\"" + json_escape(msg) + "\"";
        s += "}\n";
    } else {
        s = event;
        if (frame != INT32_MIN) s += " " + std::to_string(frame);
        if (!file.empty()) s += " " + file;
        if (!msg.empty()) s += ": " + msg;
        s += "\n";
    }
    log_raw(s);
}

// ---------------------------------------------------------------- helpers
static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static fs::path u8(const std::string& s) { return fs::u8path(s); }

void blit(const Plane& src, const Imath::Box2i& d, std::vector<float>& dst) {
    const int dw = d.max.x - d.min.x + 1;
    const int y0 = std::max(src.dw.min.y, d.min.y), y1 = std::min(src.dw.max.y, d.max.y);
    const int x0 = std::max(src.dw.min.x, d.min.x), x1 = std::min(src.dw.max.x, d.max.x);
    if (x0 > x1) return;
    for (int y = y0; y <= y1; ++y) {
        const float* s = &src.px[size_t(y - src.dw.min.y) * src.w() + size_t(x0 - src.dw.min.x)];
        std::copy(s, s + (x1 - x0 + 1), &dst[size_t(y - d.min.y) * dw + size_t(x0 - d.min.x)]);
    }
}

// ---------------------------------------------------------------- Source
std::string Source::resolve(const std::string& layer, const std::string& ch) const {
    static const std::map<std::string, std::string> LONG = {{"r", "red"}, {"g", "green"}, {"b", "blue"}, {"a", "alpha"}};
    const std::string pre = layer.empty() ? "" : lower(layer) + ".";
    std::vector<std::string> want = {pre + lower(ch)};
    auto it = LONG.find(lower(ch));
    if (it != LONG.end()) want.push_back(pre + it->second);
    if (lower(ch) == "r") want.push_back(pre + "y");
    if (lower(ch) == "y") { want.push_back(pre + "r"); want.push_back(pre + "red"); }
    for (const auto& w : want)
        for (const auto& c : channels)
            if (lower(c) == w) return c;
    // A lone channel answers R or Y (a single-channel matte with an unusual name).
    if (layer.empty() && channels.size() == 1 && (lower(ch) == "r" || lower(ch) == "y")) return channels[0];
    return "";
}

bool Source::read_spec(const std::string& layer, const std::string& ch, Plane& out, std::string& err) {
    if (ch == "L" || ch == "l") {  // Rec.709 luma
        Plane r, g, b;
        const std::string cr = resolve(layer, "R"), cg = resolve(layer, "G"), cb = resolve(layer, "B");
        if (cr.empty() || cg.empty() || cb.empty() || cr == cg) {  // grey image: luma is the one channel
            const std::string y = resolve(layer, "Y");
            if (y.empty()) { err = "no R/G/B or Y channel for luma in " + path; return false; }
            return read(y, out, err);
        }
        if (!read(cr, r, err) || !read(cg, g, err) || !read(cb, b, err)) return false;
        out = r;
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = 0.2126f * r.px[i] + 0.7152f * g.px[i] + 0.0722f * b.px[i];
        return true;
    }
    const std::string real = resolve(layer, ch);
    if (real.empty()) {
        err = "channel '" + (layer.empty() ? ch : layer + "." + ch) + "' not found in " + path;
        return false;
    }
    return read(real, out, err);
}

namespace {

class ExrSource : public Source {
public:
    std::unique_ptr<Imf::MultiPartInputFile> file;
    std::map<std::string, int> part_of;
    std::mutex m;
    bool read(const std::string& channel, Plane& out, std::string& err) override {
        std::lock_guard<std::mutex> lock(m);
        try {
            auto it = part_of.find(channel);
            if (it == part_of.end()) { err = "no channel " + channel + " in " + path; return false; }
            Imf::InputPart part(*file, it->second);
            out.dw = part.header().dataWindow();
            out.px.assign(size_t(out.w()) * out.h(), 0.f);
            Imf::FrameBuffer fb;
            char* base = reinterpret_cast<char*>(out.px.data()) -
                         (ptrdiff_t(out.dw.min.x) + ptrdiff_t(out.dw.min.y) * out.w()) * ptrdiff_t(sizeof(float));
            fb.insert(channel.c_str(), Imf::Slice(Imf::FLOAT, base, sizeof(float), sizeof(float) * size_t(out.w())));
            part.setFrameBuffer(fb);
            part.readPixels(out.dw.min.y, out.dw.max.y);
            return true;
        } catch (const std::exception& e) { err = path + ": " + e.what(); return false; }
    }
};

class PngSource : public Source {
public:
    int w = 0, h = 0, n = 0;
    std::vector<float> data;  // interleaved, 0-1
    bool read(const std::string& channel, Plane& out, std::string& err) override {
        auto it = std::find(channels.begin(), channels.end(), channel);
        if (it == channels.end()) { err = "no channel " + channel + " in " + path; return false; }
        const int c = int(it - channels.begin());
        out.dw = dw;
        out.px.resize(size_t(w) * h);
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = data[i * n + c];
        return true;
    }
};

// TIFF: 8/16-bit unsigned, 16/32-bit float, contiguous or planar, strips or tiles. Values: uint / max, float as is.
class TiffSource : public Source {
public:
    int w = 0, h = 0, n = 0;
    std::vector<float> data;  // interleaved
    bool read(const std::string& channel, Plane& out, std::string& err) override {
        auto it = std::find(channels.begin(), channels.end(), channel);
        if (it == channels.end()) { err = "no channel " + channel + " in " + path; return false; }
        const int c = int(it - channels.begin());
        out.dw = dw;
        out.px.resize(size_t(w) * h);
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = data[i * n + c];
        return true;
    }
    bool load(std::string& err) {
#ifdef _WIN32
        TIFF* t = TIFFOpenW(fs::u8path(path).wstring().c_str(), "r");
#else
        TIFF* t = TIFFOpen(path.c_str(), "r");
#endif
        if (!t) { err = "cannot open TIFF " + path; return false; }
        struct Closer { TIFF* t; ~Closer() { TIFFClose(t); } } closer{t};
        uint32_t W = 0, H = 0;
        uint16_t spp = 1, bps = 8, fmt = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG;
        TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &W);
        TIFFGetField(t, TIFFTAG_IMAGELENGTH, &H);
        TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
        TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &fmt);
        TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
        const bool is_float = fmt == SAMPLEFORMAT_IEEEFP;
        if (!((fmt == SAMPLEFORMAT_UINT && (bps == 8 || bps == 16)) || (is_float && (bps == 16 || bps == 32)))) {
            err = path + ": TIFF with " + std::to_string(bps) + "-bit " + (is_float ? "float" : "integer") +
                  " samples is not supported (8/16-bit integer, 16/32-bit float)";
            return false;
        }
        w = int(W); h = int(H); n = int(spp);
        data.assign(size_t(w) * h * n, 0.f);
        const size_t bytes = bps / 8;
        auto conv = [&](const uint8_t* p) -> float {
            if (is_float) {
                if (bps == 32) { float f; std::memcpy(&f, p, 4); return f; }
                uint16_t b; std::memcpy(&b, p, 2); half hf; hf.setBits(b); return float(hf);
            }
            if (bps == 8) return p[0] / 255.f;
            uint16_t v; std::memcpy(&v, p, 2); return v / 65535.f;
        };
        // put(x, y, sample, src) for one sample
        auto put = [&](uint32_t x, uint32_t y, int s, const uint8_t* p) {
            if (x < W && y < H) data[(size_t(y) * w + x) * n + s] = conv(p);
        };
        const int planes = planar == PLANARCONFIG_SEPARATE ? n : 1;
        const int per = planar == PLANARCONFIG_SEPARATE ? 1 : n;  // samples per pixel in one buffer
        if (TIFFIsTiled(t)) {
            uint32_t tw = 0, th = 0;
            TIFFGetField(t, TIFFTAG_TILEWIDTH, &tw);
            TIFFGetField(t, TIFFTAG_TILELENGTH, &th);
            std::vector<uint8_t> buf(size_t(TIFFTileSize(t)));
            for (int pl = 0; pl < planes; ++pl)
                for (uint32_t ty = 0; ty < H; ty += th)
                    for (uint32_t tx = 0; tx < W; tx += tw) {
                        if (TIFFReadTile(t, buf.data(), tx, ty, 0, uint16_t(pl)) < 0) { err = path + ": TIFF tile read failed"; return false; }
                        for (uint32_t y = 0; y < th; ++y)
                            for (uint32_t x = 0; x < tw; ++x)
                                for (int s = 0; s < per; ++s)
                                    put(tx + x, ty + y, pl + s, &buf[((size_t(y) * tw + x) * per + s) * bytes]);
                    }
        } else {
            std::vector<uint8_t> buf(size_t(TIFFScanlineSize(t)));
            for (int pl = 0; pl < planes; ++pl)
                for (uint32_t y = 0; y < H; ++y) {
                    if (TIFFReadScanline(t, buf.data(), y, uint16_t(pl)) < 0) { err = path + ": TIFF scanline read failed"; return false; }
                    for (uint32_t x = 0; x < W; ++x)
                        for (int s = 0; s < per; ++s) put(x, y, pl + s, &buf[(size_t(x) * per + s) * bytes]);
                }
        }
        static const char* NAMES[5][4] = {{}, {"Y"}, {"Y", "A"}, {"R", "G", "B"}, {"R", "G", "B", "A"}};
        for (int c = 0; c < n; ++c) channels.push_back(n <= 4 ? NAMES[n][c] : (c < 4 ? NAMES[4][c] : ("C" + std::to_string(c)).c_str()));
        dw = disp = Imath::Box2i(Imath::V2i(0, 0), Imath::V2i(w - 1, h - 1));
        return true;
    }
};

} // namespace

std::unique_ptr<Source> Source::open(const std::string& path, std::string& err) {
    std::error_code ec;
    if (!fs::exists(u8(path), ec)) { err = "file not found: " + path; return nullptr; }
    const std::string ext = lower(u8(path).extension().u8string());
    if (ext == ".exr") {
        auto s = std::make_unique<ExrSource>();
        s->path = path;
        try {
            s->file = std::make_unique<Imf::MultiPartInputFile>(path.c_str());
            for (int p = 0; p < s->file->parts(); ++p) {
                const Imf::Header& hd = s->file->header(p);
                for (auto c = hd.channels().begin(); c != hd.channels().end(); ++c)
                    if (s->part_of.emplace(c.name(), p).second) s->channels.push_back(c.name());
            }
            s->header = &s->file->header(0);
            s->dw = s->header->dataWindow();
            s->disp = s->header->displayWindow();
        } catch (const std::exception& e) { err = path + ": " + e.what(); return nullptr; }
        return s;
    }
    if (ext == ".png") {
        auto s = std::make_unique<PngSource>();
        s->path = path;
        const bool is16 = stbi_is_16_bit(path.c_str()) != 0;
        void* raw = is16 ? static_cast<void*>(stbi_load_16(path.c_str(), &s->w, &s->h, &s->n, 0))
                         : static_cast<void*>(stbi_load(path.c_str(), &s->w, &s->h, &s->n, 0));
        if (!raw) { err = path + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "cannot read PNG"); return nullptr; }
        const size_t count = size_t(s->w) * s->h * s->n;
        s->data.resize(count);
        for (size_t i = 0; i < count; ++i)
            s->data[i] = is16 ? static_cast<uint16_t*>(raw)[i] / 65535.f : static_cast<uint8_t*>(raw)[i] / 255.f;
        stbi_image_free(raw);
        static const char* NAMES[5][4] = {{}, {"Y"}, {"Y", "A"}, {"R", "G", "B"}, {"R", "G", "B", "A"}};
        for (int c = 0; c < s->n; ++c) s->channels.push_back(NAMES[s->n][c]);
        s->dw = s->disp = Imath::Box2i(Imath::V2i(0, 0), Imath::V2i(s->w - 1, s->h - 1));
        return s;
    }
    if (ext == ".tif" || ext == ".tiff") {
        auto s = std::make_unique<TiffSource>();
        s->path = path;
        if (!s->load(err)) return nullptr;
        return s;
    }
    err = "unsupported file type (EXR, PNG or TIFF): " + path;
    return nullptr;
}

// ---------------------------------------------------------------- sequences
std::string normalize_pattern(const std::string& p) {
    static const std::regex pct(R"(%0?(\d*)d)");
    std::smatch m;
    if (std::regex_search(p, m, pct)) {
        const int n = m[1].length() ? std::max(1, std::stoi(m[1].str())) : 4;
        return m.prefix().str() + std::string(size_t(n), '#') + m.suffix().str();
    }
    return p;
}

bool has_frames(const std::string& p) { return normalize_pattern(p).find('#') != std::string::npos; }

std::string expand_frame(const std::string& pattern, int frame) {
    const std::string p = normalize_pattern(pattern);
    const size_t a = p.find('#');
    if (a == std::string::npos) return p;
    size_t b = a;
    while (b < p.size() && p[b] == '#') ++b;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*d", int(b - a), frame);
    return p.substr(0, a) + buf + p.substr(b);
}

std::vector<int> list_frames(const std::string& pattern) {
    std::vector<int> out;
    const std::string p = normalize_pattern(pattern);
    const fs::path pp = u8(p);
    const std::string name = pp.filename().u8string();
    const size_t a = name.find('#');
    if (a == std::string::npos) return out;
    size_t b = a;
    while (b < name.size() && name[b] == '#') ++b;
    const std::string pre = lower(name.substr(0, a)), suf = lower(name.substr(b));
    const size_t pad = b - a;
    fs::path dir = pp.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string f = lower(it->path().filename().u8string());
        if (f.size() < pre.size() + suf.size() + pad) continue;
        if (f.compare(0, pre.size(), pre) != 0 || f.compare(f.size() - suf.size(), suf.size(), suf) != 0) continue;
        const std::string mid = f.substr(pre.size(), f.size() - pre.size() - suf.size());
        if (mid.size() < pad || mid.size() > 9 || !std::all_of(mid.begin(), mid.end(), ::isdigit)) continue;
        if (mid.size() > pad && mid[0] == '0') continue;  // 0001 is not a match for a 3-digit pattern
        out.push_back(std::stoi(mid));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace jdexrc
