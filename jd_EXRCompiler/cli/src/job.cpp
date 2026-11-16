// jd_exrc job: parse, validate, run frames (SPEC.md sections 3-5).
#include "exrc.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>
#include <ImfStringAttribute.h>
#include <ImfThreading.h>
#include <half.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jdexrc {

// ---------------------------------------------------------------- small parsers
bool parse_compression(const std::string& s, Imf::Compression& c) {
    static const std::map<std::string, Imf::Compression> M = {
        {"none", Imf::NO_COMPRESSION}, {"rle", Imf::RLE_COMPRESSION},   {"zips", Imf::ZIPS_COMPRESSION},
        {"zip", Imf::ZIP_COMPRESSION}, {"piz", Imf::PIZ_COMPRESSION},   {"pxr24", Imf::PXR24_COMPRESSION},
        {"b44", Imf::B44_COMPRESSION}, {"b44a", Imf::B44A_COMPRESSION}, {"dwaa", Imf::DWAA_COMPRESSION},
        {"dwab", Imf::DWAB_COMPRESSION}};
    auto it = M.find(s);
    if (it == M.end()) return false;
    c = it->second;
    return true;
}
const char* compression_name(Imf::Compression c) {
    switch (c) {
        case Imf::NO_COMPRESSION: return "none";   case Imf::RLE_COMPRESSION: return "rle";
        case Imf::ZIPS_COMPRESSION: return "zips"; case Imf::ZIP_COMPRESSION: return "zip";
        case Imf::PIZ_COMPRESSION: return "piz";   case Imf::PXR24_COMPRESSION: return "pxr24";
        case Imf::B44_COMPRESSION: return "b44";   case Imf::B44A_COMPRESSION: return "b44a";
        case Imf::DWAA_COMPRESSION: return "dwaa"; case Imf::DWAB_COMPRESSION: return "dwab";
        default: return "other";
    }
}
bool is_lossy(Imf::Compression c) {
    return c == Imf::PXR24_COMPRESSION || c == Imf::B44_COMPRESSION || c == Imf::B44A_COMPRESSION ||
           c == Imf::DWAA_COMPRESSION || c == Imf::DWAB_COMPRESSION;
}

// "RGBA" | "RGB" | "A" | "Z" (same names in and out) | "R->Z" | "R->Z,G->W" | "ViewLayer.Depth.Z->Z" | "auto"
bool parse_map(const std::string& s, std::vector<std::pair<std::string, std::string>>& map, std::string& err) {
    map.clear();
    if (s.empty() || s == "auto") return true;
    if (s.find("->") == std::string::npos) {
        for (char c : s) {
            if (!std::isalpha(static_cast<unsigned char>(c))) { err = "bad channel map: " + s; return false; }
            map.push_back({std::string(1, c), std::string(1, c)});
        }
        return true;
    }
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const size_t a = item.find("->");
        if (a == std::string::npos || a == 0 || a + 2 >= item.size()) { err = "bad channel map entry: " + item; return false; }
        map.push_back({item.substr(0, a), item.substr(a + 2)});
    }
    return true;
}

