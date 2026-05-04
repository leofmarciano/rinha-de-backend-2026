#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <clocale>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include "ann/index.h"

namespace {

struct Args {
  std::string references = "resources/references.json.gz";
  std::string out = "build/index_k8192.ivfi16";
  std::string method = "balanced";
  uint32_t nlist = 8192;
  uint32_t kmeans_sample = 131072;
  uint32_t kmeans_iters = 8;
};

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string_view a(argv[i]);
    if (a == "--references" && i + 1 < argc) args.references = argv[++i];
    if (a == "--out" && i + 1 < argc) args.out = argv[++i];
    if (a == "--method" && i + 1 < argc) args.method = argv[++i];
    if (a == "--nlist" && i + 1 < argc)
      args.nlist = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    if (a == "--kmeans-sample" && i + 1 < argc)
      args.kmeans_sample = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    if (a == "--kmeans-iters" && i + 1 < argc)
      args.kmeans_iters = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
  }
  return args;
}

uint32_t power_of_two_depth(uint32_t nlist) {
  if (nlist == 0 || nlist > rinha::kMaxNList || (nlist & (nlist - 1)) != 0) {
    throw std::runtime_error("--nlist must be a power of two in [1, 16384]");
  }
  uint32_t depth = 0;
  while ((1u << depth) < nlist) ++depth;
  return depth;
}

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

const char* skip_to(const char* p, const char* needle) {
  const char* found = std::strstr(p, needle);
  if (!found) throw std::runtime_error(std::string("missing token: ") + needle);
  return found;
}

void parse_references(const std::vector<char>& json,
                      std::vector<std::array<float, rinha::kLogicalDim>>& vectors,
                      std::vector<uint8_t>& labels) {
  const char* p = json.data();
  vectors.reserve(3'000'000);
  labels.reserve(3'000'000);

  while ((p = std::strstr(p, "\"vector\"")) != nullptr) {
    p = std::strchr(p, '[');
    if (!p) throw std::runtime_error("malformed vector");
    ++p;

    std::array<float, rinha::kLogicalDim> v{};
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      char* next = nullptr;
      v[d] = std::strtof(p, &next);
      if (next == p) throw std::runtime_error("malformed float in vector");
      p = next;
      while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    }

    p = skip_to(p, "\"label\"");
    p = std::strchr(p, ':');
    if (!p) throw std::runtime_error("malformed label");
    ++p;
    while (*p == ' ' || *p == '"') ++p;
    labels.push_back(std::strncmp(p, "fraud", 5) == 0 ? 1 : 0);
    vectors.push_back(v);

    if (vectors.size() % 500000 == 0) std::cerr << "parsed=" << vectors.size() << "\n";
  }
}

uint32_t choose_split_dim(const std::vector<std::array<float, rinha::kLogicalDim>>& vectors,
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

void split_balanced(const std::vector<std::array<float, rinha::kLogicalDim>>& vectors,
                    std::vector<uint32_t>& indices, std::vector<uint32_t>& assignment, size_t begin,
                    size_t end, uint32_t depth, uint32_t target_depth, uint32_t leaf_base) {
  if (depth == target_depth || end - begin <= 1) {
    const uint32_t leaf = leaf_base << (target_depth - depth);
    for (size_t i = begin; i < end; ++i) assignment[indices[i]] = leaf;
    return;
  }

  const uint32_t dim = choose_split_dim(vectors, indices, begin, end);
  const size_t mid = begin + (end - begin) / 2;
  std::nth_element(indices.begin() + static_cast<std::ptrdiff_t>(begin),
                   indices.begin() + static_cast<std::ptrdiff_t>(mid),
                   indices.begin() + static_cast<std::ptrdiff_t>(end),
                   [&](uint32_t a, uint32_t b) { return vectors[a][dim] < vectors[b][dim]; });
  split_balanced(vectors, indices, assignment, begin, mid, depth + 1, target_depth, leaf_base << 1);
  split_balanced(vectors, indices, assignment, mid, end, depth + 1, target_depth,
                 (leaf_base << 1) | 1u);
}

std::vector<uint32_t> assign_lists(
    const std::vector<std::array<float, rinha::kLogicalDim>>& vectors, uint32_t nlist) {
  std::vector<uint32_t> indices(vectors.size());
  std::iota(indices.begin(), indices.end(), 0);
  std::vector<uint32_t> assignment(vectors.size());
  split_balanced(vectors, indices, assignment, 0, indices.size(), 0, power_of_two_depth(nlist), 0);
  return assignment;
}

void accumulate(const std::vector<std::array<float, rinha::kLogicalDim>>& vectors,
                const std::vector<uint32_t>& assignment, uint32_t nlist,
                std::vector<uint32_t>& counts, std::vector<double>& sums) {
  counts.assign(nlist, 0);
  sums.assign(static_cast<size_t>(nlist) * rinha::kLogicalDim, 0.0);
  for (uint32_t row = 0; row < vectors.size(); ++row) {
    const uint32_t list = assignment[row];
    ++counts[list];
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      sums[static_cast<size_t>(list) * rinha::kLogicalDim + d] += vectors[row][d];
    }
  }
}

