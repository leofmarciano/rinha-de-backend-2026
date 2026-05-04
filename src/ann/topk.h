#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace rinha {

/**
 * Fixed-capacity sorted top-k container for nearest-neighbor distances.
 *
 * Insertions keep `dist` and `id` sorted ascending by distance. The container performs no dynamic
 * allocation and is intended for hot search loops.
 *
 * @tparam Capacity Maximum number of entries retained.
 */
template <uint32_t Capacity>
struct FixedTopK {
  /// Sorted distances for retained entries.
  std::array<float, Capacity> dist{};

  /// Payload ids matching `dist`.
  std::array<uint32_t, Capacity> id{};

  /// Number of valid entries currently retained.
  uint32_t size = 0;

  /// Initializes all distance slots to infinity.
  FixedTopK() {
    for (uint32_t i = 0; i < Capacity; ++i) {
      dist[i] = std::numeric_limits<float>::infinity();
      id[i] = std::numeric_limits<uint32_t>::max();
    }
  }

  /// Returns true when `(d, value)` should sort before `(other_d, other_value)`.
  static bool better(float d, uint32_t value, float other_d, uint32_t other_value) {
    return d < other_d || (d == other_d && value < other_value);
  }

  /**
   * Inserts an item if it belongs in the retained top-k set.
   *
   * @param d Distance used for ordering.
   * @param value Payload id associated with the distance.
   */
  void insert(float d, uint32_t value) {
    insert_limited(d, value, Capacity);
  }

  /**
   * Inserts an item while retaining at most `limit` entries.
   *
   * This avoids sorting a larger buffer than the caller will actually consume.
   */
  void insert_limited(float d, uint32_t value, uint32_t limit) {
    const uint32_t cap = limit < Capacity ? limit : Capacity;
    if (cap == 0) return;
    if (size == cap && !better(d, value, dist[cap - 1], id[cap - 1])) return;
    uint32_t pos = size < cap ? size++ : cap - 1;
    while (pos > 0 && better(d, value, dist[pos - 1], id[pos - 1])) {
      dist[pos] = dist[pos - 1];
      id[pos] = id[pos - 1];
      --pos;
    }
    dist[pos] = d;
    id[pos] = value;
  }
};

template <uint32_t Capacity>
struct FixedTopKInt {
  std::array<uint64_t, Capacity> dist{};
  std::array<uint32_t, Capacity> row{};
  std::array<uint32_t, Capacity> orig_id{};
  uint32_t size = 0;
  uint32_t worst_slot = 0;
  uint64_t worst_dist_value = std::numeric_limits<uint64_t>::max();
  uint32_t worst_orig_id_value = std::numeric_limits<uint32_t>::max();

  FixedTopKInt() {
    for (uint32_t i = 0; i < Capacity; ++i) {
      dist[i] = std::numeric_limits<uint64_t>::max();
      row[i] = std::numeric_limits<uint32_t>::max();
      orig_id[i] = std::numeric_limits<uint32_t>::max();
    }
  }

  static bool better(uint64_t d, uint32_t original_id, uint64_t other_d,
                     uint32_t other_original_id) {
    return d < other_d || (d == other_d && original_id < other_original_id);
  }

  static bool worse(uint64_t d, uint32_t original_id, uint64_t other_d,
                    uint32_t other_original_id) {
    return d > other_d || (d == other_d && original_id > other_original_id);
  }

  void recompute_worst() {
    uint32_t w = 0;
    for (uint32_t i = 1; i < Capacity; ++i) {
      if (worse(dist[i], orig_id[i], dist[w], orig_id[w])) w = i;
    }
    worst_slot = w;
    worst_dist_value = dist[w];
    worst_orig_id_value = orig_id[w];
  }

  void insert(uint64_t d, uint32_t row_id, uint32_t original_id) {
    if (size < Capacity) {
      const uint32_t pos = size++;
      dist[pos] = d;
      row[pos] = row_id;
      orig_id[pos] = original_id;
      if (size == Capacity) recompute_worst();
      return;
    }
    if (!better(d, original_id, worst_dist_value, worst_orig_id_value)) return;
    dist[worst_slot] = d;
    row[worst_slot] = row_id;
    orig_id[worst_slot] = original_id;
    recompute_worst();
  }

  uint64_t worst_dist() const {
    return size < Capacity ? std::numeric_limits<uint64_t>::max() : worst_dist_value;
  }
};

}  // namespace rinha
