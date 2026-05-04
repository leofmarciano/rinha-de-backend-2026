#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ann/index.h"
#include "ann/topk.h"
#include "http/server.h"
#include "util/time_parse.h"
#include "vectorize/fraud_vector.h"

namespace {

struct TestCase {
  std::string_view name;
  std::function<void()> fn;
};

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void require_near(float actual, float expected, float tolerance, std::string_view message) {
  if (std::fabs(actual - expected) > tolerance) {
    throw std::runtime_error(std::string(message) + ": got " + std::to_string(actual) +
                             ", expected " + std::to_string(expected));
  }
}

std::string fixture_request(bool known_merchant, bool last_transaction) {
  std::string known = known_merchant ? R"("m-001","m-002")" : R"("m-002","m-003")";
  std::string last =
      last_transaction ? R"({"timestamp":"2026-01-04T12:00:00Z","km_from_current":250.0})" : "null";
  return R"({
    "merchant": {"avg_amount": 400.0, "mcc": "7801", "id": "m-001"},
    "terminal": {"km_from_home": 500.0, "card_present": false, "is_online": true},
    "last_transaction": )" +
         last +
         R"(,
    "customer": {"known_merchants": [)" +
         known +
         R"(], "tx_count_24h": 10, "avg_amount": 100.0},
    "transaction": {"requested_at": "2026-01-05T12:00:00Z", "installments": 6, "amount": 250.0}
  })";
}

std::string fixture_request_with_mcc(std::string_view mcc) {
  std::string json = fixture_request(true, true);
  const size_t pos = json.find("\"7801\"");
  require(pos != std::string::npos, "fixture MCC token should exist");
  json.replace(pos + 1, 4, mcc);
  return json;
}

void write_minimal_index(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());

  constexpr uint32_t nlist = 2;
  constexpr uint32_t total = 10;
  std::array<float, rinha::kPaddedDim> safe{};
  safe.fill(0.05f);
  std::array<float, rinha::kPaddedDim> fraud{};
  fraud.fill(0.95f);

  std::vector<float> centroids(static_cast<size_t>(nlist) * rinha::kLogicalDim, 100.0f);
  std::vector<int16_t> bbox_min(static_cast<size_t>(nlist) * rinha::kLogicalDim);
  std::vector<int16_t> bbox_max(static_cast<size_t>(nlist) * rinha::kLogicalDim);
  for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
    centroids[d] = safe[d];
    centroids[static_cast<size_t>(rinha::kLogicalDim) + d] = fraud[d];
    bbox_min[d] = bbox_max[d] = rinha::quantize_i16(safe[d]);
    bbox_min[static_cast<size_t>(rinha::kLogicalDim) + d] = rinha::quantize_i16(fraud[d]);
    bbox_max[static_cast<size_t>(rinha::kLogicalDim) + d] = rinha::quantize_i16(fraud[d]);
  }

  std::vector<uint32_t> offsets(nlist + 1);
  offsets[0] = 0;
  offsets[1] = 5;
  offsets[2] = total;

  std::vector<int16_t> vectors(static_cast<size_t>(total) * rinha::kLogicalDim);
  std::vector<uint8_t> labels(total);
  std::vector<uint32_t> orig_ids(total);
  for (uint32_t row = 0; row < total; ++row) {
    const auto& source = row < 5 ? safe : fraud;
    labels[row] = row < 5 ? 0 : 1;
    orig_ids[row] = row;
    for (uint32_t d = 0; d < rinha::kLogicalDim; ++d) {
      vectors[static_cast<size_t>(d) * total + row] = rinha::quantize_i16(source[d]);
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
  header.centroid_offset = sizeof(rinha::IndexHeader);
  header.bbox_min_offset = header.centroid_offset + centroids.size() * sizeof(float);
  header.bbox_max_offset = header.bbox_min_offset + bbox_min.size() * sizeof(int16_t);
  header.list_offsets_offset = header.bbox_max_offset + bbox_max.size() * sizeof(int16_t);
  header.vectors_offset = header.list_offsets_offset + offsets.size() * sizeof(uint32_t);
  header.labels_offset = header.vectors_offset + vectors.size() * sizeof(int16_t);
  header.orig_ids_offset = header.labels_offset + labels.size() * sizeof(uint8_t);
  header.file_size = header.orig_ids_offset + orig_ids.size() * sizeof(uint32_t);
  std::memcpy(header.references_sha256, rinha::kReferencesSha256, sizeof(rinha::kReferencesSha256));

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  require(static_cast<bool>(out), "failed to create test index");
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(centroids.data()),
            static_cast<std::streamsize>(centroids.size() * sizeof(float)));
  out.write(reinterpret_cast<const char*>(bbox_min.data()),
            static_cast<std::streamsize>(bbox_min.size() * sizeof(int16_t)));
  out.write(reinterpret_cast<const char*>(bbox_max.data()),
            static_cast<std::streamsize>(bbox_max.size() * sizeof(int16_t)));
  out.write(reinterpret_cast<const char*>(offsets.data()),
            static_cast<std::streamsize>(offsets.size() * sizeof(uint32_t)));
  out.write(reinterpret_cast<const char*>(vectors.data()),
            static_cast<std::streamsize>(vectors.size() * sizeof(int16_t)));
  out.write(reinterpret_cast<const char*>(labels.data()),
            static_cast<std::streamsize>(labels.size() * sizeof(uint8_t)));
  out.write(reinterpret_cast<const char*>(orig_ids.data()),
            static_cast<std::streamsize>(orig_ids.size() * sizeof(uint32_t)));
  require(static_cast<bool>(out), "failed to write test index");
}