std::vector<float> build_centroids(uint32_t nlist, const std::vector<uint32_t>& counts,
                                   const std::vector<double>& sums) {
  std::vector<float> centroids(static_cast<size_t>(nlist) * rinha::kLogicalDim, 0.0f);
  for (uint32_t list = 0; list < nlist; ++list) {
    if (counts[list] == 0) continue;
    const double inv = 1.0 / static_cast<double>(counts[list]);
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      centroids[static_cast<size_t>(list) * rinha::kLogicalDim + d] =
          static_cast<float>(sums[static_cast<size_t>(list) * rinha::kLogicalDim + d] * inv);
    }
  }
  return centroids;
}

float centroid_distance(const std::array<float, rinha::kLogicalDim>& vector,
                        const std::vector<float>& centroids, uint32_t centroid) {
  const float* c = centroids.data() + static_cast<size_t>(centroid) * rinha::kLogicalDim;
  float sum = 0.0f;
  for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
    const float diff = vector[d] - c[d];
    sum += diff * diff;
  }
  return sum;
}

uint32_t nearest_centroid(const std::array<float, rinha::kLogicalDim>& vector,
                          const std::vector<float>& centroids, uint32_t nlist) {
  uint32_t best = 0;
  float best_dist = centroid_distance(vector, centroids, 0);
  for (uint32_t c = 1; c < nlist; ++c) {
    const float dist = centroid_distance(vector, centroids, c);
    if (dist < best_dist) {
      best_dist = dist;
      best = c;
    }
  }
  return best;
}

std::vector<uint32_t> deterministic_sample(size_t total, uint32_t requested) {
  const uint32_t sample_size =
      static_cast<uint32_t>(std::min<size_t>(total, std::max<uint32_t>(requested, 1)));
  std::vector<uint32_t> sample(sample_size);
  for (uint32_t i = 0; i < sample_size; ++i) {
    sample[i] = static_cast<uint32_t>((static_cast<uint64_t>(i) * total) / sample_size);
  }
  return sample;
}

std::vector<float> train_kmeans(
    const std::vector<std::array<float, rinha::kLogicalDim>>& vectors, uint32_t nlist,
    uint32_t sample_count, uint32_t iters) {
  std::cerr << "initializing kmeans with balanced centroids nlist=" << nlist << "\n";
  auto initial_assignment = assign_lists(vectors, nlist);
  std::vector<uint32_t> counts;
  std::vector<double> sums;
  accumulate(vectors, initial_assignment, nlist, counts, sums);
  auto centroids = build_centroids(nlist, counts, sums);

  auto sample = deterministic_sample(vectors.size(), sample_count);
  std::cerr << "training kmeans sample=" << sample.size() << " iters=" << iters << "\n";
  for (uint32_t iter = 0; iter < iters; ++iter) {
    counts.assign(nlist, 0);
    sums.assign(static_cast<size_t>(nlist) * rinha::kLogicalDim, 0.0);
    double inertia = 0.0;
    for (uint32_t row : sample) {
      const uint32_t list = nearest_centroid(vectors[row], centroids, nlist);
      ++counts[list];
      inertia += centroid_distance(vectors[row], centroids, list);
      for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
        sums[static_cast<size_t>(list) * rinha::kLogicalDim + d] += vectors[row][d];
      }
    }
    uint32_t moved = 0;
    for (uint32_t list = 0; list < nlist; ++list) {
      if (counts[list] == 0) continue;
      const double inv = 1.0 / static_cast<double>(counts[list]);
      for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
        const size_t pos = static_cast<size_t>(list) * rinha::kLogicalDim + d;
        const float next = static_cast<float>(sums[pos] * inv);
        if (next != centroids[pos]) ++moved;
        centroids[pos] = next;
      }
    }
    std::cerr << "kmeans_iter=" << (iter + 1) << " inertia=" << inertia
              << " updated_dims=" << moved << "\n";
  }
  return centroids;
}

