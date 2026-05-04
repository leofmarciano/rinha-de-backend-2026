#include <limits>

#include "ann/index.h"
#include "ann/topk.h"

namespace rinha {
namespace {

/** Computes squared L2 distance between a float query and one half-float reference vector. */
float l2_f16_flat(const std::array<float, kPaddedDim>& q, const uint16_t* v) {
  float sum = 0.0f;
  for (uint32_t i = 0; i < kPaddedDim; ++i) {
    const float d = q[i] - half_to_float(v[i]);
    sum += d * d;
  }
  return sum;
}

}  // namespace

SearchResult flat_search(const MappedIndex& index, const std::array<float, kPaddedDim>& query) {
  FixedTopK<kTopInternal> top;
  for (uint32_t pos = 0; pos < index.header->total_vectors; ++pos) {
    const uint16_t* v = index.vectors + (static_cast<size_t>(pos) * kPaddedDim);
    top.insert(l2_f16_flat(query, v), pos);
  }

  uint8_t frauds = 0;
  for (uint32_t i = 0; i < kKnn; ++i) frauds += index.labels[top.id[i]] ? 1 : 0;

  SearchResult result;
  result.fraud_count = frauds;
  result.fraud_score = static_cast<float>(frauds) / 5.0f;
  result.approved = frauds < 3;
  result.d5 = top.dist[4];
  result.d6 = top.size > 5 ? top.dist[5] : std::numeric_limits<float>::infinity();
  result.used_flat = true;
  return result;
}

}  // namespace rinha
