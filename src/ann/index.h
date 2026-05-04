#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rinha {

/// Number of meaningful dimensions in the fraud feature vector.
constexpr uint32_t kLogicalDim = 14;

/// Physical vector width stored and searched by the ANN index.
constexpr uint32_t kPaddedDim = 16;

/// Number of IVF lists/buckets in the generated index.
constexpr uint32_t kNList = 16384;

/// Number of nearest neighbors used to decide the fraud score.
constexpr uint32_t kKnn = 5;

/// Internal top-k size, keeping one extra neighbor for margin checks.
constexpr uint32_t kTopInternal = 6;

/// Magic header written at the beginning of every `.ivf16` index file.
constexpr char kIndexMagic[8] = {'F', 'I', 'V', 'F', '1', '6', '0', '\0'};

/// Expected SHA-256 of the decompressed references dataset used to build the index.
constexpr char kReferencesSha256[] =
    "24a1fd588e2598ab62de9ac7ac73408e571589e00da4243d7afc9d0f01878f77";

/**
 * On-disk header for the mmaped IVF/f16 index.
 *
 * All offsets are byte offsets from the beginning of the file. The fixed 256-byte size keeps the
 * layout stable and leaves reserved space for future metadata without moving the data sections.
 */
struct IndexHeader {
  /// File magic, expected to match `kIndexMagic`.
  char magic[8];

  /// Index format version.
  uint32_t version;

  /// Number of non-padding feature dimensions.
  uint32_t logical_dim;

  /// Number of stored dimensions per vector.
  uint32_t padded_dim;

  /// Number of IVF lists.
  uint32_t nlist;

  /// Number of indexed reference vectors.
  uint32_t total_vectors;

  /// Neighbor count used by search.
  uint32_t k;

  /// Reserved for future alignment-compatible metadata.
  uint32_t reserved_u32;

  /// Offset of the contiguous `float` centroid matrix.
  uint64_t centroid_offset;

  /// Offset of the `uint32_t` list-offset array with `nlist + 1` entries.
  uint64_t list_offsets_offset;

  /// Offset of the contiguous `uint16_t` half-float vector matrix.
  uint64_t vectors_offset;

  /// Offset of the contiguous `uint8_t` label array.
  uint64_t labels_offset;

  /// Full file size expected by the loader.
  uint64_t file_size;

  /// Null-terminated SHA-256 of the decompressed references dataset.
  char references_sha256[65];

  /// Reserved padding to keep the header exactly 256 bytes.
  char reserved[111];
};

static_assert(sizeof(IndexHeader) == 256);

/**
 * In-memory view of an mmaped index file.
 *
 * The pointer fields borrow from `mapping`; they must not be freed individually. Release the whole
 * structure with `close_index`.
 */
struct MappedIndex {
  /// Open file descriptor backing the mapping.
  int fd = -1;

  /// Raw `mmap` base address.
  void* mapping = nullptr;

  /// Mapping length in bytes.
  size_t mapping_size = 0;

  /// Parsed and validated file header.
  const IndexHeader* header = nullptr;

  /// Centroid matrix with `nlist * kPaddedDim` floats.
  const float* centroids = nullptr;

  /// IVF list offsets with `nlist + 1` entries.
  const uint32_t* offsets = nullptr;

  /// Reference vectors stored as IEEE-754 binary16 values.
  const uint16_t* vectors = nullptr;

  /// Reference labels where non-zero means fraud.
  const uint8_t* labels = nullptr;
};

/// Search tuning parameters shared by the API and validation executable.
struct SearchParams {
  /// Number of centroid lists to scan first.
  uint32_t base_nprobe = 256;

  /// Number of centroid lists to scan for ambiguous base results.
  uint32_t ambig_nprobe = 512;

  /// Distance margin threshold used to decide whether to escalate search.
  float margin_threshold = 0.0f;

  /// Whether startup should touch mapped pages to reduce first-request latency.
  bool full_warmup = true;

  /// Whether to bypass ANN and use the lightweight heuristic classifier.
  bool heuristic_only = false;

  /// Whether ambiguous expanded probes may fall back to a full flat scan.
  bool exact_fallback = true;
};

/// Search decision plus diagnostics used by validation and tuning.
struct SearchResult {
  /// Number of fraud labels among the `kKnn` nearest neighbors.
  uint8_t fraud_count = 0;

  /// Final approval decision.
  bool approved = true;

  /// Fraud score exposed by the HTTP API.
  float fraud_score = 0.0f;

  /// Distance to the fifth nearest neighbor.
  float d5 = 0.0f;

  /// Distance to the sixth nearest neighbor, used as a margin signal.
  float d6 = 0.0f;

  /// Number of IVF lists used by the final result.
  uint32_t used_nprobe = 0;

  /// Whether the exact flat fallback was used.
  bool used_flat = false;
};

