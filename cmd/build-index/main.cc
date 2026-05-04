#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "ann/index.h"

namespace {

/** Command-line configuration for the index builder. */
struct Args {
  /// Gzip-compressed references dataset path.
  std::string references = "resources/references.json.gz";

  /// Destination `.ivf16` index path.
  std::string out = "build/fraud.ivf16";
};

/** Parses `--references` and `--out` flags. */
Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string_view a(argv[i]);
    if (a == "--references" && i + 1 < argc) args.references = argv[++i];
    if (a == "--out" && i + 1 < argc) args.out = argv[++i];
  }
  return args;
}

/** Decompresses the full gzip file into memory and appends a trailing NUL byte. */
std::vector<char> read_gzip(const std::string& path) {
  gzFile file = gzopen(path.c_str(), "rb");
  if (!file) throw std::runtime_error("failed to open gzip: " + path);
  std::vector<char> data;
  std::array<char, 1 << 20> buffer{};
  while (true) {
    int n = gzread(file, buffer.data(), static_cast<unsigned>(buffer.size()));
    if (n < 0) {
      int err = 0;
      const char* msg = gzerror(file, &err);
      gzclose(file);
      throw std::runtime_error(std::string("gzread failed: ") + (msg ? msg : ""));
    }
    if (n == 0) break;
    data.insert(data.end(), buffer.data(), buffer.data() + n);
  }
  gzclose(file);
  data.push_back('\0');
  return data;
}

/** Finds a required token in the JSON stream or throws with context. */
const char* skip_to(const char* p, const char* needle) {
  const char* found = std::strstr(p, needle);
  if (!found) throw std::runtime_error(std::string("missing token: ") + needle);
  return found;
}

/**
 * Extracts reference vectors and labels from the generated references JSON.
 *
 * The parser is specialized for the references file shape and avoids building a full DOM.
 */
void parse_references(const std::vector<char>& json,
                      std::vector<std::array<float, rinha::kPaddedDim>>& vectors,
                      std::vector<uint8_t>& labels) {
  const char* p = json.data();
  vectors.reserve(3'000'000);
  labels.reserve(3'000'000);

  while ((p = std::strstr(p, "\"vector\"")) != nullptr) {
    p = std::strchr(p, '[');
    if (!p) throw std::runtime_error("malformed vector");
    ++p;

    std::array<float, rinha::kPaddedDim> v{};
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      char* next = nullptr;
      v[d] = std::strtof(p, &next);
      if (next == p) throw std::runtime_error("malformed float in vector");
      p = next;
      while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    }
    v[14] = 0.0f;
    v[15] = 0.0f;

    p = skip_to(p, "\"label\"");
    p = std::strchr(p, ':');
    if (!p) throw std::runtime_error("malformed label");
    ++p;
    while (*p == ' ' || *p == '"') ++p;
    const uint8_t label = std::strncmp(p, "fraud", 5) == 0 ? 1 : 0;

    vectors.push_back(v);
    labels.push_back(label);
  }
}

/** Chooses the feature dimension with highest variance for a balanced split. */
uint32_t choose_split_dim(const std::vector<std::array<float, rinha::kPaddedDim>>& vectors,
                          const std::vector<uint32_t>& indices, size_t begin, size_t end) {
  std::array<double, rinha::kLogicalDim> sum{};
  std::array<double, rinha::kLogicalDim> sum_sq{};
  const double n = static_cast<double>(end - begin);
  for (size_t i = begin; i < end; ++i) {
    const auto& v = vectors[indices[i]];
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      sum[d] += v[d];
      sum_sq[d] += static_cast<double>(v[d]) * static_cast<double>(v[d]);
    }
  }
  uint32_t best = 0;
  double best_var = -1.0;
  for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
    const double mean = sum[d] / n;
    const double var = sum_sq[d] / n - mean * mean;
    if (var > best_var) {
      best_var = var;
      best = d;
    }
  }
  return best;
}

/**
 * Recursively partitions vectors into `2^14` balanced leaves.
 *
 * The resulting leaf ids become IVF bucket assignments.
 */