Imf::PixelType default_type(const std::string& name, const std::vector<std::pair<std::string, std::string>>& map) {
    static const std::set<std::string> DATA = {"depth", "z", "position", "p", "pworld", "pref", "normal", "n",
                                               "motion", "mv", "vector", "velocity", "uv", "id", "objectid"};
    std::string l = name;
    for (auto& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (DATA.count(l)) return Imf::FLOAT;
    for (auto& m : map) if (m.second == "Z" || m.second == "z") return Imf::FLOAT;
    return Imf::HALF;
}

// ---------------------------------------------------------------- job JSON
static bool valid_layer_name(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    return s.front() != '.' && s.back() != '.';
}

bool job_from_json_text(const std::string& text, Job& job, std::string& err) {
    try {
        const json j = json::parse(text);
        if (j.value("version", 1) != 1) { err = "unsupported job version"; return false; }
        job.output = normalize_pattern(j.at("output").get<std::string>());
        if (j.contains("frames")) job.frames = j["frames"].is_number() ? std::to_string(j["frames"].get<int>()) : j["frames"].get<std::string>();
        if (!parse_compression(j.value("compression", std::string("zip")), job.compression)) { err = "bad compression"; return false; }
        job.multipart = j.value("multipart", false);
        job.hold_missing = j.value("hold_missing", false);
        job.metadata_from = j.contains("metadata_from") && !j["metadata_from"].is_null() ? j["metadata_from"].get<std::string>() : "";
        for (const auto& l : j.value("layers", json::array())) {
            LayerSpec s;
            s.name = l.at("name").get<std::string>();
            s.src = normalize_pattern(l.at("src").get<std::string>());
            s.src_layer = l.value("layer", std::string());
            s.main = l.value("main", false);
            if (!parse_map(l.value("map", std::string("auto")), s.map, err)) return false;
            const std::string t = l.value("type", std::string());
            s.type = t.empty() ? default_type(s.name, s.map) : (t == "float" ? Imf::FLOAT : Imf::HALF);
            if (!t.empty() && t != "float" && t != "half") { err = "type must be half or float: " + t; return false; }
            job.layers.push_back(s);
        }
        for (const auto& c : j.value("cryptomattes", json::array())) {
            CryptoSpec s;
            s.name = c.value("name", std::string("CryptoMatte"));
            s.ranks = c.value("ranks", 6);
            if (!parse_mode(c.value("mode", std::string("priority")), s.mode)) { err = "bad mode in " + s.name; return false; }
            s.background = c.contains("background") && !c["background"].is_null() ? c["background"].get<std::string>() : "";
            s.preview = c.value("preview", false);
            s.sidecar = c.value("manifest_sidecar", false);
            for (const auto& o : c.value("objects", json::array())) {
                ObjectSpec ob;
                ob.name = o.at("name").get<std::string>();
                ob.src = normalize_pattern(o.at("src").get<std::string>());
                ob.src_layer = o.value("layer", std::string());
                ob.channel = o.value("channel", std::string("R"));
                s.objects.push_back(ob);
            }
            job.cryptos.push_back(s);
        }
        job.json = j.dump();
        return true;
    } catch (const std::exception& e) { err = std::string("job: ") + e.what(); return false; }
}

bool job_from_file(const std::string& path, Job& job, std::string& err) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) { err = "cannot read job file: " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    return job_from_json_text(ss.str(), job, err);
}

std::string job_to_json(const Job& job) {
    json j;
    j["version"] = 1;
    j["output"] = job.output;
    j["frames"] = job.frames;
    j["compression"] = compression_name(job.compression);
    j["multipart"] = job.multipart;
    j["hold_missing"] = job.hold_missing;
    if (!job.metadata_from.empty()) j["metadata_from"] = job.metadata_from;
    j["layers"] = json::array();
    for (const auto& l : job.layers) {
        std::string m;
        for (const auto& p : l.map) m += (m.empty() ? "" : ",") + p.first + "->" + p.second;
        json o = {{"name", l.name}, {"src", l.src}, {"map", m.empty() ? "auto" : m}, {"type", l.type == Imf::FLOAT ? "float" : "half"}};
        if (l.main) o["main"] = true;
        if (!l.src_layer.empty()) o["layer"] = l.src_layer;
        j["layers"].push_back(o);
    }
    j["cryptomattes"] = json::array();
    for (const auto& c : job.cryptos) {
        json o = {{"name", c.name}, {"ranks", c.ranks},
                  {"mode", c.mode == Mode::Priority ? "priority" : c.mode == Mode::Holdout ? "holdout" : "normalise"},
                  {"preview", c.preview}, {"manifest_sidecar", c.sidecar}, {"objects", json::array()}};
        if (!c.background.empty()) o["background"] = c.background;
        for (const auto& ob : c.objects) {
            json x = {{"name", ob.name}, {"src", ob.src}, {"channel", ob.channel}};
            if (!ob.src_layer.empty()) x["layer"] = ob.src_layer;
            o["objects"].push_back(x);
        }
        j["cryptomattes"].push_back(o);
    }
    return j.dump();
}