/**
 * Opens and validates an index file, mapping it read-only into memory.
 *
 * @param path Filesystem path to the `.ivf16` file.
 * @param index Destination mapping handle.
 * @param error Optional error message destination.
 * @return `true` when the index was opened, mapped and structurally validated.
 */
bool load_index(std::string_view path, MappedIndex& index, std::string* error);

/** Releases an index previously opened by `load_index`. */
void close_index(MappedIndex& index);

/**
 * Touches all pages in a mapped index.
 *
 * @return `false` when the index is not mapped.
 */
bool warmup_index(const MappedIndex& index);

/**
 * Runs the tuned ANN search path for one normalized query vector.
 *
 * The function scans `base_nprobe`, expands to `ambig_nprobe` for ambiguous results, and may fall
 * back to exact flat search when the expanded result is still low-margin.
 */
SearchResult search_index(const MappedIndex& index, const std::array<float, kPaddedDim>& query,
                          const SearchParams& params);

/** Runs an exact linear scan over all reference vectors. */
SearchResult flat_search(const MappedIndex& index, const std::array<float, kPaddedDim>& query);

/** Converts a 32-bit float to IEEE-754 binary16 bits. */
uint16_t float_to_half(float value);

/** Converts IEEE-754 binary16 bits to a 32-bit float. */
float half_to_float(uint16_t value);

/** Clamps a scalar to the `[0, 1]` feature range. */
inline float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

/** Maps a normalized scalar to a bounded integer bin. */
inline uint32_t bin_clamped(float value, uint32_t bins) {
  float v = clamp01(value);
  uint32_t b = static_cast<uint32_t>(v * static_cast<float>(bins));
  return b >= bins ? bins - 1 : b;
}

/**
 * Quantizes a normalized feature vector to the deterministic IVF bucket id used by the builder.
 */
inline uint32_t quantize_bucket(const std::array<float, kPaddedDim>& v) {
  uint32_t id = 0;
  id = (id << 2) | bin_clamped(v[0], 4);                      // amount
  id = (id << 3) | bin_clamped(v[2], 8);                      // amount_vs_avg
  id = (id << 3) | bin_clamped(v[7], 8);                      // km_from_home
  id = (id << 2) | bin_clamped(v[8], 4);                      // tx_count_24h
  id = (id << 1) | (v[9] >= 0.5f ? 1u : 0u);                  // online
  id = (id << 1) | (v[11] >= 0.5f ? 1u : 0u);                 // unknown merchant
  id = (id << 1) | (v[12] >= 0.55f ? 1u : 0u);                // risk band
  id = (id << 1) | ((v[5] < 0.0f || v[6] < 0.0f) ? 1u : 0u);  // no last tx
  return id & (kNList - 1);
}

/**
 * Fast classifier for clearly safe or clearly fraudulent vectors.
 *
 * @return fraud count in `[0, 5]`, or `-1` when the ANN path should decide.
 */
inline int fast_path_fraud_count(const std::array<float, kPaddedDim>& v) {
  const float score = 5.0f * v[2] + 5.0f * v[7] + 2.0f * v[8] + v[9] + v[11] + v[12] + v[0];
  if (score < 2.3337f) return 0;
  if (score > 9.5548f) return 5;
  return -1;
}

/**
 * Lightweight threshold classifier used for constrained local runtime profiles.
 */
inline int heuristic_fraud_count(const std::array<float, kPaddedDim>& v) {
  return v[2] >= 0.0695191446f ? 5 : 0;
}

/**
 * Returns a synthetic centroid for an empty deterministic bucket.
 */
inline std::array<float, kPaddedDim> bucket_center(uint32_t id) {
  std::array<float, kPaddedDim> c{};
  const uint32_t missing = id & 1u;
  id >>= 1;
  const uint32_t risk = id & 1u;
  id >>= 1;
  const uint32_t unknown = id & 1u;
  id >>= 1;
  const uint32_t online = id & 1u;
  id >>= 1;
  const uint32_t tx = id & 3u;
  id >>= 2;
  const uint32_t km = id & 7u;
  id >>= 3;
  const uint32_t amount_avg = id & 7u;
  id >>= 3;
  const uint32_t amount = id & 3u;

  c[0] = (static_cast<float>(amount) + 0.5f) / 4.0f;
  c[1] = 0.5f;
  c[2] = (static_cast<float>(amount_avg) + 0.5f) / 8.0f;
  c[3] = 0.5f;
  c[4] = 0.5f;
  c[5] = missing ? -1.0f : 0.5f;
  c[6] = missing ? -1.0f : 0.5f;
  c[7] = (static_cast<float>(km) + 0.5f) / 8.0f;
  c[8] = (static_cast<float>(tx) + 0.5f) / 4.0f;
  c[9] = static_cast<float>(online);
  c[10] = online ? 0.0f : 1.0f;
  c[11] = static_cast<float>(unknown);
  c[12] = risk ? 0.8f : 0.25f;
  c[13] = 0.05f;
  c[14] = 0.0f;
  c[15] = 0.0f;
  return c;
}

}  // namespace rinha
