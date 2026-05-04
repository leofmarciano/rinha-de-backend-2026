#include <cstdlib>
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
  std::string index = "build/fraud.ivf16";

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
  bool exact_fallback = true;
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

    size_t pos = 0;
    size_t total = 0;
    size_t mismatches = 0;
    size_t parse_errors = 0;
    size_t expanded = 0;
    size_t flat = 0;
    while ((pos = json.find("\"request\"", pos)) != std::string_view::npos) {
      size_t colon = json.find(':', pos);
      if (colon == std::string_view::npos) break;
      std::string_view request;
      if (!object_span(json, colon + 1, request)) break;
      bool expected = true;
      if (!expected_approved(json, pos + request.size(), expected)) break;

      rinha::FraudRequest req;
      std::array<float, rinha::kPaddedDim> query{};
      bool predicted = true;
      if (!rinha::parse_fraud_request(request, req) || !rinha::vectorize_request(req, query)) {
        ++parse_errors;
      } else {
        const int fast_frauds = rinha::fast_path_fraud_count(query);
        if (args.heuristic_only) {
          predicted = rinha::heuristic_fraud_count(query) < 3;
        } else if (fast_frauds >= 0) {
          predicted = fast_frauds < 3;
        } else {
          rinha::SearchResult result = args.flat_only ? rinha::flat_search(index, query)
                                                      : rinha::search_index(index, query, params);
          predicted = result.approved;
          if (result.used_nprobe >= params.ambig_nprobe) ++expanded;
          if (result.used_flat) ++flat;
        }
      }
      if (predicted != expected) ++mismatches;
      ++total;
      pos = colon + request.size();
      if (args.limit && total >= args.limit) break;
    }

    std::cout << "validated=" << total << " mismatches=" << mismatches
              << " parse_errors=" << parse_errors << " expanded=" << expanded << " flat=" << flat
              << " mismatch_rate="
              << (total ? (100.0 * static_cast<double>(mismatches) / static_cast<double>(total))
                        : 0.0)
              << "%\n";
    rinha::close_index(index);
    return parse_errors == 0 ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "validate-index failed: " << e.what() << "\n";
    return 1;
  }
}