void write_invalid_header_index(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  rinha::IndexHeader header{};
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
}

void write_invalid_offsets_index(const std::filesystem::path& path) {
  write_minimal_index(path);
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  require(static_cast<bool>(file), "invalid offset fixture should open");
  uint64_t bad_file_size = sizeof(rinha::IndexHeader);
  file.seekp(static_cast<std::streamoff>(offsetof(rinha::IndexHeader, file_size)));
  file.write(reinterpret_cast<const char*>(&bad_file_size), sizeof(bad_file_size));
}

void test_time_parse() {
  int64_t epoch = -1;
  int hour = -1;
  int weekday = -1;
  require(rinha::parse_iso_utc("1970-01-01T00:00:00Z", epoch, hour, weekday),
          "epoch timestamp should parse");
  require(epoch == 0, "epoch seconds should be zero");
  require(hour == 0, "hour should be zero");
  require(weekday == 3, "1970-01-01 should be Thursday with Monday as zero");

  require(rinha::parse_iso_utc("2026-01-05T12:34:56Z", epoch, hour, weekday),
          "modern timestamp should parse");
  require(hour == 12, "hour should be parsed by fixed position");
  require(weekday == 0, "2026-01-05 should be Monday");

  require(!rinha::parse_iso_utc("2026/01/05 12:34:56", epoch, hour, weekday),
          "malformed timestamp should fail");
  require(!rinha::parse_iso_utc("20x6-01-05T12:34:56Z", epoch, hour, weekday),
          "invalid four-digit field should fail");
  require(!rinha::parse_iso_utc("2026-0x-05T12:34:56Z", epoch, hour, weekday),
          "invalid two-digit field should fail");
  require(!rinha::parse_iso_utc("short", epoch, hour, weekday), "short timestamp should fail");
}

void test_half_conversion() {
  require(rinha::half_to_float(rinha::float_to_half(0.0f)) == 0.0f, "zero should round-trip");
  require_near(rinha::half_to_float(rinha::float_to_half(1.0f)), 1.0f, 0.001f,
               "one should round-trip");
  require_near(rinha::half_to_float(rinha::float_to_half(0.333f)), 0.333f, 0.001f,
               "fraction should round-trip approximately");
  require_near(rinha::half_to_float(rinha::float_to_half(0.00001f)), 0.00001f, 0.00001f,
               "subnormal half should round-trip approximately");
  require(rinha::half_to_float(1) > 0.0f, "explicit subnormal half should convert");
  require(std::isinf(rinha::half_to_float(rinha::float_to_half(1.0e20f))),
          "large float should convert to infinity");
}

void test_topk_and_helpers() {
  require(rinha::clamp01(-1.0f) == 0.0f, "clamp lower bound");
  require(rinha::clamp01(2.0f) == 1.0f, "clamp upper bound");
  require(rinha::bin_clamped(0.99f, 4) == 3, "bin should clamp to last bucket");

  rinha::FixedTopK<3> top;
  top.insert(3.0f, 30);
  top.insert(1.0f, 10);
  top.insert(2.0f, 20);
  top.insert(4.0f, 40);
  top.insert(0.5f, 5);
  require(top.size == 3, "top-k should keep fixed capacity");
  require(top.id[0] == 5 && top.id[1] == 10 && top.id[2] == 20, "top-k should stay sorted");

  std::array<float, rinha::kPaddedDim> v{};
  v.fill(0.0f);
  require(rinha::fast_path_fraud_count(v) == 0, "low score should be safe fast-path");
  v.fill(1.0f);
  require(rinha::fast_path_fraud_count(v) == 5, "high score should be fraud fast-path");
  v.fill(0.02f);
  require(rinha::heuristic_fraud_count(v) == 0, "low heuristic feature should be safe");
  v[2] = 0.08f;
  require(rinha::heuristic_fraud_count(v) == 5, "high heuristic feature should be fraud");
}

