// jd_exrc shared declarations (SPEC.md sections 3-5).
#pragma once
#include "crypto_core.h"

#include <ImathBox.h>
#include <ImfCompression.h>
#include <ImfHeader.h>
#include <ImfPixelType.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jdexrc {

constexpr const char* VERSION = "1.0.0";
enum { EXIT_OK = 0, EXIT_CHECK = 1, EXIT_BADJOB = 2, EXIT_IO = 3 };

// ---------------------------------------------------------------- logging (plain or one JSON object per line)
void log_set_json(bool on);
bool log_set_file(const std::string& path);  // also write every log line to this file (UTF-8 path)
void log_raw(const std::string& text);       // stdout + log file
bool log_is_json();
void log_line(const std::string& event, const std::string& msg, int frame = INT32_MIN, const std::string& file = "");

// ---------------------------------------------------------------- images
struct Plane {
    Imath::Box2i dw;          // pixels cover dw (inclusive)
    std::vector<float> px;    // row-major over dw
    int w() const { return dw.max.x - dw.min.x + 1; }
    int h() const { return dw.max.y - dw.min.y + 1; }
};

// One input file (one frame): EXR (single or multi-part) or PNG.
class Source {
public:
    static std::unique_ptr<Source> open(const std::string& path, std::string& err);
    virtual ~Source() = default;
    std::string path;
    Imath::Box2i dw, disp;                 // of the first part
    const Imf::Header* header = nullptr;   // first part header (EXR only), for metadata
    std::vector<std::string> channels;     // every channel name, all parts
    // Find the real channel for (source layer prefix, requested channel); "" if none. Accepts R/r/red, the Y<->R
    // fallback for single-channel images, and a lone channel when R/Y is asked.
    std::string resolve(const std::string& layer, const std::string& ch) const;
    // Read one real channel as float over its own data window.
    virtual bool read(const std::string& channel, Plane& out, std::string& err) = 0;
    // Read (layer, ch) with "L" = Rec.709 luma of R, G, B.
    bool read_spec(const std::string& layer, const std::string& ch, Plane& out, std::string& err);
};

// Copy src into a buffer laid out over dst_dw (pixels outside src stay as they are).
void blit(const Plane& src, const Imath::Box2i& dst_dw, std::vector<float>& dst);

// ---------------------------------------------------------------- sequences
std::string normalize_pattern(const std::string& p);   // %04d / %d -> ####
bool has_frames(const std::string& p);
std::string expand_frame(const std::string& p, int frame);
std::vector<int> list_frames(const std::string& pattern); // frames found on disk, sorted

// ---------------------------------------------------------------- job (SPEC 5)
struct LayerSpec {
    std::string name, src, src_layer;
    bool main = false;
    std::vector<std::pair<std::string, std::string>> map;  // source channel -> output channel; empty = auto
    Imf::PixelType type = Imf::HALF;
};
struct ObjectSpec { std::string name, src, src_layer, channel = "R"; };
struct CryptoSpec {
    std::string name = "CryptoMatte", background;
    int ranks = 6;
    Mode mode = Mode::Priority;
    bool preview = false, sidecar = false;
    std::vector<ObjectSpec> objects;
};
struct Job {
    std::string output, frames = "auto", metadata_from;
    Imf::Compression compression = Imf::ZIP_COMPRESSION;
    bool multipart = false, hold_missing = false, store_job = true;
    std::vector<LayerSpec> layers;
    std::vector<CryptoSpec> cryptos;
    std::string json;  // the job as compact JSON, stored in the output metadata
};

bool parse_compression(const std::string& s, Imf::Compression& c);
const char* compression_name(Imf::Compression c);
bool is_lossy(Imf::Compression c);
bool parse_map(const std::string& s, std::vector<std::pair<std::string, std::string>>& map, std::string& err);
Imf::PixelType default_type(const std::string& layer_name, const std::vector<std::pair<std::string, std::string>>& map);

bool job_from_json_text(const std::string& text, Job& job, std::string& err);
bool job_from_file(const std::string& path, Job& job, std::string& err);
std::string job_to_json(const Job& job);
bool validate_job(Job& job, std::string& err);

struct RunOptions { std::string frames; int threads = 0; bool dry_run = false, check = false; };
int run_job(const Job& job, const RunOptions& opt);

// ---------------------------------------------------------------- check (SPEC 8.7)
int check_file(const std::string& path, bool json, std::string& report);

} // namespace jdexrc
