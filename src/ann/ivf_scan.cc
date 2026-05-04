#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <string>

#include "ann/index.h"
#include "ann/topk.h"

namespace rinha {
namespace {

constexpr std::array<uint32_t, kLogicalDim> kScanOrder = {5,  6, 2,  0, 7,  8, 11,
                                                          12, 9, 10, 1, 13, 3, 4};

uint64_t section_end(uint64_t offset, uint64_t count, uint64_t item_size) {
  if (count != 0 && item_size > (std::numeric_limits<uint64_t>::max() / count)) {
    return std::numeric_limits<uint64_t>::max();
  }
  const uint64_t bytes = count * item_size;
  if (offset > std::numeric_limits<uint64_t>::max() - bytes) {
    return std::numeric_limits<uint64_t>::max();
  }
  return offset + bytes;
}

uint64_t sqdiff(int16_t a, int16_t b) {
  const int32_t d = static_cast<int32_t>(a) - static_cast<int32_t>(b);
  return static_cast<uint64_t>(static_cast<int64_t>(d) * static_cast<int64_t>(d));
}

float centroid_distance(const int16_t q[kLogicalDim], const float* centroid) {
  float sum = 0.0f;
  for (uint32_t d = 0; d < kLogicalDim; ++d) {
    const float diff = dequantize_i16(q[d]) - centroid[d];
    sum += diff * diff;
  }
  return sum;
}

template <uint32_t MaxProbe>
FixedTopK<MaxProbe> nearest_centroids(const MappedIndex& index, const int16_t q[kLogicalDim],
                                      uint32_t nprobe) {
  FixedTopK<MaxProbe> top;
  const uint32_t limit = std::min<uint32_t>({nprobe, MaxProbe, index.header->nlist});
  for (uint32_t c = 0; c < index.header->nlist; ++c) {
    top.insert_limited(centroid_distance(q, index.centroids + static_cast<size_t>(c) * kLogicalDim),
                       c, limit);
  }
  return top;
}

uint64_t bbox_lower_bound(const MappedIndex& index, const int16_t q[kLogicalDim],
                          uint32_t cluster) {
  const int16_t* mn = index.bbox_min + static_cast<size_t>(cluster) * kLogicalDim;
  const int16_t* mx = index.bbox_max + static_cast<size_t>(cluster) * kLogicalDim;
  uint64_t sum = 0;
  for (uint32_t d = 0; d < kLogicalDim; ++d) {
    int32_t diff = 0;
    if (q[d] < mn[d]) {
      diff = static_cast<int32_t>(mn[d]) - static_cast<int32_t>(q[d]);
    } else if (q[d] > mx[d]) {
      diff = static_cast<int32_t>(q[d]) - static_cast<int32_t>(mx[d]);
    }
    sum += static_cast<uint64_t>(static_cast<int64_t>(diff) * static_cast<int64_t>(diff));
  }
  return sum;
}

void scan_range_scalar(const MappedIndex& index, uint32_t begin, uint32_t end,
                       const int16_t q[kLogicalDim], FixedTopKInt<kTopInternal>& top) {
  const uint32_t total = index.header->total_vectors;
  for (uint32_t row = begin; row < end; ++row) {
    uint64_t dist = 0;
    bool abandoned = false;
    const uint64_t worst = top.worst_dist();
    for (uint32_t dim : kScanOrder) {
      dist += sqdiff(q[dim], index.vectors[static_cast<size_t>(dim) * total + row]);
      if (dist > worst) {
        abandoned = true;
        break;
      }
    }
    if (!abandoned) top.insert(dist, row, index.orig_ids[row]);
  }
}

#if defined(__AVX2__)
void add_dim8(__m256i& lo64, __m256i& hi64, int16_t qd, const int16_t* values, uint32_t row) {
  const __m128i raw = _mm_loadu_si128(reinterpret_cast<const __m128i*>(values + row));
  const __m256i v32 = _mm256_cvtepi16_epi32(raw);
  const __m256i q32 = _mm256_set1_epi32(static_cast<int>(qd));
  const __m256i diff = _mm256_sub_epi32(v32, q32);
  const __m256i sq32 = _mm256_mullo_epi32(diff, diff);
  const __m128i low128 = _mm256_castsi256_si128(sq32);
  const __m128i high128 = _mm256_extracti128_si256(sq32, 1);
  lo64 = _mm256_add_epi64(lo64, _mm256_cvtepi32_epi64(low128));
  hi64 = _mm256_add_epi64(hi64, _mm256_cvtepi32_epi64(high128));
}

void scan_range_avx2(const MappedIndex& index, uint32_t begin, uint32_t end,
                     const int16_t q[kLogicalDim], FixedTopKInt<kTopInternal>& top) {
  const uint32_t total = index.header->total_vectors;
  alignas(32) uint64_t lo[4];
  alignas(32) uint64_t hi[4];
  uint32_t row = begin;
  const uint32_t limit = end - ((end - begin) % 8);
  for (; row < limit; row += 8) {
    __m256i lo64 = _mm256_setzero_si256();
    __m256i hi64 = _mm256_setzero_si256();
    for (uint32_t dim : kScanOrder) {
      add_dim8(lo64, hi64, q[dim], index.vectors + static_cast<size_t>(dim) * total, row);
    }
    _mm256_store_si256(reinterpret_cast<__m256i*>(lo), lo64);
    _mm256_store_si256(reinterpret_cast<__m256i*>(hi), hi64);
    for (uint32_t lane = 0; lane < 4; ++lane) {
      const uint32_t r = row + lane;
      top.insert(lo[lane], r, index.orig_ids[r]);
    }
    for (uint32_t lane = 0; lane < 4; ++lane) {
      const uint32_t r = row + 4 + lane;
      top.insert(hi[lane], r, index.orig_ids[r]);
    }
  }
  if (row < end) scan_range_scalar(index, row, end, q, top);
}
#endif

void scan_range_fast(const MappedIndex& index, uint32_t begin, uint32_t end,
                     const int16_t q[kLogicalDim], FixedTopKInt<kTopInternal>& top) {
#if defined(__AVX2__)
  scan_range_avx2(index, begin, end, q, top);
#else
  scan_range_scalar(index, begin, end, q, top);
#endif
}

SearchResult finish_result(const MappedIndex& index, const FixedTopKInt<kTopInternal>& top,
                           uint32_t used_nprobe, bool used_flat, bool used_bbox,
                           uint32_t scanned_candidates, uint32_t repaired_clusters) {
  SearchResult result;
  if (top.size < kKnn) {
    result.used_nprobe = used_nprobe;
    result.used_flat = used_flat;
    result.used_bbox = used_bbox;
    result.scanned_candidates = scanned_candidates;
    result.repaired_clusters = repaired_clusters;
    return result;
  }

  uint8_t frauds = 0;
  for (uint32_t i = 0; i < kKnn; ++i) frauds += index.labels[top.row[i]] ? 1 : 0;
  result.fraud_count = frauds;
  result.approved = frauds < 3;
  result.fraud_score = static_cast<float>(frauds) / 5.0f;
  result.d5 = static_cast<float>(top.dist[4]);
  result.d6 =
      top.size > 5 ? static_cast<float>(top.dist[5]) : std::numeric_limits<float>::infinity();
  result.used_nprobe = used_nprobe;
  result.used_flat = used_flat;
  result.used_bbox = used_bbox;
  result.scanned_candidates = scanned_candidates;
  result.repaired_clusters = repaired_clusters;
  return result;
}

SearchResult scan_probe(const MappedIndex& index, const int16_t q[kLogicalDim], uint32_t nprobe,
                        bool repair) {
  constexpr uint32_t kMaxProbe = 512;
  nprobe = std::min<uint32_t>({nprobe, kMaxProbe, index.header->nlist});
  if (nprobe == 0) nprobe = 1;

  auto probes = nearest_centroids<kMaxProbe>(index, q, nprobe);
  FixedTopKInt<kTopInternal> top;
  std::array<uint8_t, kMaxNList> scanned{};
  uint32_t scanned_candidates = 0;

  for (uint32_t i = 0; i < probes.size && i < nprobe; ++i) {
    const uint32_t cluster = probes.id[i];
    if (cluster >= index.header->nlist) continue;
    scanned[cluster] = 1;
    const uint32_t begin = index.offsets[cluster];
    const uint32_t end = index.offsets[cluster + 1];
    if (end <= begin) continue;
    scan_range_fast(index, begin, end, q, top);
    scanned_candidates += end - begin;
  }

  uint32_t repaired_clusters = 0;
  if (repair) {
    for (uint32_t cluster = 0; cluster < index.header->nlist; ++cluster) {
      if (scanned[cluster]) continue;
      const uint32_t begin = index.offsets[cluster];
      const uint32_t end = index.offsets[cluster + 1];
      if (end <= begin) continue;
      if (bbox_lower_bound(index, q, cluster) <= top.worst_dist()) {
        scan_range_fast(index, begin, end, q, top);
        scanned_candidates += end - begin;
        ++repaired_clusters;
      }
    }
  }

  return finish_result(index, top, nprobe, false, repair, scanned_candidates, repaired_clusters);
}

bool ambiguous(const SearchResult& result, float margin_threshold) {
  if (result.fraud_count == 2 || result.fraud_count == 3) return true;
  return result.d6 < std::numeric_limits<float>::infinity() &&
         (result.d6 - result.d5) <= margin_threshold;
}

bool low_margin(const SearchResult& result, float margin_threshold) {
  return result.d6 < std::numeric_limits<float>::infinity() &&
         (result.d6 - result.d5) <= margin_threshold;
}

void quantize_query(const std::array<float, kPaddedDim>& query, int16_t out[kLogicalDim]) {
  for (uint32_t d = 0; d < kLogicalDim; ++d) out[d] = quantize_i16(query[d]);
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
  uint32_t bits = 0;
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
  const bool valid_header =
      std::memcmp(header->magic, kIndexMagic, sizeof(header->magic)) == 0 && header->version == 1 &&
      header->logical_dim == kLogicalDim && header->stored_dim == kLogicalDim &&
      header->nlist > 0 && header->nlist <= kMaxNList && header->total_vectors > 0 &&
      header->k == kKnn && header->file_size == static_cast<uint64_t>(st.st_size) &&
      header->scale > 9999.0f && header->scale < 10001.0f;
  if (!valid_header) {
    if (error) *error = "invalid ivfi16 header";
    munmap(mapping, static_cast<size_t>(st.st_size));
    close(fd);
    return false;
  }

  const uint64_t nlist = header->nlist;
  const uint64_t n = header->total_vectors;
  const bool sections_ok =
      section_end(header->centroid_offset, nlist * kLogicalDim, sizeof(float)) <=
          header->file_size &&
      section_end(header->bbox_min_offset, nlist * kLogicalDim, sizeof(int16_t)) <=
          header->file_size &&
      section_end(header->bbox_max_offset, nlist * kLogicalDim, sizeof(int16_t)) <=
          header->file_size &&
      section_end(header->list_offsets_offset, nlist + 1, sizeof(uint32_t)) <= header->file_size &&
      section_end(header->vectors_offset, n * kLogicalDim, sizeof(int16_t)) <= header->file_size &&
      section_end(header->labels_offset, n, sizeof(uint8_t)) <= header->file_size &&
      section_end(header->orig_ids_offset, n, sizeof(uint32_t)) <= header->file_size;
  if (!sections_ok) {
    if (error) *error = "invalid ivfi16 section offsets";
    munmap(mapping, static_cast<size_t>(st.st_size));
    close(fd);
    return false;
  }

  const auto* base = static_cast<const uint8_t*>(mapping);
  const auto* offsets = reinterpret_cast<const uint32_t*>(base + header->list_offsets_offset);
  if (offsets[0] != 0 || offsets[header->nlist] != header->total_vectors) {
    if (error) *error = "invalid ivfi16 list offsets";
    munmap(mapping, static_cast<size_t>(st.st_size));
    close(fd);
    return false;
  }
  for (uint32_t i = 0; i < header->nlist; ++i) {
    if (offsets[i] > offsets[i + 1]) {
      if (error) *error = "non-monotonic ivfi16 list offsets";
      munmap(mapping, static_cast<size_t>(st.st_size));
      close(fd);
      return false;
    }
  }

  index.fd = fd;
  index.mapping = mapping;
  index.mapping_size = static_cast<size_t>(st.st_size);
  index.header = header;
  index.centroids = reinterpret_cast<const float*>(base + header->centroid_offset);
  index.bbox_min = reinterpret_cast<const int16_t*>(base + header->bbox_min_offset);
  index.bbox_max = reinterpret_cast<const int16_t*>(base + header->bbox_max_offset);
  index.offsets = offsets;
  index.vectors = reinterpret_cast<const int16_t*>(base + header->vectors_offset);
  index.labels = reinterpret_cast<const uint8_t*>(base + header->labels_offset);
  index.orig_ids = reinterpret_cast<const uint32_t*>(base + header->orig_ids_offset);
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
  int16_t q[kLogicalDim];
  quantize_query(query, q);

  const bool base_repair = params.bbox_mode == BBoxMode::kAlways;
  SearchResult base = scan_probe(index, q, params.base_nprobe, base_repair);
  if (!ambiguous(base, params.margin_threshold)) return base;

  const bool expanded_repair = params.bbox_mode != BBoxMode::kOff;
  SearchResult expanded = scan_probe(index, q, params.ambig_nprobe, expanded_repair);
  if (params.exact_fallback &&
      (expanded.approved != base.approved || low_margin(expanded, params.margin_threshold))) {
    SearchResult exact = flat_search(index, query);
    exact.used_nprobe = expanded.used_nprobe;
    return exact;
  }
  return expanded;
}

}  // namespace rinha
