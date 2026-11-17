// 4Dwarper analysis data: one flat, versioned blob (saved in the AE project inside an ARBITRARY param).
// Everything is resolution independent: time in frames, H and field vectors in normalised coordinates
// (u = (x - (w-1)/2) / (w/2), v = (y - (h-1)/2) / (w/2)). Flat = no pointers, so flatten/unflatten are memcpy.
//
// Version 2 (4Dwarper 0.3.0): per target frame the solve gives the homography of BOTH whole source frames around
// the solved source time (Ha for floor(src), Hb for floor(src)+1) and one residual field for each of them.
// Layout:  BlobHeader | frame_count x [ FrameRecord | field A | field B ] | pin_count x Pin
// Each field is grid_w*grid_h pairs of float (du, dv), normalised units, row-major (0,0 = no field).
// Readers use header_bytes / record_bytes / pin_bytes as strides. A blob of another version is NOT read: the
// effect asks the user to analyse again (never a crash).
#pragma once
#include <cstdint>
#include <cstring>

namespace w4d {

const uint32_t kMagic = 0x31443457;   // 'W4D1' little-endian
const uint32_t kVersion = 2;

struct BlobHeader {
  uint32_t magic, version;
  uint32_t header_bytes;     // sizeof(BlobHeader) when written
  uint32_t record_bytes;     // stride of one frame record including its two fields
  uint32_t frame_count;
  uint32_t pin_count;        // user pins (v2 morph); 0 for now
  uint32_t pin_bytes;        // stride of one Pin
  uint32_t flags;
  double first_time_s;       // layer time of record 0, seconds
  double frame_dur_s;        // seconds per frame at analysis time
  uint32_t source_frames;    // source frames the solve could use (0..source_frames-1 on the record grid); 0 = any
  uint32_t mode;             // 1 = time + space, 2 = space only, 3 = time only
  uint32_t source_first;     // first source frame (record grid) inside the source layer's in/out
  uint32_t pad0;
  uint32_t reserved[8];      // status reason text
};

struct FrameRecord {
  double src_frame;          // source time for this target frame, float frames (same frame grid as the target)
  double Hn[9];              // blend of Ha and Hb at the fractional time (kept for simple readers)
  double Ha[9];              // target -> source frame floor(src_frame), normalised, row-major
  double Hb[9];              // target -> source frame floor(src_frame)+1
  float quality;             // 0..1, reserved
  uint32_t flags;
  uint32_t grid_w, grid_h;   // field resolution (0,0 = none); field A then field B follow this record
};

struct Pin {                 // reserved for v2 morph: a user correspondence, normalised coordinates
  double frame;
  float tu, tv, su, sv;      // target point, source point
  float weight; uint32_t flags;
};

inline size_t record_stride(uint32_t gw, uint32_t gh) { return sizeof(FrameRecord) + size_t(gw) * gh * 2 * 2 * sizeof(float); }
inline size_t blob_size(uint32_t frames, uint32_t gw, uint32_t gh, uint32_t pins = 0) {
  return sizeof(BlobHeader) + size_t(frames) * record_stride(gw, gh) + size_t(pins) * sizeof(Pin);
}
inline void init_header(BlobHeader* h, uint32_t frames, uint32_t gw, uint32_t gh, double first_s, double dur_s) {
  std::memset(h, 0, sizeof(*h));
  h->magic = kMagic; h->version = kVersion; h->header_bytes = sizeof(BlobHeader);
  h->record_bytes = uint32_t(record_stride(gw, gh));
  h->frame_count = frames; h->pin_bytes = sizeof(Pin); h->first_time_s = first_s; h->frame_dur_s = dur_s;
  h->mode = 1;
}
// Result state shown by the status banner: low byte of BlobHeader::flags, short reason text in BlobHeader::reserved.
enum Status : uint32_t { kNone = 0, kSolved = 1, kFailed = 2 };
inline void set_status(BlobHeader* h, uint32_t status, const char* reason) {
  h->flags = (h->flags & ~0xFFu) | (status & 0xFFu);
  std::memset(h->reserved, 0, sizeof(h->reserved));
  if (reason) std::strncpy((char*)h->reserved, reason, sizeof(h->reserved) - 1);
}
inline uint32_t status_of(const BlobHeader* h) {
  uint32_t s = h->flags & 0xFFu;
  return s == kNone && h->frame_count ? uint32_t(kSolved) : s;
}
inline const char* reason_of(const BlobHeader* h) { return (const char*)h->reserved; }

// A 4Dwarper blob of an older version that holds an analysis: show "analyse again", do not read it.
inline bool old_version(const void* p, size_t bytes) {
  if (!p || bytes < 3 * sizeof(uint32_t)) return false;
  const uint32_t* u = (const uint32_t*)p;
  return u[0] == kMagic && u[1] != kVersion && bytes >= 5 * sizeof(uint32_t) && u[4] > 0;
}
inline bool valid(const void* p, size_t bytes) {
  if (!p || bytes < sizeof(BlobHeader)) return false;
  const BlobHeader* h = (const BlobHeader*)p;
  if (h->magic != kMagic || h->version != kVersion || h->header_bytes < sizeof(BlobHeader) || h->record_bytes < sizeof(FrameRecord))
    return false;
  if (size_t(h->header_bytes) + size_t(h->frame_count) * h->record_bytes > bytes) return false;
  return true;
}
inline FrameRecord* record(void* p, uint32_t i) {
  BlobHeader* h = (BlobHeader*)p;
  return (FrameRecord*)((char*)p + h->header_bytes + size_t(i) * h->record_bytes);
}
inline const FrameRecord* record(const void* p, uint32_t i) { return record(const_cast<void*>(p), i); }
// Fields of a record; nullptr when the record has none or the stride is too small for them.
inline const float* field_a(const BlobHeader* h, const FrameRecord* r) {
  if (!r->grid_w || !r->grid_h || h->record_bytes < record_stride(r->grid_w, r->grid_h)) return nullptr;
  return (const float*)(r + 1);
}
inline const float* field_b(const BlobHeader* h, const FrameRecord* r) {
  const float* a = field_a(h, r);
  return a ? a + size_t(r->grid_w) * r->grid_h * 2 : nullptr;
}

}  // namespace w4d