void split_balanced(const std::vector<std::array<float, rinha::kPaddedDim>>& vectors,
                    std::vector<uint32_t>& indices, std::vector<uint16_t>& assignment, size_t begin,
                    size_t end, uint32_t depth, uint32_t leaf_base) {
  if (depth == 14) {
    for (size_t i = begin; i < end; ++i) assignment[indices[i]] = static_cast<uint16_t>(leaf_base);
    return;
  }
  const uint32_t dim = choose_split_dim(vectors, indices, begin, end);
  const size_t mid = begin + (end - begin) / 2;
  std::nth_element(indices.begin() + static_cast<std::ptrdiff_t>(begin),
                   indices.begin() + static_cast<std::ptrdiff_t>(mid),
                   indices.begin() + static_cast<std::ptrdiff_t>(end),
                   [&](uint32_t a, uint32_t b) { return vectors[a][dim] < vectors[b][dim]; });
  const uint32_t child_base = leaf_base << 1;
  split_balanced(vectors, indices, assignment, begin, mid, depth + 1, child_base);
  split_balanced(vectors, indices, assignment, mid, end, depth + 1, child_base | 1u);
}

/** Assigns every reference vector to a balanced IVF leaf. */
std::vector<uint16_t> assign_balanced_leaves(
    const std::vector<std::array<float, rinha::kPaddedDim>>& vectors) {
  std::vector<uint32_t> indices(vectors.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::vector<uint16_t> assignment(vectors.size());
  split_balanced(vectors, indices, assignment, 0, indices.size(), 0, 0);
  return assignment;
}

/** Counts vectors per list and accumulates vector sums for centroid construction. */
void accumulate_lists(const std::vector<std::array<float, rinha::kPaddedDim>>& vectors,
                      const std::vector<uint16_t>& assignment, std::vector<uint32_t>& counts,
                      std::vector<double>& sums) {
  for (uint32_t i = 0; i < vectors.size(); ++i) {
    const uint32_t bucket = assignment[i];
    ++counts[bucket];
    for (uint32_t d = 0; d < rinha::kPaddedDim; ++d) {
      sums[static_cast<size_t>(bucket) * rinha::kPaddedDim + d] += vectors[i][d];
    }
  }
}

/** Builds one centroid per IVF list, using deterministic synthetic centers for empty lists. */
std::vector<float> build_centroids(const std::vector<uint32_t>& counts,
                                   const std::vector<double>& sums) {
  std::vector<float> centroids(static_cast<size_t>(rinha::kNList) * rinha::kPaddedDim);
  for (uint32_t bucket = 0; bucket < rinha::kNList; ++bucket) {
    auto fallback = rinha::bucket_center(bucket);
    for (uint32_t d = 0; d < rinha::kPaddedDim; ++d) {
      if (counts[bucket] == 0) {
        centroids[static_cast<size_t>(bucket) * rinha::kPaddedDim + d] = fallback[d];
      } else {
        centroids[static_cast<size_t>(bucket) * rinha::kPaddedDim + d] =
            static_cast<float>(sums[static_cast<size_t>(bucket) * rinha::kPaddedDim + d] /
                               static_cast<double>(counts[bucket]));
      }
    }
  }
  return centroids;
}

/** Converts per-list counts into prefix offsets for contiguous vector storage. */
std::vector<uint32_t> prefix_offsets(const std::vector<uint32_t>& counts) {
  std::vector<uint32_t> offsets(rinha::kNList + 1);
  for (uint32_t i = 0; i < rinha::kNList; ++i) offsets[i + 1] = offsets[i] + counts[i];
  return offsets;
}

/** Prints distribution diagnostics for generated IVF lists. */
void print_histogram(const std::vector<uint32_t>& counts) {
  uint32_t non_empty = 0;
  uint32_t max_count = 0;
  uint64_t total = 0;
  for (uint32_t c : counts) {
    if (c) ++non_empty;
    max_count = std::max(max_count, c);
    total += c;
  }
  std::vector<uint32_t> sorted = counts;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double p) {
    size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
  };
  std::cerr << "vectors=" << total << " non_empty_lists=" << non_empty
            << " empty_lists=" << (rinha::kNList - non_empty) << " p50=" << pct(0.50)
            << " p95=" << pct(0.95) << " p99=" << pct(0.99) << " max=" << max_count << "\n";
}