bool validate_job(Job& job, std::string& err) {
    if (job.output.empty()) { err = "no output"; return false; }
    if (job.layers.empty() && job.cryptos.empty()) { err = "nothing to write: no layers and no cryptomattes"; return false; }
    std::set<std::string> names, out_channels;
    int mains = 0;
    for (auto& l : job.layers) {
        if (!valid_layer_name(l.name)) { err = "bad layer name: " + l.name; return false; }
        if (!names.insert(l.name).second) { err = "duplicate layer name: " + l.name; return false; }
        mains += l.main;
        if (l.src.empty()) { err = "layer " + l.name + " has no src"; return false; }
    }
    if (mains > 1) { err = "more than one main layer"; return false; }
    for (auto& c : job.cryptos) {
        if (!valid_type_name(c.name)) { err = "bad Cryptomatte name (letters, digits, _ only): " + c.name; return false; }
        if (names.count(c.name)) { err = "Cryptomatte name clashes with a layer: " + c.name; return false; }
        for (auto& n : names)
            if (n.size() == c.name.size() + 2 && n.compare(0, c.name.size(), c.name) == 0) { err = "layer " + n + " clashes with Cryptomatte " + c.name; return false; }
        names.insert(c.name);
        if (c.ranks < 2 || c.ranks > 16 || (c.ranks & 1)) { err = "ranks must be even, 2 to 16 (" + c.name + ")"; return false; }
        if (c.objects.empty()) { err = "Cryptomatte " + c.name + " has no objects"; return false; }
        std::set<std::string> on;
        std::set<uint32_t> ids;
        for (auto& o : c.objects) {
            if (o.name.empty()) { err = "empty object name in " + c.name; return false; }
            if (!on.insert(o.name).second) { err = "duplicate object name: " + o.name; return false; }
            if (!ids.insert(name_to_id_bits(o.name)).second) { err = "object names hash to the same ID: " + o.name; return false; }
            if (o.src.empty()) { err = "object " + o.name + " has no src"; return false; }
        }
        if (!c.background.empty() && (on.count(c.background) || !ids.insert(name_to_id_bits(c.background)).second)) {
            err = "background name clashes with an object: " + c.background; return false;
        }
    }
    if (is_lossy(job.compression) && !job.cryptos.empty() && !job.multipart) {
        err = std::string("lossy compression (") + compression_name(job.compression) + ") is refused with a Cryptomatte; use zip, zips, piz, rle or none";
        return false;
    }
    if (job.json.empty()) job.json = job_to_json(job);
    return true;
}

