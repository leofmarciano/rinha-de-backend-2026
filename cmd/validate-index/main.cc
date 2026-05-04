#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "ann/index.h"
#include "vectorize/fraud_vector.h"

namespace {

/** Command-line configuration for validation runs. */
struct Args {
  /// Path to the generated index file.
  std::string index = "build/index_k8192.ivfi16";
  std::string out_json;

  /// JSON file containing request fixtures and expected decisions.
  std::string queries = "test/test-data.json";

  /// Maximum number of fixtures to validate; zero means all.
  size_t limit = 0;

  /// Base IVF probe count.
  uint32_t base_nprobe = 256;

  /// Expanded IVF probe count for ambiguous cases.
  uint32_t ambig_nprobe = 512;

  /// Whether to bypass IVF and run exact flat search.
  bool flat_only = false;

  /// Whether to bypass ANN and use only the heuristic classifier.
  bool heuristic_only = false;

  /// Whether ambiguous expanded ANN results may trigger exact flat fallback.
  bool exact_fallback = false;
  bool fast_path = false;
  bool dump_mismatches = false;
  rinha::BBoxMode bbox_mode = rinha::BBoxMode::kAmbiguousOnly;
};

/** Parses validation CLI flags. */
Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    std::string_view a(argv[i]);
    if (a == "--index" && i + 1 < argc) args.index = argv[++i];
    if (a == "--queries" && i + 1 < argc) args.queries = argv[++i];
    if (a == "--limit" && i + 1 < argc) args.limit = std::strtoull(argv[++i], nullptr, 10);
    if (a == "--base-nprobe" && i + 1 < argc)
      args.base_nprobe = std::strtoul(argv[++i], nullptr, 10);
    if (a == "--ambig-nprobe" && i + 1 < argc)
      args.ambig_nprobe = std::strtoul(argv[++i], nullptr, 10);
    if (a == "--flat-only") args.flat_only = true;
    if (a == "--heuristic-only") args.heuristic_only = true;
    if (a == "--no-exact-fallback") args.exact_fallback = false;
    if (a == "--exact-fallback") args.exact_fallback = true;
    if (a == "--fast-path") args.fast_path = true;
    if (a == "--dump-mismatches") args.dump_mismatches = true;
    if (a == "--out-json" && i + 1 < argc) args.out_json = argv[++i];
    if (a == "--bbox-mode" && i + 1 < argc) {
      const char* mode = argv[++i];
      if (std::strcmp(mode, "off") == 0)
        args.bbox_mode = rinha::BBoxMode::kOff;
      else if (std::strcmp(mode, "always") == 0)
        args.bbox_mode = rinha::BBoxMode::kAlways;
      else
        args.bbox_mode = rinha::BBoxMode::kAmbiguousOnly;
    }
  }
  return args;
}