void test_parser_and_vectorizer() {
  std::string json = fixture_request(true, true);
  rinha::FraudRequest req;
  require(rinha::parse_fraud_request(json, req), "valid request should parse");
  require(req.amount == 250.0, "amount should parse");
  require(req.installments == 6, "installments should parse");
  require(req.known_merchant_count == 2, "known merchant count should parse");
  require(req.has_last_transaction, "last transaction should parse");

  std::array<float, rinha::kPaddedDim> out{};
  require(rinha::vectorize_request(req, out), "valid request should vectorize");
  require_near(out[0], 0.025f, 0.0001f, "amount feature");
  require_near(out[1], 0.5f, 0.0001f, "installments feature");
  require_near(out[2], 0.25f, 0.0001f, "amount/customer average ratio feature");
  require_near(out[3], 12.0f / 23.0f, 0.0001f, "hour feature");
  require_near(out[4], 0.0f, 0.0001f, "weekday feature");
  require_near(out[5], 1.0f, 0.0001f, "last transaction delta feature");
  require_near(out[6], 0.25f, 0.0001f, "last transaction distance feature");
  require_near(out[7], 0.5f, 0.0001f, "home distance feature");
  require_near(out[8], 0.5f, 0.0001f, "transaction count feature");
  require(out[9] == 1.0f, "online feature");
  require(out[10] == 0.0f, "card present feature");
  require(out[11] == 0.0f, "known merchant feature");
  require_near(out[12], 0.8f, 0.0001f, "MCC risk feature");
  require_near(out[13], 0.04f, 0.0001f, "merchant average amount feature");

  std::string unknown_json = fixture_request(false, false);
  require(rinha::parse_fraud_request(unknown_json, req), "null last transaction request parses");
  require(!req.has_last_transaction, "null last transaction should be represented");
  require(rinha::vectorize_request(req, out), "null last transaction request vectorizes");
  require(out[5] == -1.0f && out[6] == -1.0f, "missing last transaction uses sentinels");
  require(out[11] == 1.0f, "unknown merchant feature");

  require(!rinha::parse_fraud_request(R"({"transaction":{}})", req), "missing objects should fail");
  require(!rinha::parse_fraud_request(R"({"transaction":{"amount":1,"installments":1,)", req),
          "malformed string/object should fail");
  req.requested_at = "bad";
  require(!rinha::vectorize_request(req, out), "invalid requested_at should fail");

  const std::vector<std::pair<std::string_view, float>> mcc_cases = {
      {"5411", 0.15f}, {"5812", 0.30f}, {"5912", 0.20f}, {"5944", 0.45f}, {"7802", 0.75f},
      {"7995", 0.85f}, {"4511", 0.35f}, {"5311", 0.25f}, {"5999", 0.50f}, {"0000", 0.50f},
  };
  for (const auto& [mcc, expected] : mcc_cases) {
    std::string mcc_json = fixture_request_with_mcc(mcc);
    require(rinha::parse_fraud_request(mcc_json, req), "MCC fixture should parse");
    require(rinha::vectorize_request(req, out), "MCC fixture should vectorize");
    require_near(out[12], expected, 0.0001f, "MCC risk should match lookup");
  }
}

