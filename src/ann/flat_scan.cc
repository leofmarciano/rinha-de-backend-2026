#include <limits>

#include "ann/index.h"
#include "ann/topk.h"

namespace rinha {
namespace {

uint64_t sqdiff(int16_t a, int16_t b) {
  const int32_t d = static_cast<int32_t>(a) - static_cast<int32_t>(b);
  return static_cast<uint64_t>(static_cast<int64_t>(d) * static_cast<int64_t>(d));
}

}  // namespace

SearchResult flat_search(const MappedIndex& index, const std::array<float, kPaddedDim>& query) {
  int16_t q[kLogicalDim];
  for (uint32_t d = 0; d < kLogicalDim; ++d) q[d] = quantize_i16(query[d]);

  FixedTopKInt<kTopInternal> top;
  const uint32_t total = index.header->total_vectors;
  for (uint32_t row = 0; row < total; ++row) {
    uint64_t dist = 0;
    for (uint32_t dim = 0; dim < kLogicalDim; ++dim) {
      dist += sqdiff(q[dim], index.vectors[static_cast<size_t>(dim) * total + row]);
    }
    top.insert(dist, row, index.orig_ids[row]);
  }

  uint8_t frauds = 0;
  for (uint32_t i = 0; i < kKnn; ++i) frauds += index.labels[top.row[i]] ? 1 : 0;

  SearchResult result;
  result.fraud_count = frauds;
  result.approved = frauds < 3;
  result.fraud_score = static_cast<float>(frauds) / 5.0f;
  result.d5 = static_cast<float>(top.dist[4]);
  result.d6 =
      top.size > 5 ? static_cast<float>(top.dist[5]) : std::numeric_limits<float>::infinity();
  result.used_flat = true;
  result.scanned_candidates = total;
  return result;
}

}  // namespace rinha
