#include "crypto_core.h"
#include <cmath>
#include <cstdio>
using namespace jdexrc;
static int g_pass = 0, g_fail = 0;
#define CHECK(x) do { if (x) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x); } } while (0)
static bool near_(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main() {
    // MurmurHash3_x86_32, seed 0
    CHECK(murmur3_32(std::string("")) == 0x00000000u);
    CHECK(murmur3_32(std::string("hello")) == 0x248bfa47u);
    CHECK(murmur3_32(std::string("foo")) == 0xf6a5c420u);
    // seeded vectors (widely published)
    CHECK(murmur3_32(std::string(""), 1) == 0x514e28b7u);
    CHECK(murmur3_32(std::string("The quick brown fox jumps over the lazy dog"), 0x9747b28cu) == 0x2fa826cdu);

    // exponent fix
    CHECK(fix_exponent(0x00000000u) == 0x00800000u);
    CHECK(fix_exponent(0x7f800001u) == 0x7f000001u);
    CHECK(fix_exponent(0xff800000u) == 0xff000000u);
    CHECK(fix_exponent(0x80000001u) == 0x80800001u);
    CHECK(fix_exponent(0x248bfa47u) == 0x248bfa47u);
    CHECK(name_to_hex("") == "00800000");
    CHECK(name_to_hex("hello") == "248bfa47");
    CHECK(float_to_bits(name_to_id("hello")) == 0x248bfa47u);
    CHECK(layer_key("hello") == "248bfa4");
    CHECK(layer_key("CryptoMatte").size() == 7);
    std::printf("info: layer_key(CryptoMatte)=%s hex(foo)=%s\n", layer_key("CryptoMatte").c_str(), name_to_hex("foo").c_str());

    // coverage modes
    float c[4];
    { float m[2] = {0.5f, 0.5f}; float bg = coverage(m, c, 2, Mode::Priority);
      CHECK(near_(c[0], 0.5f) && near_(c[1], 0.5f) && near_(bg, 0.f)); }
    { float m[2] = {1.f, 1.f}; coverage(m, c, 2, Mode::Priority);
      CHECK(near_(c[0], 1.f) && near_(c[1], 0.f)); }
    { float m[3] = {0.7f, 0.6f, 0.9f}; float bg = coverage(m, c, 3, Mode::Priority);
      CHECK(near_(c[0], 0.7f) && near_(c[1], 0.3f) && near_(c[2], 0.f) && near_(bg, 0.f)); }
    { float m[2] = {0.5f, 0.5f}; float bg = coverage(m, c, 2, Mode::Holdout);
      CHECK(near_(c[0], 0.5f) && near_(c[1], 0.25f) && near_(bg, 0.25f)); }
    { float m[2] = {1.f, 1.f}; coverage(m, c, 2, Mode::Normalise);
      CHECK(near_(c[0], 0.5f) && near_(c[1], 0.5f)); }
    { float m[2] = {0.2f, 0.3f}; float bg = coverage(m, c, 2, Mode::Normalise);
      CHECK(near_(c[0], 0.2f) && near_(c[1], 0.3f) && near_(bg, 0.5f)); }
    { float m[2] = {2.f, -1.f}; float bg = coverage(m, c, 2, Mode::Priority); // clamp
      CHECK(near_(c[0], 1.f) && near_(c[1], 0.f) && near_(bg, 0.f)); }
    Mode md = Mode::Normalise;
    CHECK(parse_mode("priority", md) && md == Mode::Priority);
    CHECK(parse_mode("holdout", md) && md == Mode::Holdout);
    CHECK(!parse_mode("bogus", md));

    // rank packing
    std::vector<Rank> scratch;
    { uint32_t ids[4] = {40, 10, 30, 20}; float cv[4] = {0.1f, 0.5f, 0.f, 0.4f}; Rank r[4];
      size_t d = pack_ranks(ids, cv, 4, r, 4, scratch);
      CHECK(d == 0 && r[0].id_bits == 10 && r[1].id_bits == 20 && r[2].id_bits == 40);
      CHECK(r[3].id_bits == 0 && r[3].cov == 0.f); }
    { uint32_t ids[3] = {30, 10, 20}; float cv[3] = {0.25f, 0.25f, 0.5f}; Rank r[2];
      size_t d = pack_ranks(ids, cv, 3, r, 2, scratch); // tie by ID bits, one dropped
      CHECK(d == 1 && r[0].id_bits == 20 && r[1].id_bits == 10); }

    // manifest + names
    CHECK(manifest_json({"hello", ""}) == "{\"\":\"00800000\",\"hello\":\"248bfa47\"}");
    CHECK(manifest_json({"b", "a"}).find("\"a\"") < manifest_json({"b", "a"}).find("\"b\""));
    CHECK(manifest_json({"q\"x"}).find("q\\\"x") != std::string::npos);
    CHECK(valid_type_name("CryptoMatte") && !valid_type_name("Crypto.Matte") && !valid_type_name(""));

    std::printf("test_core: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