std::vector<uint32_t> assign_to_centroids(
    const std::vector<std::array<float, rinha::kLogicalDim>>& vectors,
    const std::vector<float>& centroids, uint32_t nlist) {
  std::vector<uint32_t> assignment(vectors.size());
  for (uint32_t row = 0; row < vectors.size(); ++row) {
    assignment[row] = nearest_centroid(vectors[row], centroids, nlist);
    if ((row + 1) % 500000 == 0) std::cerr << "assigned=" << (row + 1) << "\n";
  }
  return assignment;
}

std::vector<uint32_t> prefix_offsets(const std::vector<uint32_t>& counts) {
  std::vector<uint32_t> offsets(counts.size() + 1);
  for (size_t i = 0; i < counts.size(); ++i) offsets[i + 1] = offsets[i] + counts[i];
  return offsets;
}

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
  std::cerr << "vectors=" << total << " lists=" << counts.size() << " non_empty=" << non_empty
            << " empty=" << (counts.size() - non_empty) << " p50=" << pct(0.50)
            << " p95=" << pct(0.95) << " p99=" << pct(0.99) << " max=" << max_count << "\n";
}

uint64_t align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

void pad_to(std::ofstream& out, uint64_t& pos, uint64_t target) {
  static constexpr std::array<char, 64> zeros{};
  while (pos < target) {
    const uint64_t take = std::min<uint64_t>(zeros.size(), target - pos);
    out.write(zeros.data(), static_cast<std::streamsize>(take));
    pos += take;
  }
}

template <typename T>
void write_span(std::ofstream& out, uint64_t& pos, const std::vector<T>& values) {
  const uint64_t bytes = static_cast<uint64_t>(values.size() * sizeof(T));
  out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(bytes));
  pos += bytes;
}

