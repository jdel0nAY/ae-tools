// jd_exrc: command line (SPEC.md section 5).
#include "exrc.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfMultiPartInputFile.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include <stb_image_write.h>
#include <tiffio.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

using namespace jdexrc;
namespace {

// printf-style output that also goes to --log-file (every command, so hosts like AE can read it back).
void say(const char* fmt, ...) {
    char small[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (size_t(n) < sizeof small) { log_raw(small); return; }
    std::string big(size_t(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    big.resize(size_t(n));
    log_raw(big);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? b : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

// "file[:TAIL]": TAIL is taken when it holds "->" or is 1-4 letters (so "D:/x.exr" stays a path).
void split_tail(const std::string& v, std::string& file, std::string& tail) {
    file = v; tail.clear();
    const size_t colon = v.rfind(':');
    if (colon == std::string::npos || colon <= 1) return;
    const std::string t = v.substr(colon + 1);
    const bool letters = !t.empty() && t.size() <= 4 && t.find_first_not_of("RGBAYZLrgbayzl") == std::string::npos;
    if (letters || (t.find("->") != std::string::npos && t.find_first_of("/\\") == std::string::npos)) { file = v.substr(0, colon); tail = t; }
}

// Shared by merge and crypto: builds a Job from flags.
int build_job(const std::vector<std::string>& a, Job& job, RunOptions& ro, std::string& write_job) {
    std::string err, main_name;
    std::vector<std::string> force_float, force_half;
    CryptoSpec cs;
    std::string mode_s = "priority", comp_s = "zip";
    for (size_t i = 0; i < a.size() && err.empty(); ++i) {
        auto need = [&](std::string& dst) { if (i + 1 >= a.size()) { err = "missing value for " + a[i]; return false; } dst = a[++i]; return true; };
        std::string v;
        const std::string& o = a[i];
        if (o == "-o") { if (need(v)) job.output = normalize_pattern(v); }
        else if (o == "--frames" || o == "--frame") { need(ro.frames); }
        else if (o == "--threads") { if (need(v)) ro.threads = std::atoi(v.c_str()); }
        else if (o == "--compression") { need(comp_s); }
        else if (o == "--multipart") job.multipart = true;
        else if (o == "--hold-missing") job.hold_missing = true;
        else if (o == "--metadata-from") need(job.metadata_from);
        else if (o == "--dry-run") ro.dry_run = true;
        else if (o == "--check") ro.check = true;
        else if (o == "--json-log") log_set_json(true);
        else if (o == "--write-job") need(write_job);
        else if (o == "--no-job-meta") job.store_job = false;
        else if (o == "--main") need(main_name);
        else if (o == "--float") { if (need(v)) force_float.push_back(v); }
        else if (o == "--half") { if (need(v)) force_half.push_back(v); }
        else if (o == "--layer") {
            if (!need(v)) break;
            const size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) { err = "bad --layer (want name=file[:MAP]): " + v; break; }
            LayerSpec l;
            l.name = v.substr(0, eq);
            std::string tail;
            split_tail(v.substr(eq + 1), l.src, tail);
            l.src = normalize_pattern(l.src);
            if (!parse_map(tail, l.map, err)) break;
            job.layers.push_back(l);
        }
        else if (o == "--name") need(cs.name);
        else if (o == "--mode") need(mode_s);
        else if (o == "--ranks") { if (need(v)) cs.ranks = std::atoi(v.c_str()); }
        else if (o == "--background") need(cs.background);
        else if (o == "--preview") cs.preview = true;
        else if (o == "--manifest-sidecar") cs.sidecar = true;
        else if (o == "--matte") {
            if (!need(v)) break;
            const size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) { err = "bad --matte (want name=file[:CH]): " + v; break; }
            const auto names = split(v.substr(0, eq), ',');
            std::string file, chans;
            split_tail(v.substr(eq + 1), file, chans);
            if (chans.find("->") != std::string::npos) { err = "--matte takes channel letters, not a map: " + v; break; }
            if (chans.empty()) chans = names.size() == 1 ? "R" : std::string("RGBA").substr(0, names.size());
            if (chans.size() != names.size()) { err = "names and channels differ in count: " + v; break; }
            for (size_t k = 0; k < names.size(); ++k) cs.objects.push_back({names[k], normalize_pattern(file), "", std::string(1, chans[k])});
        }
        else err = "unknown option: " + o;
    }
    if (err.empty() && !parse_mode(mode_s, cs.mode)) err = "bad --mode: " + mode_s;
    if (err.empty() && !parse_compression(comp_s, job.compression)) err = "bad --compression: " + comp_s;
    for (auto& l : job.layers) {
        l.main = l.name == main_name || (main_name.empty() && (l.name == "beauty" || l.name == "rgba"));
        l.type = default_type(l.name, l.map);
        for (auto& f : force_float) if (f == l.name) l.type = Imf::FLOAT;
        for (auto& f : force_half) if (f == l.name) l.type = Imf::HALF;
    }
    if (!cs.objects.empty()) job.cryptos.push_back(cs);
    if (err.empty()) validate_job(job, err);
    if (!err.empty()) { log_line("error", err); return EXIT_BADJOB; }
    return EXIT_OK;
}

int finish(Job& job, RunOptions& ro, const std::string& write_job) {
    if (!write_job.empty()) {
        std::ofstream f(std::filesystem::u8path(write_job), std::ios::binary);
        f << job.json;
        if (!f) { log_line("error", "cannot write " + write_job); return EXIT_IO; }
    }
    return run_job(job, ro);
}

int cmd_build(const std::vector<std::string>& a) {
    Job job; RunOptions ro; std::string wj;
    const int rc = build_job(a, job, ro, wj);
    return rc != EXIT_OK ? rc : finish(job, ro, wj);
}

int cmd_run(const std::vector<std::string>& a) {
    std::string file, err;
    RunOptions ro;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "--frames" && i + 1 < a.size()) ro.frames = a[++i];
        else if (a[i] == "--threads" && i + 1 < a.size()) ro.threads = std::atoi(a[++i].c_str());
        else if (a[i] == "--dry-run") ro.dry_run = true;
        else if (a[i] == "--check") ro.check = true;
        else if (a[i] == "--json-log") log_set_json(true);
        else if (file.empty() && a[i][0] != '-') file = a[i];
        else { log_line("error", "unknown option: " + a[i]); return EXIT_BADJOB; }
    }
    if (file.empty()) { log_line("error", "usage: jd_exrc run job.json [--frames A-B] [--threads N] [--dry-run] [--json-log]"); return EXIT_BADJOB; }
    Job job;
    if (!job_from_file(file, job, err) || !validate_job(job, err)) { log_line("error", err); return EXIT_BADJOB; }
    return run_job(job, ro);
}

int cmd_check(const std::vector<std::string>& a) {
    bool js = false;
    std::vector<std::string> files;
    for (auto& s : a) { if (s == "--json") js = true; else files.push_back(s); }
    if (files.empty()) { say("usage: jd_exrc check file.exr [more.exr ...] [--json]\n"); return EXIT_BADJOB; }
    int worst = EXIT_OK;
    for (auto& f : files) {
        std::string rep;
        const int rc = check_file(f, js, rep);
        log_raw(rep + "\n");
        worst = std::max(worst, rc);
    }
    return worst;
}

int cmd_info(const std::vector<std::string>& a) {
    if (a.size() != 1) { say("usage: jd_exrc info file.exr\n"); return EXIT_BADJOB; }
    static const char* TYPES[] = {"uint", "half", "float"};
    try {
        Imf::MultiPartInputFile in(a[0].c_str());
        say("%s: %d part(s)\n", a[0].c_str(), in.parts());
        for (int p = 0; p < in.parts(); ++p) {
            const Imf::Header& hd = in.header(p);
            const Imath::Box2i dw = hd.dataWindow(), ds = hd.displayWindow();
            say("part %d%s%s: data [%d,%d]-[%d,%d] display [%d,%d]-[%d,%d] compression=%s\n", p,
                        hd.hasName() ? " name=" : "", hd.hasName() ? hd.name().c_str() : "", dw.min.x, dw.min.y,
                        dw.max.x, dw.max.y, ds.min.x, ds.min.y, ds.max.x, ds.max.y, compression_name(hd.compression()));
            for (auto it = hd.channels().begin(); it != hd.channels().end(); ++it) {
                const int t = int(it.channel().type);
                say("  channel %-32s %s\n", it.name(), t >= 0 && t < 3 ? TYPES[t] : "?");
            }
            for (auto it = hd.begin(); it != hd.end(); ++it) {
                const std::string nm = it.name();
                if (nm.find("cryptomatte/") == std::string::npos && nm.rfind("jd_exrcompiler/version", 0) != 0) continue;
                auto* s = dynamic_cast<const Imf::StringAttribute*>(&it.attribute());
                std::string v = s ? s->value() : std::string("(non-string)");
                if (v.size() > 200) v = v.substr(0, 200) + "...";
                say("  %s = %s\n", nm.c_str(), v.c_str());
            }
        }
    } catch (const std::exception& e) { say("error: %s\n", e.what()); return EXIT_IO; }
    return EXIT_OK;
}

// Test helper: small RGB matte (left block, right block, box over both), shifted by --shift px.
// Writes EXR (float) or, with a .png name, 8-bit PNG.
int cmd_testmatte(const std::vector<std::string>& a) {
    std::string out;
    int shift = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "-o" && i + 1 < a.size()) out = a[++i];
        else if (a[i] == "--shift" && i + 1 < a.size()) shift = std::atoi(a[++i].c_str());
    }
    if (out.empty()) { say("usage: jd_exrc testmatte -o file.exr|file.png [--shift N]\n"); return EXIT_BADJOB; }
    const int w = 64, h = 32;
    std::vector<float> r(size_t(w) * h), g(r.size()), b(r.size());
    for (int y = 0; y < h; ++y)
        for (int x0 = 0; x0 < w; ++x0) {
            const size_t i = size_t(y) * w + x0;
            const int x = x0 - shift;
            r[i] = x < 30 ? 1.f : (x < 34 ? (34 - x) / 4.f : 0.f);
            g[i] = x >= 34 ? 1.f : (x >= 30 ? (x - 30) / 4.f : 0.f);
            b[i] = (y >= 8 && y < 24 && x >= 16 && x < 48) ? 1.f : 0.f;
        }
    const bool png = out.size() > 4 && (out.substr(out.size() - 4) == ".png" || out.substr(out.size() - 4) == ".PNG");
    if (png) {
        std::vector<unsigned char> px(size_t(w) * h * 3);
        for (size_t i = 0; i < r.size(); ++i) {
            px[i * 3 + 0] = (unsigned char)(r[i] * 255.f + 0.5f);
            px[i * 3 + 1] = (unsigned char)(g[i] * 255.f + 0.5f);
            px[i * 3 + 2] = (unsigned char)(b[i] * 255.f + 0.5f);
        }
        if (!stbi_write_png(out.c_str(), w, h, 3, px.data(), w * 3)) { say("error: cannot write %s\n", out.c_str()); return EXIT_IO; }
    } else if (out.size() > 4 && (out.substr(out.size() - 4) == ".tif" || out.substr(out.size() - 5) == ".tiff")) {
        // .tif: 32-bit float, contiguous strips. .tiff: 16-bit integer, planar tiles (both reader paths covered).
        const bool f32 = out.substr(out.size() - 4) == ".tif";
        TIFF* t = TIFFOpenW(std::filesystem::u8path(out).wstring().c_str(), "w");
        if (!t) { say("error: cannot write %s\n", out.c_str()); return EXIT_IO; }
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, uint32_t(w));
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, uint32_t(h));
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, uint16_t(3));
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, uint16_t(f32 ? 32 : 16));
        TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, uint16_t(f32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT));
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, uint16_t(PHOTOMETRIC_RGB));
        TIFFSetField(t, TIFFTAG_COMPRESSION, uint16_t(f32 ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_LZW));
        std::vector<float>* buf[3] = {&r, &g, &b};
        bool ok = true;
        if (f32) {
            TIFFSetField(t, TIFFTAG_PLANARCONFIG, uint16_t(PLANARCONFIG_CONTIG));
            TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, uint32_t(8));
            std::vector<float> row(size_t(w) * 3);
            for (int y = 0; y < h && ok; ++y) {
                for (int x = 0; x < w; ++x) for (int c = 0; c < 3; ++c) row[size_t(x) * 3 + c] = (*buf[c])[size_t(y) * w + x];
                ok = TIFFWriteScanline(t, row.data(), uint32_t(y), 0) >= 0;
            }
        } else {
            TIFFSetField(t, TIFFTAG_PLANARCONFIG, uint16_t(PLANARCONFIG_SEPARATE));
            TIFFSetField(t, TIFFTAG_TILEWIDTH, uint32_t(16));
            TIFFSetField(t, TIFFTAG_TILELENGTH, uint32_t(16));
            std::vector<uint16_t> tile(16 * 16);
            for (int c = 0; c < 3 && ok; ++c)
                for (int ty = 0; ty < h; ty += 16)
                    for (int tx = 0; tx < w; tx += 16) {
                        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
                            const int X = tx + x, Y = ty + y;
                            const float v = (X < w && Y < h) ? (*buf[c])[size_t(Y) * w + X] : 0.f;
                            tile[size_t(y) * 16 + x] = uint16_t(v * 65535.f + 0.5f);
                        }
                        ok = ok && TIFFWriteTile(t, tile.data(), uint32_t(tx), uint32_t(ty), 0, uint16_t(c)) >= 0;
                    }
        }
        TIFFClose(t);
        if (!ok) { say("error: cannot write %s\n", out.c_str()); return EXIT_IO; }
    } else {
        try {
            Imf::Header hd(w, h);
            Imf::FrameBuffer fb;
            const char* nm[3] = {"R", "G", "B"};
            std::vector<float>* buf[3] = {&r, &g, &b};
            for (int c = 0; c < 3; ++c) {
                hd.channels().insert(nm[c], Imf::Channel(Imf::FLOAT));
                fb.insert(nm[c], Imf::Slice(Imf::FLOAT, reinterpret_cast<char*>(buf[c]->data()), sizeof(float), sizeof(float) * w));
            }
            hd.insert("testmatte/shift", Imf::StringAttribute(std::to_string(shift)));
            Imf::OutputFile of(out.c_str(), hd);
            of.setFrameBuffer(fb);
            of.writePixels(h);
        } catch (const std::exception& e) { say("error: %s\n", e.what()); return EXIT_IO; }
    }
    say("wrote %s\n", out.c_str());
    return EXIT_OK;
}

