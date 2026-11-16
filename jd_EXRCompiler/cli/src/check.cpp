// jd_exrc check: Cryptomatte spec checks on any EXR (SPEC.md 8.7). Reads files from any writer.
#include "exrc.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStringAttribute.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jdexrc {

int check_file(const std::string& path, bool as_json, std::string& report) {
    json out = {{"file", path}, {"cryptomattes", json::array()}};
    bool all_ok = true;
    try {
        Imf::MultiPartInputFile in(path.c_str());
        // Metadata from every part (writers differ in where they put it).
        std::map<std::string, std::map<std::string, std::string>> meta;  // key -> field -> value
        std::map<std::string, std::pair<int, Imf::PixelType>> where;     // channel -> (part, type)
        std::map<int, Imf::Compression> comp;
        static const std::regex KEY(R"(^(?:exr/)?cryptomatte/([0-9a-f]{7})/(\w+)$)");
        for (int p = 0; p < in.parts(); ++p) {
            const Imf::Header& hd = in.header(p);
            comp[p] = hd.compression();
            for (auto it = hd.begin(); it != hd.end(); ++it) {
                std::smatch m;
                const std::string nm = it.name();
                if (!std::regex_match(nm, m, KEY)) continue;
                if (auto* s = dynamic_cast<const Imf::StringAttribute*>(&it.attribute())) meta[m[1]][m[2]] = s->value();
            }
            for (auto c = hd.channels().begin(); c != hd.channels().end(); ++c) where[c.name()] = {p, c.channel().type};
        }
        if (meta.empty()) {
            out["note"] = "no Cryptomatte metadata";
            all_ok = false;  // nothing to check counts as a failure
        }
        for (auto& kv : meta) {
            const std::string key = kv.first;
            auto& f = kv.second;
            json cj = {{"key", key}, {"problems", json::array()}};
            auto problem = [&](const std::string& s) { cj["problems"].push_back(s); };
            const std::string name = f.count("name") ? f["name"] : "";
            cj["name"] = name;
            if (name.empty()) { problem("no name"); out["cryptomattes"].push_back(cj); all_ok = false; continue; }
            if (layer_key(name) != key) problem("key " + key + " does not match name (expected " + layer_key(name) + ")");
            if (f["hash"] != "MurmurHash3_32") problem("hash is '" + f["hash"] + "', expected MurmurHash3_32");
            if (f["conversion"] != "uint32_to_float32") problem("conversion is '" + f["conversion"] + "', expected uint32_to_float32");

            // Manifest (inline or sidecar)
            std::map<uint32_t, std::string> manifest;
            std::string mtext = f.count("manifest") ? f["manifest"] : "";
            if (mtext.empty() && f.count("manif_file")) {
                if (f["manif_file"].find('\\') != std::string::npos) problem("manif_file uses back-slashes");
                std::ifstream mf(fs::u8path(path).parent_path() / fs::u8path(f["manif_file"]), std::ios::binary);
                std::stringstream ss; ss << mf.rdbuf(); mtext = ss.str();
                if (!mf) problem("manifest sidecar not found: " + f["manif_file"]);
            }
            try {
                const json mj = json::parse(mtext.empty() ? "{}" : mtext);
                for (auto it = mj.begin(); it != mj.end(); ++it) {
                    const std::string hx = it.value().get<std::string>();
                    const uint32_t bits = uint32_t(std::stoul(hx, nullptr, 16));
                    if (hx.size() != 8) problem("manifest hex is not 8 characters: " + it.key());
                    if (bits != name_to_id_bits(it.key())) problem("manifest hex for '" + it.key() + "' is " + hx + ", expected " + name_to_hex(it.key()));
                    manifest[bits] = it.key();
                }
            } catch (const std::exception& e) { problem(std::string("manifest is not valid JSON: ") + e.what()); }
            cj["objects"] = manifest.size();

            // Rank layers name00, name01, ... with r/g/b/a in any of the usual spellings
            std::map<std::pair<int, char>, std::string> lut;
            const std::regex CH("^" + std::regex_replace(name, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)") +
                                R"((\d\d)\.(r|g|b|a|red|green|blue|alpha)$)", std::regex::icase);
            for (auto& w : where) {
                std::smatch m;
                if (std::regex_match(w.first, m, CH)) lut[{std::stoi(m[1]), char(std::tolower(m[2].str()[0]))}] = w.first;
            }
            int nl = 0;
            while (lut.count({nl, 'r'}) && lut.count({nl, 'g'}) && lut.count({nl, 'b'}) && lut.count({nl, 'a'})) ++nl;
            cj["ranks"] = nl * 2;
            if (nl == 0) { problem("no rank channels " + name + "00.R/G/B/A"); out["cryptomattes"].push_back(cj); all_ok = false; continue; }

            // Read every rank channel as float
            std::vector<std::vector<float>> ch(size_t(nl) * 4);
            Imath::Box2i dw;
            int w = 0, h = 0;
            for (int l = 0; l < nl; ++l)
                for (int c = 0; c < 4; ++c) {
                    const std::string& nm = lut[{l, "rgba"[c]}];
                    const int p = where[nm].first;
                    if (where[nm].second != Imf::FLOAT) problem("channel " + nm + " is not 32-bit float");
                    if (is_lossy(comp[p])) problem(std::string("part with ") + nm + " uses lossy compression (" + compression_name(comp[p]) + ")");
                    Imf::InputPart part(in, p);
                    dw = part.header().dataWindow();
                    w = dw.max.x - dw.min.x + 1; h = dw.max.y - dw.min.y + 1;
                    auto& buf = ch[size_t(l) * 4 + c];
                    buf.assign(size_t(w) * h, 0.f);
                    char* base = reinterpret_cast<char*>(buf.data()) - (ptrdiff_t(dw.min.x) + ptrdiff_t(dw.min.y) * w) * ptrdiff_t(sizeof(float));
                    Imf::FrameBuffer fb;
                    fb.insert(nm.c_str(), Imf::Slice(Imf::FLOAT, base, sizeof(float), sizeof(float) * size_t(w)));
                    part.setFrameBuffer(fb);
                    part.readPixels(dw.min.y, dw.max.y);
                }

            // Per-pixel checks (count + first example)
            struct Count { unsigned long long n = 0; std::string first; };
            std::map<std::string, Count> bad;
            auto hit = [&](const char* what, size_t p) {
                Count& c = bad[what];
                if (c.n++ == 0) c.first = "(" + std::to_string(dw.min.x + int(p % size_t(w))) + "," + std::to_string(dw.min.y + int(p / size_t(w))) + ")";
            };
            std::set<uint32_t> unknown;
            unsigned long long covered = 0;
            const size_t npix = size_t(w) * h;
            for (size_t p = 0; p < npix; ++p) {
                double sum = 0;
                float prev = 2.f;
                for (int r = 0; r < nl * 2; ++r) {
                    const float idf = ch[size_t(r / 2) * 4 + (r % 2) * 2][p], cov = ch[size_t(r / 2) * 4 + (r % 2) * 2 + 1][p];
                    if (!(cov >= -1e-6f && cov <= 1.f + 1e-6f)) hit("coverage outside 0-1", p);
                    if (cov > prev + 1e-6f) hit("ranks not sorted by coverage", p);
                    prev = cov;
                    sum += cov;
                    if (cov > 0) {
                        const uint32_t b = float_to_bits(idf), e = (b >> 23) & 0xff;
                        if (e == 0 || e == 255) hit("ID is denormal, inf or NaN", p);
                        if (!manifest.count(b)) { hit("ID not in manifest", p); unknown.insert(b); }
                    }
                }
                if (sum > 1.0 + 1e-5) hit("coverage sum > 1", p);
                covered += sum > 0;
            }
            cj["covered_pixels"] = covered;
            // Unsorted ranks only warn: readers sum coverage per ID, and Cycles itself writes a few (seen in 5.2.3).
            cj["warnings"] = json::array();
            for (auto& b : bad) {
                const std::string line = b.first + ": " + std::to_string(b.second.n) + " pixel(s), first at " + b.second.first;
                if (b.first == "ranks not sorted by coverage") cj["warnings"].push_back(line);
                else problem(line);
            }
            if (!unknown.empty()) cj["unknown_ids"] = unknown.size();
            all_ok &= cj["problems"].empty();
            cj["ok"] = cj["problems"].empty();
            out["cryptomattes"].push_back(cj);
        }
    } catch (const std::exception& e) {
        out["error"] = e.what();
        report = as_json ? out.dump() : "error: " + std::string(e.what());
        return EXIT_IO;
    }
    out["ok"] = all_ok;
    if (as_json) report = out.dump();
    else {
        std::string s = path + "\n";
        if (out.contains("note")) s += "  " + out["note"].get<std::string>() + "\n";
        for (auto& c : out["cryptomattes"]) {
            s += "  " + c.value("name", std::string("?")) + " (key " + c["key"].get<std::string>() + "): " +
                 std::to_string(c.value("ranks", 0)) + " ranks, " + std::to_string(c.value("objects", size_t(0))) + " objects, " +
                 std::to_string(c.value("covered_pixels", 0ull)) + " covered pixels: " + (c["problems"].empty() ? "OK" : "PROBLEMS") + "\n";
            for (auto& p : c["problems"]) s += "    - " + p.get<std::string>() + "\n";
            if (c.contains("warnings"))
                for (auto& p : c["warnings"]) s += "    ~ warning: " + p.get<std::string>() + "\n";
        }
        s += all_ok ? "PASS" : "FAIL";
        report = s;
    }
    return all_ok ? EXIT_OK : EXIT_CHECK;
}

} // namespace jdexrc