/**
 * Writes the final mmap-friendly index file.
 *
 * Vectors are sorted by assigned list, converted to half-float, and paired with compact labels.
 */
void write_index(const std::string& path, const std::vector<float>& centroids,
                 const std::vector<uint32_t>& offsets, const std::vector<uint16_t>& assignment,
                 const std::vector<std::array<float, rinha::kPaddedDim>>& vectors,
                 const std::vector<uint8_t>& labels) {
  std::vector<uint32_t> cursor = offsets;
  std::vector<uint16_t> sorted_vectors(static_cast<size_t>(vectors.size()) * rinha::kPaddedDim);
  std::vector<uint8_t> sorted_labels(labels.size());

  for (uint32_t i = 0; i < vectors.size(); ++i) {
    const uint32_t bucket = assignment[i];
    const uint32_t pos = cursor[bucket]++;
    for (uint32_t d = 0; d < rinha::kPaddedDim; ++d) {
      sorted_vectors[static_cast<size_t>(pos) * rinha::kPaddedDim + d] =
          rinha::float_to_half(vectors[i][d]);
    }
    sorted_labels[pos] = labels[i];
  }

  rinha::IndexHeader header{};
  std::memcpy(header.magic, rinha::kIndexMagic, sizeof(header.magic));
  header.version = 1;
  header.logical_dim = rinha::kLogicalDim;
  header.padded_dim = rinha::kPaddedDim;
  header.nlist = rinha::kNList;
  header.total_vectors = static_cast<uint32_t>(vectors.size());
  header.k = rinha::kKnn;
  header.centroid_offset = sizeof(rinha::IndexHeader);
  header.list_offsets_offset = header.centroid_offset + centroids.size() * sizeof(float);
  header.vectors_offset = header.list_offsets_offset + offsets.size() * sizeof(uint32_t);
  header.labels_offset = header.vectors_offset + sorted_vectors.size() * sizeof(uint16_t);
  header.file_size = header.labels_offset + sorted_labels.size() * sizeof(uint8_t);
  std::memcpy(header.references_sha256, rinha::kReferencesSha256, sizeof(rinha::kReferencesSha256));

  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("failed to create index: " + path);
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(centroids.data()),
            static_cast<std::streamsize>(centroids.size() * sizeof(float)));
  out.write(reinterpret_cast<const char*>(offsets.data()),
            static_cast<std::streamsize>(offsets.size() * sizeof(uint32_t)));
  out.write(reinterpret_cast<const char*>(sorted_vectors.data()),
            static_cast<std::streamsize>(sorted_vectors.size() * sizeof(uint16_t)));
  out.write(reinterpret_cast<const char*>(sorted_labels.data()),
            static_cast<std::streamsize>(sorted_labels.size() * sizeof(uint8_t)));
  if (!out) throw std::runtime_error("failed while writing index");
}

}  // namespace

int main(int argc, char** argv) {
  std::setlocale(LC_ALL, "C");
  try {
    Args args = parse_args(argc, argv);
    std::cerr << "reading " << args.references << "\n";
    auto json = read_gzip(args.references);
    std::cerr << "decompressed_bytes=" << (json.size() - 1) << "\n";

    std::vector<std::array<float, rinha::kPaddedDim>> vectors;
    std::vector<uint8_t> labels;
    parse_references(json, vectors, labels);

    std::cerr << "assigning balanced IVF leaves\n";
    auto assignment = assign_balanced_leaves(vectors);
    std::vector<uint32_t> counts(rinha::kNList);
    std::vector<double> sums(static_cast<size_t>(rinha::kNList) * rinha::kPaddedDim);
    accumulate_lists(vectors, assignment, counts, sums);
    print_histogram(counts);

    auto centroids = build_centroids(counts, sums);
    auto offsets = prefix_offsets(counts);
    write_index(args.out, centroids, offsets, assignment, vectors, labels);
    std::cerr << "wrote " << args.out << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "build-index failed: " << e.what() << "\n";
    return 1;
  }
}