// Debug helper: every channel of one pixel (value and float bits).
int cmd_pixel(const std::vector<std::string>& a) {
    if (a.size() != 3) { say("usage: jd_exrc pixel file.exr x y\n"); return EXIT_BADJOB; }
    const int x = std::atoi(a[1].c_str()), y = std::atoi(a[2].c_str());
    std::string err;
    auto s = Source::open(a[0], err);
    if (!s) { say("error: %s\n", err.c_str()); return EXIT_IO; }
    for (auto& c : s->channels) {
        Plane p;
        if (!s->read(c, p, err)) { say("error: %s\n", err.c_str()); return EXIT_IO; }
        if (x < p.dw.min.x || x > p.dw.max.x || y < p.dw.min.y || y > p.dw.max.y) { say("%-32s (outside data window)\n", c.c_str()); continue; }
        const float f = p.px[size_t(y - p.dw.min.y) * p.w() + size_t(x - p.dw.min.x)];
        say("%-32s %.9g  0x%08x\n", c.c_str(), f, float_to_bits(f));
    }
    return EXIT_OK;
}

const char* USAGE =
    "jd_exrc 1.0.1: merge passes into multi-layer EXRs and build Cryptomattes from mattes\n"
    "\n"
    "  jd_exrc run job.json [--frames A-B] [--threads N] [--dry-run] [--json-log]\n"
    "  jd_exrc merge -o out.####.exr --layer beauty=beauty.####.exr:RGBA --layer depth=z.####.exr:R->Z\n"
    "  jd_exrc crypto -o crypto.####.exr --matte hero=hero.####.png --matte car,tree,sky=rgb.####.exr:RGB\n"
    "  jd_exrc check file.exr [--json]\n"
    "  jd_exrc info file.exr\n"
    "\n"
    "merge and crypto take the same options, so one command can write passes and a Cryptomatte:\n"
    "  -o PATH               output (#### or %04d for frames)\n"
    "  --layer NAME=FILE[:MAP]   MAP: RGBA, RGB, A, Y, Z, or SRC->DST[,SRC->DST] (default: R,G,B,A,Y,Z found)\n"
    "  --main NAME           layer written as plain R,G,B,A (default: a layer named beauty or rgba)\n"
    "  --float NAME / --half NAME   pixel type (default: half; float for depth, position, normal, motion, uv, id, Z)\n"
    "  --matte NAMES=FILE[:CH]  CH per name: R G B A Y, or L (luma); a,b,c=file:RGB splits one file in three\n"
    "  --name CryptoMatte  --mode priority|holdout|normalise  --ranks 6  --background NAME\n"
    "  --preview  --manifest-sidecar\n"
    "  --frames A-B  --hold-missing  --compression zip|zips|piz|rle|none|dwaa|dwab|b44|b44a|pxr24\n"
    "  --multipart  --metadata-from LAYER  --threads N  --dry-run  --json-log  --write-job job.json\n"
    "  --no-job-meta         don't store the job JSON in the output metadata\n"
    "  --check               spec-check every written frame (run, merge, crypto); exit 1 if one fails\n"
    "  --log-file PATH       (any command) also write every output line to PATH\n"
    "\n"
    "Exit codes: 0 ok, 1 check failed, 2 bad job, 3 file error.\n";

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + (argc > 1 ? 2 : 1), argv + argc);
    const std::string cmd = argc > 1 ? argv[1] : "";
    // --log-file PATH (any command): every output line is also written to PATH, for hosts like After Effects
    // whose system.callSystem does not pass a child's stdout through.
    for (size_t i = 0; i < args.size(); ++i)
        if (args[i] == "--log-file") {
            if (i + 1 >= args.size() || !log_set_file(args[i + 1])) {
                say("error: --log-file needs a writable path\n");
                return EXIT_BADJOB;
            }
            args.erase(args.begin() + long(i), args.begin() + long(i) + 2);
            break;
        }
    if (cmd == "run") return cmd_run(args);
    if (cmd == "merge" || cmd == "crypto") return cmd_build(args);
    if (cmd == "check") return cmd_check(args);
    if (cmd == "info") return cmd_info(args);
    if (cmd == "testmatte") return cmd_testmatte(args);
    if (cmd == "pixel") return cmd_pixel(args);
    if (cmd == "--version" || cmd == "version") { say("jd_exrc %s\n", VERSION); return EXIT_OK; }
    say("%s", USAGE);
    return cmd.empty() || cmd == "--help" || cmd == "-h" || cmd == "help" ? EXIT_OK : EXIT_BADJOB;
}