/** Reads an entire file into memory. */
std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("failed to open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/** Advances over JSON whitespace. */
size_t skip_ws(std::string_view s, size_t pos) {
  while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t'))
    ++pos;
  return pos;
}

/** Skips over a JSON string span, including escaped bytes. */
size_t skip_string(std::string_view s, size_t pos) {
  ++pos;
  while (pos < s.size()) {
    if (s[pos] == '\\') {
      pos += 2;
      continue;
    }
    if (s[pos] == '"') return pos + 1;
    ++pos;
  }
  return std::string_view::npos;
}

/** Captures a balanced JSON object span starting near `pos`. */
bool object_span(std::string_view s, size_t pos, std::string_view& out) {
  pos = skip_ws(s, pos);
  if (pos >= s.size() || s[pos] != '{') return false;
  int depth = 0;
  for (size_t i = pos; i < s.size();) {
    if (s[i] == '"') {
      i = skip_string(s, i);
      if (i == std::string_view::npos) return false;
      continue;
    }
    if (s[i] == '{') ++depth;
    if (s[i] == '}' && --depth == 0) {
      out = s.substr(pos, i - pos + 1);
      return true;
    }
    ++i;
  }
  return false;
}

/** Reads the `expected_approved` boolean following a request fixture. */
bool expected_approved(std::string_view s, size_t pos, bool& out) {
  size_t e = s.find("\"expected_approved\"", pos);
  if (e == std::string_view::npos) return false;
  e = s.find(':', e);
  if (e == std::string_view::npos) return false;
  e = skip_ws(s, e + 1);
  if (s.substr(e, 4) == "true") {
    out = true;
    return true;
  }
  if (s.substr(e, 5) == "false") {
    out = false;
    return true;
  }
  return false;
}

bool expected_score(std::string_view s, size_t pos, int& out_frauds) {
  size_t e = s.find("\"expected_fraud_score\"", pos);
  if (e == std::string_view::npos) return false;
  e = s.find(':', e);
  if (e == std::string_view::npos) return false;
  e = skip_ws(s, e + 1);
  const char* begin = s.data() + e;
  char* end = nullptr;
  double score = std::strtod(begin, &end);
  if (end == begin) return false;
  out_frauds = static_cast<int>(score * 5.0 + 0.5);
  if (out_frauds < 0) out_frauds = 0;
  if (out_frauds > 5) out_frauds = 5;
  return true;
}

const char* bbox_mode_name(rinha::BBoxMode mode) {
  switch (mode) {
    case rinha::BBoxMode::kOff:
      return "off";
    case rinha::BBoxMode::kAlways:
      return "always";
    case rinha::BBoxMode::kAmbiguousOnly:
      return "ambiguous-only";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Args args = parse_args(argc, argv);
    rinha::MappedIndex index;
    std::string error;
    if (!rinha::load_index(args.index, index, &error)) {
      std::cerr << "failed to load index: " << error << "\n";
      return 1;
    }
    rinha::warmup_index(index);

    std::string data = read_file(args.queries);
    std::string_view json(data);
    rinha::SearchParams params;
    params.base_nprobe = args.base_nprobe;
    params.ambig_nprobe = args.ambig_nprobe;
    params.exact_fallback = args.exact_fallback;
    params.fast_path = args.fast_path;
    params.bbox_mode = args.bbox_mode;

    size_t pos = 0;
    size_t total = 0;
    size_t mismatches = 0;
    size_t false_positive = 0;
    size_t false_negative = 0;
    size_t parse_errors = 0;
    size_t expanded = 0;
    size_t flat = 0;
    size_t bbox = 0;
    uint64_t candidates = 0;
    uint64_t repaired_clusters = 0;
    size_t fraud_count_diff = 0;
    while ((pos = json.find("\"request\"", pos)) != std::string_view::npos) {
      size_t colon = json.find(':', pos);
      if (colon == std::string_view::npos) break;
      std::string_view request;
      if (!object_span(json, colon + 1, request)) break;
      bool expected = true;
      if (!expected_approved(json, pos + request.size(), expected)) break;
      int expected_frauds = expected ? 0 : 5;
      expected_score(json, pos + request.size(), expected_frauds);

      rinha::FraudRequest req;
      std::array<float, rinha::kPaddedDim> query{};
      bool predicted = true;
      int predicted_frauds = 0;
      uint32_t predicted_candidates = 0;
      if (!rinha::parse_fraud_request(request, req) || !rinha::vectorize_request(req, query)) {
        ++parse_errors;
      } else {
        if (args.heuristic_only) {
          predicted_frauds = rinha::heuristic_fraud_count(query);
          predicted = predicted_frauds < 3;
        } else if (args.fast_path) {
          const int fast_frauds = rinha::fast_path_fraud_count(query);
          if (fast_frauds >= 0) {
            predicted_frauds = fast_frauds;
            predicted = predicted_frauds < 3;
          } else {
            rinha::SearchResult result = args.flat_only ? rinha::flat_search(index, query)
                                                        : rinha::search_index(index, query, params);
            predicted_frauds = result.fraud_count;
            predicted = result.approved;
            predicted_candidates = result.scanned_candidates;
            if (result.used_nprobe >= params.ambig_nprobe) ++expanded;
            if (result.used_flat) ++flat;
            if (result.used_bbox) ++bbox;
            candidates += result.scanned_candidates;
            repaired_clusters += result.repaired_clusters;
          }
        } else {
          rinha::SearchResult result = args.flat_only ? rinha::flat_search(index, query)
                                                      : rinha::search_index(index, query, params);
          predicted_frauds = result.fraud_count;
          predicted = result.approved;
          predicted_candidates = result.scanned_candidates;
          if (result.used_nprobe >= params.ambig_nprobe) ++expanded;
          if (result.used_flat) ++flat;
          if (result.used_bbox) ++bbox;
          candidates += result.scanned_candidates;
          repaired_clusters += result.repaired_clusters;
        }
      }
      if (predicted_frauds != expected_frauds) ++fraud_count_diff;
      if (predicted != expected) {
        ++mismatches;
        if (expected && !predicted) ++false_positive;
        if (!expected && predicted) ++false_negative;
        if (args.dump_mismatches) {
          rinha::SearchParams repaired_params = params;
          repaired_params.bbox_mode = rinha::BBoxMode::kAlways;
          rinha::SearchResult repaired = rinha::search_index(index, query, repaired_params);
          std::cerr << "mismatch i=" << total << " expected=" << expected
                    << " predicted=" << predicted << " predicted_frauds=" << predicted_frauds
                    << " repaired=" << repaired.approved
                    << " repaired_frauds=" << static_cast<int>(repaired.fraud_count)
                    << " candidates=" << predicted_candidates << " repaired_candidates="
                    << repaired.scanned_candidates << " q=[";
          for (size_t d = 0; d < query.size(); ++d) {
            if (d) std::cerr << ',';
            std::cerr << query[d];
          }
          std::cerr << "]\n";
        }
      }
      ++total;
      pos = colon + request.size();
      if (args.limit && total >= args.limit) break;
    }

    const size_t weighted_errors = false_positive + false_negative * 3 + parse_errors * 5;
    const double mismatch_rate =
        total ? (100.0 * static_cast<double>(mismatches) / static_cast<double>(total)) : 0.0;
    const double avg_candidates =
        total ? static_cast<double>(candidates) / static_cast<double>(total) : 0.0;
    std::cout << "validated=" << total << " mismatches=" << mismatches << " fp=" << false_positive
              << " fn=" << false_negative << " weighted_errors=" << weighted_errors
              << " parse_errors=" << parse_errors << " fraud_count_diff=" << fraud_count_diff
              << " expanded=" << expanded << " flat=" << flat << " bbox=" << bbox
              << " repaired_clusters=" << repaired_clusters << " avg_candidates=" << avg_candidates
              << " mismatch_rate=" << mismatch_rate << "%\n";
    if (!args.out_json.empty()) {
      auto parent = std::filesystem::path(args.out_json).parent_path();
      if (!parent.empty()) std::filesystem::create_directories(parent);
      std::ofstream out(args.out_json, std::ios::binary | std::ios::trunc);
      out << "{\n"
          << "  \"index\": \"" << args.index << "\",\n"
          << "  \"nlist\": " << index.header->nlist << ",\n"
          << "  \"base_nprobe\": " << params.base_nprobe << ",\n"
          << "  \"ambig_nprobe\": " << params.ambig_nprobe << ",\n"
          << "  \"bbox_mode\": \"" << bbox_mode_name(params.bbox_mode) << "\",\n"
          << "  \"exact_fallback\": " << (params.exact_fallback ? "true" : "false") << ",\n"
          << "  \"validated\": " << total << ",\n"
          << "  \"mismatches\": " << mismatches << ",\n"
          << "  \"false_positive\": " << false_positive << ",\n"
          << "  \"false_negative\": " << false_negative << ",\n"
          << "  \"weighted_errors\": " << weighted_errors << ",\n"
          << "  \"parse_errors\": " << parse_errors << ",\n"
          << "  \"fraud_count_diff\": " << fraud_count_diff << ",\n"
          << "  \"expanded\": " << expanded << ",\n"
          << "  \"flat\": " << flat << ",\n"
          << "  \"bbox\": " << bbox << ",\n"
          << "  \"repaired_clusters\": " << repaired_clusters << ",\n"
          << "  \"avg_candidates\": " << avg_candidates << ",\n"
          << "  \"mismatch_rate\": " << mismatch_rate << "\n"
          << "}\n";
    }
    rinha::close_index(index);
    return parse_errors == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "validate-index failed: " << e.what() << "\n";
    return 1;
  }
}