// ---------------------------------------------------------------- one frame
namespace {

struct OutChannel { std::string name; Imf::PixelType type; std::vector<float> buf; Plane src; int part = 0; };

struct FrameResult { int code = EXIT_OK; std::string msg, file; unsigned long long dropped_objs = 0, dropped_px = 0; };

std::string resolve_path(const std::string& pattern, int frame, const std::vector<int>& avail, bool hold) {
    if (frame == INT32_MIN || !has_frames(pattern)) return normalize_pattern(pattern);
    if (!hold || avail.empty() || std::binary_search(avail.begin(), avail.end(), frame)) return expand_frame(pattern, frame);
    auto it = std::upper_bound(avail.begin(), avail.end(), frame);
    return expand_frame(pattern, it == avail.begin() ? *it : *(it - 1));
}

std::string sidecar_name(const Job& job, const std::string& type) {
    fs::path p = fs::u8path(job.output);
    std::string stem = p.filename().u8string();
    const size_t hash = stem.find('#');
    stem = hash != std::string::npos ? stem.substr(0, hash) : fs::u8path(stem).stem().u8string();
    while (!stem.empty() && (stem.back() == '.' || stem.back() == '_')) stem.pop_back();
    return (stem.empty() ? "" : stem + "_") + type + ".json";
}

FrameResult compile_frame(const Job& job, int frame, const std::map<std::string, std::vector<int>>& avail) {
    FrameResult r;
    std::map<std::string, std::unique_ptr<Source>> cache;
    auto get = [&](const std::string& pattern) -> Source* {
        auto a = avail.find(pattern);
        const std::string path = resolve_path(pattern, frame, a == avail.end() ? std::vector<int>() : a->second, job.hold_missing);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second.get();
        std::string err;
        auto s = Source::open(path, err);
        if (!s) { r.code = EXIT_IO; r.msg = err; return nullptr; }
        return cache.emplace(path, std::move(s)).first->second.get();
    };

    // ---- read every plane
    std::vector<OutChannel> chans;
    const Source* meta_src = nullptr;
    const Source* first = nullptr;
    int part = 0;
    for (const auto& l : job.layers) {
        Source* s = get(l.src);
        if (!s) return r;
        if (!first) first = s;
        if ((job.metadata_from.empty() && !meta_src && s->header) || job.metadata_from == l.name) meta_src = s;
        auto map = l.map;
        if (map.empty())  // auto: R, G, B, A, Y, Z that exist under the source layer
            for (const char* c : {"R", "G", "B", "A", "Y", "Z"}) {
                const std::string real = s->resolve(l.src_layer, c);
                bool dup = false;
                for (auto& m : map) dup |= s->resolve(l.src_layer, m.first) == real;
                if (!real.empty() && !dup) map.push_back({c, c});
            }
        if (map.empty()) { r.code = EXIT_BADJOB; r.msg = "no channels found for layer " + l.name + " in " + s->path; return r; }
        for (const auto& m : map) {
            OutChannel oc;
            oc.name = l.main ? m.second : l.name + "." + m.second;
            oc.type = l.type;
            oc.part = part;
            std::string err;
            if (!s->read_spec(l.src_layer, m.first, oc.src, err)) { r.code = EXIT_IO; r.msg = err; return r; }
            for (auto& o : chans) if (o.name == oc.name) { r.code = EXIT_BADJOB; r.msg = "two layers write channel " + oc.name; return r; }
            chans.push_back(std::move(oc));
        }
        ++part;
    }
    std::vector<std::vector<Plane>> mattes(job.cryptos.size());
    for (size_t ci = 0; ci < job.cryptos.size(); ++ci)
        for (const auto& o : job.cryptos[ci].objects) {
            Source* s = get(o.src);
            if (!s) return r;
            if (!first) first = s;
            Plane p;
            std::string err;
            if (!s->read_spec(o.src_layer, o.channel, p, err)) { r.code = EXIT_IO; r.msg = err; return r; }
            mattes[ci].push_back(std::move(p));
        }

    // ---- windows: one display window; data window = union
    const Imath::Box2i disp = first->disp;
    for (auto& kv : cache)
        if (kv.second->disp != disp) { r.code = EXIT_BADJOB; r.msg = "display window differs: " + kv.first + " vs " + first->path; return r; }
    Imath::Box2i dw;
    for (auto& c : chans) dw.extendBy(c.src.dw);
    for (auto& v : mattes) for (auto& p : v) dw.extendBy(p.dw);
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    const size_t npix = size_t(w) * h;
    for (auto& c : chans) { c.buf.assign(npix, 0.f); blit(c.src, dw, c.buf); c.src.px.clear(); c.src.px.shrink_to_fit(); }

    // ---- Cryptomattes
    std::vector<std::vector<std::pair<std::string, std::string>>> crypto_meta(job.cryptos.size());
    for (size_t ci = 0; ci < job.cryptos.size(); ++ci) {
        const CryptoSpec& cs = job.cryptos[ci];
        const size_t n = cs.objects.size(), nobj = n + (cs.background.empty() ? 0 : 1);
        std::vector<std::vector<float>> m(n, std::vector<float>(npix, 0.f));
        for (size_t i = 0; i < n; ++i) { blit(mattes[ci][i], dw, m[i]); mattes[ci][i].px.clear(); }
        std::vector<uint32_t> ids(nobj);
        std::vector<std::string> names;
        for (size_t i = 0; i < n; ++i) { ids[i] = name_to_id_bits(cs.objects[i].name); names.push_back(cs.objects[i].name); }
        if (!cs.background.empty()) { ids[n] = name_to_id_bits(cs.background); names.push_back(cs.background); }
        const int nl = cs.ranks / 2;
        const size_t base = chans.size();
        static const char* RGBA[4] = {"R", "G", "B", "A"};
        for (int l = 0; l < nl; ++l)
            for (int c = 0; c < 4; ++c) {
                OutChannel oc;
                char nm[96];
                std::snprintf(nm, sizeof nm, "%s%02d.%s", cs.name.c_str(), l, RGBA[c]);
                oc.name = nm; oc.type = Imf::FLOAT; oc.part = part; oc.buf.assign(npix, 0.f);
                chans.push_back(std::move(oc));
            }
        const size_t pbase = chans.size();
        if (cs.preview)
            for (int c = 0; c < 3; ++c) {
                OutChannel oc; oc.name = cs.name + "." + RGBA[c]; oc.type = Imf::HALF; oc.part = part; oc.buf.assign(npix, 0.f);
                chans.push_back(std::move(oc));
            }
        std::vector<float> mv(nobj), cv(nobj);
        std::vector<Rank> rk(size_t(cs.ranks)), scratch;
        for (size_t p = 0; p < npix; ++p) {
            for (size_t i = 0; i < n; ++i) mv[i] = m[i][p];
            const float bg = coverage(mv.data(), cv.data(), n, cs.mode);
            if (!cs.background.empty()) cv[n] = bg;
            const size_t d = pack_ranks(ids.data(), cv.data(), nobj, rk.data(), size_t(cs.ranks), scratch);
            if (d) { r.dropped_objs += d; ++r.dropped_px; }
            float pr = 0, pg = 0, pb = 0;
            for (int k = 0; k < cs.ranks; ++k) {
                chans[base + size_t(k / 2) * 4 + (k % 2) * 2 + 0].buf[p] = bits_to_float(rk[k].id_bits);
                chans[base + size_t(k / 2) * 4 + (k % 2) * 2 + 1].buf[p] = rk[k].cov;
                if (cs.preview && rk[k].cov > 0) {
                    const uint32_t b = rk[k].id_bits;
                    pr += rk[k].cov * float(b & 0xff) / 255.f;
                    pg += rk[k].cov * float((b >> 8) & 0xff) / 255.f;
                    pb += rk[k].cov * float((b >> 16) & 0xff) / 255.f;
                }
            }
            if (cs.preview) { chans[pbase].buf[p] = pr; chans[pbase + 1].buf[p] = pg; chans[pbase + 2].buf[p] = pb; }
        }
        const std::string k = "cryptomatte/" + layer_key(cs.name) + "/";
        crypto_meta[ci] = {{k + "name", cs.name}, {k + "hash", "MurmurHash3_32"}, {k + "conversion", "uint32_to_float32"}};
        if (cs.sidecar) {
            crypto_meta[ci].push_back({k + "manif_file", sidecar_name(job, cs.name)});  // file written once by run_job
        } else {
            crypto_meta[ci].push_back({k + "manifest", manifest_json(names)});
        }
        ++part;
    }

    // ---- header(s)
    static const std::set<std::string> RESERVED = {"channels", "compression", "dataWindow", "displayWindow", "lineOrder",
                                                   "name", "type", "version", "chunkCount", "tiles", "view", "multiView"};
    auto make_header = [&](Imf::Compression comp) {
        Imf::Header hd(disp, dw, 1.f, Imath::V2f(0, 0), 1.f, Imf::INCREASING_Y, comp);
        if (meta_src && meta_src->header)
            for (auto it = meta_src->header->begin(); it != meta_src->header->end(); ++it) {
                const std::string nm = it.name();
                if (RESERVED.count(nm) || nm.rfind("cryptomatte/", 0) == 0 || nm.rfind("jd_exrcompiler/", 0) == 0) continue;
                try { hd.insert(nm.c_str(), it.attribute()); } catch (...) {}
            }
        hd.insert("jd_exrcompiler/version", Imf::StringAttribute(VERSION));
        if (job.store_job) hd.insert("jd_exrcompiler/job", Imf::StringAttribute(job.json));
        for (auto& cm : crypto_meta) for (auto& kv : cm) hd.insert(kv.first.c_str(), Imf::StringAttribute(kv.second));
        return hd;
    };
    // OpenEXR converts pixel types on read only, so half channels get a half buffer here (float->half rounds to
    // nearest; a half input survives half->float->half bit for bit).
    std::vector<std::vector<half>> hbufs(chans.size());
    for (size_t i = 0; i < chans.size(); ++i)
        if (chans[i].type == Imf::HALF) {
            hbufs[i].resize(chans[i].buf.size());
            for (size_t p = 0; p < chans[i].buf.size(); ++p) hbufs[i][p] = half(chans[i].buf[p]);
            chans[i].buf.clear(); chans[i].buf.shrink_to_fit();
        }
    auto slice_for = [&](OutChannel& c) {
        const size_t i = size_t(&c - chans.data());
        const ptrdiff_t off = ptrdiff_t(dw.min.x) + ptrdiff_t(dw.min.y) * w;
        if (c.type == Imf::HALF) {
            char* base = reinterpret_cast<char*>(hbufs[i].data()) - off * ptrdiff_t(sizeof(half));
            return Imf::Slice(Imf::HALF, base, sizeof(half), sizeof(half) * size_t(w));
        }
        char* base = reinterpret_cast<char*>(c.buf.data()) - off * ptrdiff_t(sizeof(float));
        return Imf::Slice(Imf::FLOAT, base, sizeof(float), sizeof(float) * size_t(w));
    };

    r.file = expand_frame(job.output, frame == INT32_MIN ? 0 : frame);
    if (frame == INT32_MIN) r.file = job.output;
    try {
        std::error_code ec;
        fs::create_directories(fs::u8path(r.file).parent_path(), ec);
        if (!job.multipart) {
            Imf::Header hd = make_header(job.compression);
            Imf::FrameBuffer fb;
            for (auto& c : chans) { hd.channels().insert(c.name.c_str(), Imf::Channel(c.type)); fb.insert(c.name.c_str(), slice_for(c)); }
            Imf::OutputFile of(r.file.c_str(), hd);
            of.setFrameBuffer(fb);
            of.writePixels(h);
        } else {
            std::vector<Imf::Header> hds;
            std::vector<std::string> part_names;
            for (const auto& l : job.layers) part_names.push_back(l.name);
            for (const auto& c : job.cryptos) part_names.push_back(c.name);
            for (int p = 0; p < part; ++p) {
                const bool is_crypto = p >= int(job.layers.size());
                Imf::Header hd = make_header(is_crypto && is_lossy(job.compression) ? Imf::ZIP_COMPRESSION : job.compression);
                hd.setName(part_names[size_t(p)]);
                hd.setType(Imf::SCANLINEIMAGE);
                for (auto& c : chans) if (c.part == p) hd.channels().insert(c.name.c_str(), Imf::Channel(c.type));
                hds.push_back(hd);
            }
            Imf::MultiPartOutputFile of(r.file.c_str(), hds.data(), int(hds.size()));
            for (int p = 0; p < part; ++p) {
                Imf::OutputPart op(of, p);
                Imf::FrameBuffer fb;
                for (auto& c : chans) if (c.part == p) fb.insert(c.name.c_str(), slice_for(c));
                op.setFrameBuffer(fb);
                op.writePixels(h);
            }
        }
    } catch (const std::exception& e) { r.code = EXIT_IO; r.msg = e.what(); return r; }
    return r;
}

} // namespace

