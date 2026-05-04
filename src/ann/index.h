#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rinha {

constexpr uint32_t kLogicalDim = 14;
constexpr uint32_t kPaddedDim = 16;
constexpr uint32_t kKnn = 5;
constexpr uint32_t kTopInternal = 5;
constexpr uint32_t kMaxNList = 16384;
constexpr float kFixedScale = 10000.0f;

constexpr char kIndexMagic[8] = {'I', 'V', 'F', 'I', '1', '6', '0', '\0'};
constexpr char kReferencesSha256[] =
    "24a1fd588e2598ab62de9ac7ac73408e571589e00da4243d7afc9d0f01878f77";

enum class BBoxMode : uint32_t {
  kOff = 0,
  kAmbiguousOnly = 1,
  kAlways = 2,
  kBoundaryOnly = 3,
};

struct IndexHeader {
  char magic[8];
  uint32_t version;
  uint32_t logical_dim;
  uint32_t stored_dim;
  uint32_t nlist;
  uint32_t total_vectors;
  uint32_t k;
  uint32_t flags;
  float scale;
  uint32_t reserved_u32;
  uint64_t centroid_offset;
  uint64_t bbox_min_offset;
  uint64_t bbox_max_offset;
  uint64_t list_offsets_offset;
  uint64_t vectors_offset;
  uint64_t labels_offset;
  uint64_t orig_ids_offset;
  uint64_t file_size;
  char references_sha256[65];
  char reserved[75];
};

static_assert(sizeof(IndexHeader) == 256);

struct MappedIndex {
  int fd = -1;
  void* mapping = nullptr;
  size_t mapping_size = 0;
  const IndexHeader* header = nullptr;
  const float* centroids = nullptr;
  const int16_t* bbox_min = nullptr;
  const int16_t* bbox_max = nullptr;
  const uint32_t* offsets = nullptr;
  const int16_t* vectors = nullptr;
  const uint8_t* labels = nullptr;
  const uint32_t* orig_ids = nullptr;
};

struct SearchParams {
  uint32_t base_nprobe = 20;
  uint32_t ambig_nprobe = 40;
  float margin_threshold = 0.0f;
  bool full_warmup = true;
  bool heuristic_only = false;
  bool exact_fallback = false;
  bool fast_path = false;
  BBoxMode bbox_mode = BBoxMode::kAmbiguousOnly;
};

struct SearchResult {
  uint8_t fraud_count = 0;
  bool approved = true;
  float fraud_score = 0.0f;
  float d5 = 0.0f;
  float d6 = 0.0f;
  uint32_t used_nprobe = 0;
  bool used_flat = false;
  bool used_bbox = false;
  uint32_t scanned_candidates = 0;
  uint32_t repaired_clusters = 0;
};

bool load_index(std::string_view path, MappedIndex& index, std::string* error);
void close_index(MappedIndex& index);
bool warmup_index(const MappedIndex& index);

SearchResult search_index(const MappedIndex& index, const std::array<float, kPaddedDim>& query,
                          const SearchParams& params);
SearchResult flat_search(const MappedIndex& index, const std::array<float, kPaddedDim>& query);

uint16_t float_to_half(float value);
float half_to_float(uint16_t value);

inline float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

inline int16_t quantize_i16(float value) {
  if (value < -1.0f) value = -1.0f;
  if (value > 1.0f) value = 1.0f;
  float scaled = value * kFixedScale;
  scaled += scaled >= 0.0f ? 0.5f : -0.5f;
  if (scaled < -10000.0f) scaled = -10000.0f;
  if (scaled > 10000.0f) scaled = 10000.0f;
  return static_cast<int16_t>(scaled);
}

inline float dequantize_i16(int16_t value) {
  return static_cast<float>(value) / kFixedScale;
}

inline uint32_t bin_clamped(float value, uint32_t bins) {
  float v = clamp01(value);
  uint32_t b = static_cast<uint32_t>(v * static_cast<float>(bins));
  return b >= bins ? bins - 1 : b;
}

inline int fast_path_fraud_count(const std::array<float, kPaddedDim>& v) {
  const float score = 5.0f * v[2] + 5.0f * v[7] + 2.0f * v[8] + v[9] + v[11] + v[12] + v[0];
  if (score < 1.95f) return 0;
  if (score > 11.1f) return 5;
  return -1;
}

inline int heuristic_fraud_count(const std::array<float, kPaddedDim>& v) {
  return v[2] >= 0.0695191446f ? 5 : 0;
}

}  // namespace rinha
