#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>

#include "ann/index.h"
#include "ann/topk.h"

namespace rinha {
namespace {

/** Computes squared L2 distance between a query vector and a float centroid. */
float l2_f32(const std::array<float, kPaddedDim>& q, const float* c) {
  float sum = 0.0f;
  for (uint32_t i = 0; i < kPaddedDim; ++i) {
    const float d = q[i] - c[i];
    sum += d * d;
  }
  return sum;
}

/** Computes squared L2 distance between a query vector and a half-float reference vector. */
float l2_f16(const std::array<float, kPaddedDim>& q, const uint16_t* v) {
  float sum = 0.0f;
  for (uint32_t i = 0; i < kPaddedDim; ++i) {
    const float d = q[i] - half_to_float(v[i]);
    sum += d * d;
  }
  return sum;
}

/**
 * Finds the closest centroids to a query.
 *
 * @tparam MaxProbe Compile-time storage bound for retained centroids.
 */
template <uint32_t MaxProbe>
FixedTopK<MaxProbe> nearest_centroids(const MappedIndex& index,
                                      const std::array<float, kPaddedDim>& query, uint32_t nprobe) {
  FixedTopK<MaxProbe> top;
  const uint32_t limit = std::min<uint32_t>(nprobe, MaxProbe);
  for (uint32_t i = 0; i < index.header->nlist; ++i) {
    top.insert_limited(l2_f32(query, index.centroids + (static_cast<size_t>(i) * kPaddedDim)), i,
                       limit);
  }
  return top;
}

/**
 * Scans the nearest IVF lists and classifies from the nearest `kKnn` reference labels.
 */
SearchResult scan_probe(const MappedIndex& index, const std::array<float, kPaddedDim>& query,
                        uint32_t nprobe) {
  constexpr uint32_t kMaxProbe = 1024;
  if (nprobe > kMaxProbe) nprobe = kMaxProbe;
  auto centroids = nearest_centroids<kMaxProbe>(index, query, nprobe);
  FixedTopK<kTopInternal> top;
  auto scan_bucket = [&](uint32_t bucket) {
    const uint32_t begin = index.offsets[bucket];
    const uint32_t end = index.offsets[bucket + 1];
    for (uint32_t pos = begin; pos < end; ++pos) {
      const uint16_t* v = index.vectors + (static_cast<size_t>(pos) * kPaddedDim);
      top.insert(l2_f16(query, v), pos);
    }
  };

  for (uint32_t i = 0; i < centroids.size && i < nprobe; ++i) scan_bucket(centroids.id[i]);

  if (top.size < kKnn) return flat_search(index, query);

  uint8_t frauds = 0;
  for (uint32_t i = 0; i < kKnn; ++i) frauds += index.labels[top.id[i]] ? 1 : 0;

  SearchResult result;
  result.fraud_count = frauds;
  result.fraud_score = static_cast<float>(frauds) / 5.0f;
  result.approved = frauds < 3;
  result.d5 = top.dist[4];
  result.d6 = top.size > 5 ? top.dist[5] : std::numeric_limits<float>::infinity();
  result.used_nprobe = nprobe;
  return result;
}

/** Returns whether a search result is close enough to the decision boundary to expand. */
bool ambiguous(const SearchResult& result, float margin_threshold) {
  if (result.fraud_count == 2 || result.fraud_count == 3) return true;
  if (result.d6 < std::numeric_limits<float>::infinity() &&
      (result.d6 - result.d5) <= margin_threshold) {
    return true;
  }
  return false;
}

/** Returns whether the fifth and sixth neighbors are too close to trust the ANN margin. */
bool low_margin(const SearchResult& result, float margin_threshold) {
  return result.d6 < std::numeric_limits<float>::infinity() &&
         (result.d6 - result.d5) <= margin_threshold;
}

}  // namespace

uint16_t float_to_half(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  int32_t exp = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
  uint32_t mant = bits & 0x7fffffu;

  if (exp <= 0) {
    if (exp < -10) return static_cast<uint16_t>(sign);
    mant |= 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - exp);
    uint32_t half_mant = mant >> shift;
    if ((mant >> (shift - 1)) & 1u) ++half_mant;
    return static_cast<uint16_t>(sign | half_mant);
  }
  if (exp >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
  uint32_t half = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
  if (mant & 0x1000u) ++half;
  return static_cast<uint16_t>(half);
}

float half_to_float(uint16_t value) {
  const uint32_t sign = (static_cast<uint32_t>(value & 0x8000u)) << 16;
  uint32_t exp = (value >> 10) & 0x1fu;
  uint32_t mant = value & 0x03ffu;
  uint32_t bits;

  if (exp == 0) {
    if (mant == 0) {
      bits = sign;
    } else {
      exp = 1;
      while ((mant & 0x0400u) == 0) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x03ffu;
      bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
  } else if (exp == 31) {
    bits = sign | 0x7f800000u | (mant << 13);
  } else {
    bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
  }

  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

bool load_index(std::string_view path, MappedIndex& index, std::string* error) {
  std::string p(path);
  int fd = open(p.c_str(), O_RDONLY);
  if (fd < 0) {
    if (error) *error = "open failed: " + std::string(std::strerror(errno));
    return false;
  }

  struct stat st = {};
  if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(IndexHeader))) {
    if (error) *error = "fstat failed or file too small";
    close(fd);
    return false;
  }

  void* mapping = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    if (error) *error = "mmap failed: " + std::string(std::strerror(errno));
    close(fd);
    return false;
  }