void write_index(const std::string& path, uint32_t nlist, const std::vector<float>& centroids,
                 const std::vector<uint32_t>& offsets, const std::vector<uint32_t>& assignment,
                 const std::vector<std::array<float, rinha::kLogicalDim>>& vectors,
                 const std::vector<uint8_t>& labels) {
  const uint32_t total = static_cast<uint32_t>(vectors.size());
  std::vector<uint32_t> cursor = offsets;
  std::vector<int16_t> sorted_vectors(static_cast<size_t>(total) * rinha::kLogicalDim);
  std::vector<uint8_t> sorted_labels(total);
  std::vector<uint32_t> sorted_orig_ids(total);
  std::vector<int16_t> bbox_min(static_cast<size_t>(nlist) * rinha::kLogicalDim,
                                std::numeric_limits<int16_t>::max());
  std::vector<int16_t> bbox_max(static_cast<size_t>(nlist) * rinha::kLogicalDim,
                                std::numeric_limits<int16_t>::min());

  for (uint32_t original = 0; original < total; ++original) {
    const uint32_t list = assignment[original];
    const uint32_t row = cursor[list]++;
    sorted_labels[row] = labels[original];
    sorted_orig_ids[row] = original;
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      const int16_t qv = rinha::quantize_i16(vectors[original][d]);
      sorted_vectors[static_cast<size_t>(d) * total + row] = qv;
      int16_t& mn = bbox_min[static_cast<size_t>(list) * rinha::kLogicalDim + d];
      int16_t& mx = bbox_max[static_cast<size_t>(list) * rinha::kLogicalDim + d];
      mn = std::min(mn, qv);
      mx = std::max(mx, qv);
    }
  }

  for (uint32_t list = 0; list < nlist; ++list) {
    if (offsets[list] != offsets[list + 1]) continue;
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      const int16_t q =
          rinha::quantize_i16(centroids[static_cast<size_t>(list) * rinha::kLogicalDim + d]);
      bbox_min[static_cast<size_t>(list) * rinha::kLogicalDim + d] = q;
      bbox_max[static_cast<size_t>(list) * rinha::kLogicalDim + d] = q;
    }
  }

  rinha::IndexHeader header{};
  std::memcpy(header.magic, rinha::kIndexMagic, sizeof(header.magic));
  header.version = 1;
  header.logical_dim = rinha::kLogicalDim;
  header.stored_dim = rinha::kLogicalDim;
  header.nlist = nlist;
  header.total_vectors = total;
  header.k = rinha::kKnn;
  header.scale = rinha::kFixedScale;

  uint64_t pos = sizeof(rinha::IndexHeader);
  header.centroid_offset = align_up(pos, 64);
  pos = header.centroid_offset + static_cast<uint64_t>(centroids.size() * sizeof(float));
  header.bbox_min_offset = align_up(pos, 64);
  pos = header.bbox_min_offset + static_cast<uint64_t>(bbox_min.size() * sizeof(int16_t));
  header.bbox_max_offset = align_up(pos, 64);
  pos = header.bbox_max_offset + static_cast<uint64_t>(bbox_max.size() * sizeof(int16_t));
  header.list_offsets_offset = align_up(pos, 64);
  pos = header.list_offsets_offset + static_cast<uint64_t>(offsets.size() * sizeof(uint32_t));
  header.vectors_offset = align_up(pos, 64);
  pos = header.vectors_offset + static_cast<uint64_t>(sorted_vectors.size() * sizeof(int16_t));
  header.labels_offset = align_up(pos, 64);
  pos = header.labels_offset + static_cast<uint64_t>(sorted_labels.size() * sizeof(uint8_t));
  header.orig_ids_offset = align_up(pos, 64);
  pos = header.orig_ids_offset + static_cast<uint64_t>(sorted_orig_ids.size() * sizeof(uint32_t));
  header.file_size = pos;
  std::memcpy(header.references_sha256, rinha::kReferencesSha256, sizeof(rinha::kReferencesSha256));

  auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("failed to create " + path);

  pos = 0;
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  pos += sizeof(header);
  pad_to(out, pos, header.centroid_offset);
  write_span(out, pos, centroids);
  pad_to(out, pos, header.bbox_min_offset);
  write_span(out, pos, bbox_min);
  pad_to(out, pos, header.bbox_max_offset);
  write_span(out, pos, bbox_max);
  pad_to(out, pos, header.list_offsets_offset);
  write_span(out, pos, offsets);
  pad_to(out, pos, header.vectors_offset);
  write_span(out, pos, sorted_vectors);
  pad_to(out, pos, header.labels_offset);
  write_span(out, pos, sorted_labels);
  pad_to(out, pos, header.orig_ids_offset);
  write_span(out, pos, sorted_orig_ids);
  if (!out || pos != header.file_size) throw std::runtime_error("failed while writing index");
}

}  // namespace

int main(int argc, char** argv) {
  std::setlocale(LC_ALL, "C");
  try {
    Args args = parse_args(argc, argv);
    power_of_two_depth(args.nlist);
    std::cerr << "reading " << args.references << "\n";
    auto json = read_gzip(args.references);
    std::cerr << "decompressed_bytes=" << (json.size() - 1) << "\n";

    std::vector<std::array<float, rinha::kLogicalDim>> vectors;
    std::vector<uint8_t> labels;
    parse_references(json, vectors, labels);
    if (vectors.empty()) throw std::runtime_error("no references parsed");

    std::vector<uint32_t> assignment;
    std::vector<float> centroids;
    if (args.method == "kmeans") {
      centroids = train_kmeans(vectors, args.nlist, args.kmeans_sample, args.kmeans_iters);
      std::cerr << "assigning nearest kmeans centroids nlist=" << args.nlist << "\n";
      assignment = assign_to_centroids(vectors, centroids, args.nlist);
    } else if (args.method == "balanced") {
      std::cerr << "assigning balanced lists nlist=" << args.nlist << "\n";
      assignment = assign_lists(vectors, args.nlist);
    } else {
      throw std::runtime_error("--method must be balanced or kmeans");
    }

    std::vector<uint32_t> counts;
    std::vector<double> sums;
    accumulate(vectors, assignment, args.nlist, counts, sums);
    print_histogram(counts);

    centroids = build_centroids(args.nlist, counts, sums);
    auto offsets = prefix_offsets(counts);
    write_index(args.out, args.nlist, centroids, offsets, assignment, vectors, labels);
    std::cerr << "wrote " << args.out << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "build-index failed: " << e.what() << "\n";
    return 1;
  }
}