void test_index_integration() {
  const auto path = std::filesystem::temp_directory_path() / "rinha-native-test" / "mini.ivfi16";
  write_minimal_index(path);

  rinha::MappedIndex index;
  std::string error;
  require(rinha::load_index(path.string(), index, &error), "minimal index should load");
  require(index.header->total_vectors == 10, "loaded index should expose vector count");
  require(rinha::warmup_index(index), "loaded index should warm up");

  std::array<float, rinha::kPaddedDim> safe{};
  safe.fill(0.05f);
  rinha::SearchResult safe_result = rinha::search_index(index, safe, rinha::SearchParams{});
  require(safe_result.approved, "safe vector should be approved");
  require(safe_result.fraud_count == 0, "safe vector should find safe neighbors");

  std::array<float, rinha::kPaddedDim> fraud{};
  fraud.fill(0.95f);
  rinha::SearchResult fraud_result = rinha::flat_search(index, fraud);
  require(!fraud_result.approved, "fraud vector should be denied");
  require(fraud_result.fraud_count == 5, "fraud vector should find fraud neighbors");
  require(fraud_result.used_flat, "flat search should mark flat usage");

  rinha::close_index(index);
  require(index.mapping == nullptr && index.fd == -1, "close_index should reset mapping state");

  require(!rinha::load_index(path.parent_path().string(), index, &error),
          "directory should not load");
  require(!rinha::load_index((path.parent_path() / "missing.ivfi16").string(), index, &error),
          "missing index should not load");

  const auto invalid_header_path = path.parent_path() / "invalid-header.ivfi16";
  write_invalid_header_index(invalid_header_path);
  require(!rinha::load_index(invalid_header_path.string(), index, &error),
          "invalid header should not load");

  const auto invalid_offsets_path = path.parent_path() / "invalid-offsets.ivfi16";
  write_invalid_offsets_index(invalid_offsets_path);
  require(!rinha::load_index(invalid_offsets_path.string(), index, &error),
          "invalid offsets should not load");
}

void test_http_request_handler() {
  rinha::MappedIndex index;
  rinha::SearchParams params;
  params.heuristic_only = true;

  std::string ready =
      rinha::handle_http_request(index, params, "GET /ready HTTP/1.1\r\nHost: test\r\n\r\n");
  require(ready.find("HTTP/1.1 200 OK\r\n") == 0, "ready should return 200");
  require(ready.find("Content-Type: text/plain\r\n") != std::string::npos,
          "ready should be text/plain");
  require(ready.ends_with("ready\n"), "ready body should match");

  std::string missing =
      rinha::handle_http_request(index, params, "GET /missing HTTP/1.1\r\nHost: test\r\n\r\n");
  require(missing.find("HTTP/1.1 404 Not Found\r\n") == 0, "missing route should return 404");
  require(missing.ends_with("not found\n"), "missing route body should match");

  std::string invalid_body = "{}";
  std::string invalid_request = "POST /fraud-score HTTP/1.1\r\nHost: test\r\nContent-Length: " +
                                std::to_string(invalid_body.size()) + "\r\n\r\n" + invalid_body;
  std::string invalid = rinha::handle_http_request(index, params, invalid_request);
  require(invalid.find("HTTP/1.1 200 OK\r\n") == 0,
          "invalid fraud payload should still return 200");
  require(invalid.find("Content-Type: application/json\r\n") != std::string::npos,
          "fraud response should be JSON");
  require(invalid.ends_with(R"({"approved":true,"fraud_score":0.0})"),
          "invalid fraud payload should use fallback body");

  std::string body = fixture_request(true, true);
  std::string request =
      "POST /fraud-score HTTP/1.1\r\nHost: test\r\ncontent-length: " + std::to_string(body.size()) +
      "\r\nConnection: close\r\n\r\n" + body;
  std::string scored = rinha::handle_http_request(index, params, request);
  require(scored.find("HTTP/1.1 200 OK\r\n") == 0, "valid fraud payload should return 200");
  require(scored.ends_with(R"({"approved":false,"fraud_score":1.0})"),
          "heuristic fraud payload should deny");

  std::string incomplete =
      rinha::handle_http_request(index, params, "POST /fraud-score HTTP/1.1\r\n");
  require(incomplete.empty(), "incomplete request should not produce a response");
}

int run_tests(const std::vector<TestCase>& tests) {
  int failed = 0;
  for (const auto& test : tests) {
    try {
      test.fn();
      std::cerr << "[PASS] " << test.name << "\n";
    } catch (const std::exception& e) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<TestCase> unit_tests = {
      {"time_parse", test_time_parse},
      {"half_conversion", test_half_conversion},
      {"topk_and_helpers", test_topk_and_helpers},
      {"parser_and_vectorizer", test_parser_and_vectorizer},
  };
  const std::vector<TestCase> integration_tests = {
      {"index_integration", test_index_integration},
      {"http_request_handler", test_http_request_handler},
  };

  if (argc == 2 && std::string_view(argv[1]) == "--unit") return run_tests(unit_tests);
  if (argc == 2 && std::string_view(argv[1]) == "--integration") {
    return run_tests(integration_tests);
  }

  int rc = run_tests(unit_tests);
  if (run_tests(integration_tests) != 0) rc = 1;
  return rc;
}