// ---------------------------------------------------------------- run
int run_job(const Job& job, const RunOptions& opt) {
    // Patterns with frames, and the frames found on disk for each.
    std::set<std::string> patterns;
    for (auto& l : job.layers) if (has_frames(l.src)) patterns.insert(l.src);
    for (auto& c : job.cryptos) for (auto& o : c.objects) if (has_frames(o.src)) patterns.insert(o.src);
    std::map<std::string, std::vector<int>> avail;
    for (auto& p : patterns) avail[p] = list_frames(p);

    std::vector<int> frames;
    const std::string spec = !opt.frames.empty() ? opt.frames : job.frames;
    if (spec.empty() || spec == "auto") {
        bool firstp = true;
        for (auto& kv : avail) {
            if (firstp) { frames = kv.second; firstp = false; continue; }
            std::vector<int> x;
            std::set_intersection(frames.begin(), frames.end(), kv.second.begin(), kv.second.end(), std::back_inserter(x));
            frames.swap(x);
        }
        if (patterns.empty()) frames = {INT32_MIN};
        else if (frames.empty()) { log_line("error", "no frame is present in every input sequence"); return EXIT_IO; }
    } else {
        int a = 0, b = 0;
        char dash = 0;
        std::stringstream ss(spec);
        ss >> a;
        if (ss >> dash) { if (dash != '-' || !(ss >> b)) { log_line("error", "bad --frames (want N or A-B): " + spec); return EXIT_BADJOB; } }
        else b = a;
        if (b < a) { log_line("error", "bad --frames: end before start"); return EXIT_BADJOB; }
        for (int f = a; f <= b; ++f) frames.push_back(f);
        if (!job.hold_missing)
            for (auto& kv : avail)
                for (int f : frames)
                    if (!std::binary_search(kv.second.begin(), kv.second.end(), f)) {
                        log_line("error", "missing frame (use hold_missing to reuse the nearest one)", f, expand_frame(kv.first, f));
                        return EXIT_IO;
                    }
    }
    if (frames.size() > 1 && !has_frames(job.output)) { log_line("error", "output needs #### for more than one frame: " + job.output); return EXIT_BADJOB; }
    if (frames.size() == 1 && frames[0] != INT32_MIN && !has_frames(job.output)) {}  // a still from one frame: fine

    log_line("start", std::to_string(frames.size()) + " frame(s), " + std::to_string(job.layers.size()) + " layer(s), " +
                          std::to_string(job.cryptos.size()) + " cryptomatte(s)", INT32_MIN, job.output);
    if (opt.dry_run) {
        for (int f : frames) log_line("plan", "", f, f == INT32_MIN ? job.output : expand_frame(job.output, f));
        return EXIT_OK;
    }

    // Manifest sidecars: the same for every frame, so written once here.
    for (const auto& cs : job.cryptos) {
        if (!cs.sidecar) continue;
        std::vector<std::string> names;
        for (auto& o : cs.objects) names.push_back(o.name);
        if (!cs.background.empty()) names.push_back(cs.background);
        std::error_code ec;
        const fs::path dir = fs::u8path(job.output).parent_path();
        fs::create_directories(dir, ec);
        const fs::path sp = dir / fs::u8path(sidecar_name(job, cs.name));
        std::ofstream f(sp, std::ios::binary);
        f << manifest_json(names);
        if (!f) { log_line("error", "cannot write manifest sidecar " + sp.u8string()); return EXIT_IO; }
    }

    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    int nthreads = opt.threads > 0 ? opt.threads : int(std::min(4u, hw));
    nthreads = std::max(1, std::min(nthreads, int(frames.size())));
    std::atomic<size_t> next{0};
    std::atomic<int> worst{EXIT_OK};
    std::atomic<bool> stop{false};
    auto worker = [&]() {
        for (;;) {
            const size_t i = next++;
            if (i >= frames.size() || stop) return;
            FrameResult fr = compile_frame(job, frames[i], avail);
            const int f = frames[i] == INT32_MIN ? INT32_MIN : frames[i];
            if (fr.code != EXIT_OK) {
                log_line("error", fr.msg, f, fr.file);
                int w = worst.load();
                while (fr.code > w && !worst.compare_exchange_weak(w, fr.code)) {}
                stop = true;
                return;
            }
            log_line("frame", fr.dropped_px ? "dropped " + std::to_string(fr.dropped_objs) + " object(s) in " +
                                                  std::to_string(fr.dropped_px) + " pixel(s) (more objects than ranks)" : "ok",
                     f, fr.file);
            // --check: spec-check the frame just written (one call instead of run + check; AE pays ~5 s per call)
            if (opt.check && !job.cryptos.empty()) {
                std::string rep;
                const int rc = check_file(fr.file, false, rep);
                if (rc == EXIT_OK) log_line("check", "PASS", f, fr.file);
                else {
                    std::string first = rep;  // the first problem line is enough for the log
                    const size_t dash = rep.find("    - ");
                    if (dash != std::string::npos) first = rep.substr(dash + 6, rep.find('\n', dash) - dash - 6);
                    log_line("check", "FAIL: " + first, f, fr.file);
                    int w = worst.load();
                    while (rc > w && !worst.compare_exchange_weak(w, rc)) {}
                }
            }
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    log_line(worst == EXIT_OK ? "done" : "failed", "");
    return worst;
}

} // namespace jdexrc