  const auto* header = static_cast<const IndexHeader*>(mapping);
  if (std::memcmp(header->magic, kIndexMagic, sizeof(header->magic)) != 0 || header->version != 1 ||
      header->logical_dim != kLogicalDim || header->padded_dim != kPaddedDim ||
      header->nlist != kNList || header->k != kKnn ||
      header->file_size != static_cast<uint64_t>(st.st_size)) {
    if (error) *error = "invalid index header";
    munmap(mapping, static_cast<size_t>(st.st_size));
    close(fd);
    return false;
  }

  const uint64_t needed = header->labels_offset + static_cast<uint64_t>(header->total_vectors);
  if (needed > header->file_size) {
    if (error) *error = "invalid index offsets";
    munmap(mapping, static_cast<size_t>(st.st_size));
    close(fd);
    return false;
  }

  const auto* base = static_cast<const uint8_t*>(mapping);
  index.fd = fd;
  index.mapping = mapping;
  index.mapping_size = static_cast<size_t>(st.st_size);
  index.header = header;
  index.centroids = reinterpret_cast<const float*>(base + header->centroid_offset);
  index.offsets = reinterpret_cast<const uint32_t*>(base + header->list_offsets_offset);
  index.vectors = reinterpret_cast<const uint16_t*>(base + header->vectors_offset);
  index.labels = reinterpret_cast<const uint8_t*>(base + header->labels_offset);
  return true;
}

void close_index(MappedIndex& index) {
  if (index.mapping) munmap(index.mapping, index.mapping_size);
  if (index.fd >= 0) close(index.fd);
  index = MappedIndex{};
}

bool warmup_index(const MappedIndex& index) {
  if (!index.mapping || index.mapping_size == 0) return false;
  volatile uint8_t sink = 0;
  const auto* p = static_cast<const uint8_t*>(index.mapping);
  constexpr size_t page = 4096;
  for (size_t i = 0; i < index.mapping_size; i += page) sink ^= p[i];
  sink ^= p[index.mapping_size - 1];
  return true;
}

SearchResult search_index(const MappedIndex& index, const std::array<float, kPaddedDim>& query,
                          const SearchParams& params) {
  SearchResult base = scan_probe(index, query, params.base_nprobe);
  if (!ambiguous(base, params.margin_threshold)) return base;

  SearchResult expanded = scan_probe(index, query, params.ambig_nprobe);
  if (params.exact_fallback &&
      (expanded.approved != base.approved || low_margin(expanded, params.margin_threshold))) {
    SearchResult exact = flat_search(index, query);
    exact.used_nprobe = params.ambig_nprobe;
    exact.used_flat = true;
    return exact;
  }
  return expanded;
}

}  // namespace rinha
