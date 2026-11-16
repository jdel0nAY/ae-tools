// jd_exrc Cryptomatte core: dependency-free (SPEC.md section 4).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace jdexrc {

// MurmurHash3_x86_32 (Austin Appleby, public domain).
inline uint32_t murmur3_32(const void* key, size_t len, uint32_t seed = 0) {
    const uint8_t* data = static_cast<const uint8_t*>(key);
    const size_t nblocks = len / 4;
    uint32_t h1 = seed;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    auto rotl = [](uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    for (size_t i = 0; i < nblocks; ++i) {
        const uint8_t* p = data + i * 4;
        uint32_t k1 = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        k1 *= c1; k1 = rotl(k1, 15); k1 *= c2;
        h1 ^= k1; h1 = rotl(h1, 13); h1 = h1 * 5 + 0xe6546b64u;
    }
    const uint8_t* tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= uint32_t(tail[2]) << 16; [[fallthrough]];
        case 2: k1 ^= uint32_t(tail[1]) << 8; [[fallthrough]];
        case 1: k1 ^= tail[0];
                k1 *= c1; k1 = rotl(k1, 15); k1 *= c2; h1 ^= k1;
    }
    h1 ^= uint32_t(len);
    h1 ^= h1 >> 16; h1 *= 0x85ebca6bu; h1 ^= h1 >> 13; h1 *= 0xc2b2ae35u; h1 ^= h1 >> 16;
    return h1;
}
inline uint32_t murmur3_32(const std::string& s, uint32_t seed = 0) { return murmur3_32(s.data(), s.size(), seed); }

// Exponent fix (4.3): no denormals, inf or NaN.
inline uint32_t fix_exponent(uint32_t h) {
    const uint32_t e = (h >> 23) & 0xFFu;
    if (e == 0 || e == 255) h ^= (1u << 23);
    return h;
}
// ID bits of a name (hash after the fix).
inline uint32_t name_to_id_bits(const std::string& name) { return fix_exponent(murmur3_32(name)); }
inline float bits_to_float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }
inline uint32_t float_to_bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline float name_to_id(const std::string& name) { return bits_to_float(name_to_id_bits(name)); }
inline std::string id_to_hex(uint32_t bits) { char buf[16]; std::snprintf(buf, sizeof buf, "%08x", bits); return buf; }
inline std::string name_to_hex(const std::string& name) { return id_to_hex(name_to_id_bits(name)); }
// Metadata key for type name T: first 7 hex chars (reference layer_hash).
inline std::string layer_key(const std::string& type_name) { return name_to_hex(type_name).substr(0, 7); }

enum class Mode { Priority, Holdout, Normalise };
inline bool parse_mode(const std::string& s, Mode& m) {
    if (s == "priority") m = Mode::Priority;
    else if (s == "holdout") m = Mode::Holdout;
    else if (s == "normalise" || s == "normalize") m = Mode::Normalise;
    else return false;
    return true;
}
inline float clamp01(float v) { return !(v > 0.f) ? 0.f : (v > 1.f ? 1.f : v); } // NaN -> 0

// Coverage for one pixel (4.2). m: matte values in priority order (clamped here), c: out, n values.
// Returns the background coverage 1 - sum(c), clamped at 0.
inline float coverage(const float* m, float* c, size_t n, Mode mode) {
    double sum = 0.0;
    switch (mode) {
        case Mode::Priority: {
            for (size_t i = 0; i < n; ++i) {
                double rest = 1.0 - sum; if (rest < 0.0) rest = 0.0;
                double v = std::min(double(clamp01(m[i])), rest);
                c[i] = float(v); sum += v;
            }
        } break;
        case Mode::Holdout: {
            double t = 1.0;
            for (size_t i = 0; i < n; ++i) {
                double mi = clamp01(m[i]);
                double v = mi * t; t *= (1.0 - mi);
                c[i] = float(v); sum += v;
            }
        } break;
        case Mode::Normalise: {
            double s = 0.0;
            for (size_t i = 0; i < n; ++i) s += clamp01(m[i]);
            const double k = s > 1.0 ? 1.0 / s : 1.0;
            for (size_t i = 0; i < n; ++i) { double v = clamp01(m[i]) * k; c[i] = float(v); sum += v; }
        } break;
    }
    const double bg = 1.0 - sum;
    return bg > 0.0 ? float(bg) : 0.f;
}

struct Rank { uint32_t id_bits; float cov; };

// Rank packing for one pixel (4.4). ids/c: n objects. out: `ranks` entries (id, coverage), empty = 0,0.
// Sorted by coverage descending, ties by ID bits ascending. Returns the number of objects dropped.
inline size_t pack_ranks(const uint32_t* ids, const float* c, size_t n, Rank* out, size_t ranks,
                         std::vector<Rank>& scratch) {
    scratch.clear();
    for (size_t i = 0; i < n; ++i) if (c[i] > 0.f) scratch.push_back({ids[i], c[i]});
    std::sort(scratch.begin(), scratch.end(), [](const Rank& a, const Rank& b) {
        if (a.cov != b.cov) return a.cov > b.cov;
        return a.id_bits < b.id_bits;
    });
    const size_t kept = std::min(scratch.size(), ranks);
    for (size_t i = 0; i < kept; ++i) out[i] = scratch[i];
    for (size_t i = kept; i < ranks; ++i) out[i] = {0u, 0.f};
    return scratch.size() - kept;
}

inline std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (ch < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
                else o += char(ch); // UTF-8 passes through
        }
    }
    return o;
}

// Manifest JSON (4.5): {"name":"hex8",...}, sorted by name (byte order), compact.
inline std::string manifest_json(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string o = "{";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) o += ",";
        o += "\"" + json_escape(names[i]) + "\":\"" + name_to_hex(names[i]) + "\"";
    }
    return o + "}";
}

// Type name rule (4.4): letters, digits and '_' only, not empty.
inline bool valid_type_name(const std::string& t) {
    if (t.empty()) return false;
    for (unsigned char ch : t)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    return true;
}

} // namespace jdexrc
