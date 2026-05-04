#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include "ann/index.h"
#include "http/server.h"

namespace {

/** Reads an unsigned integer environment variable with a fallback value. */
uint32_t env_u32(const char* name, uint32_t fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
}

/** Reads a permissive boolean environment variable with a fallback value. */
bool env_bool(const char* name, bool fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' ||
         value[0] == 'Y';
}

float env_float(const char* name, float fallback) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  return std::strtof(value, nullptr);
}

rinha::BBoxMode env_bbox_mode() {
  const char* explicit_mode = std::getenv("BBOX_MODE");
  if (explicit_mode && *explicit_mode) {
    if (std::strcmp(explicit_mode, "off") == 0) return rinha::BBoxMode::kOff;
    if (std::strcmp(explicit_mode, "always") == 0) return rinha::BBoxMode::kAlways;
    return rinha::BBoxMode::kAmbiguousOnly;
  }
  return env_bool("USE_BBOX_REPAIR", true) ? rinha::BBoxMode::kAmbiguousOnly
                                           : rinha::BBoxMode::kOff;
}

}  // namespace

int main() {
  try {
    const char* index_path_env = std::getenv("INDEX_PATH");
    std::string index_path =
        index_path_env && *index_path_env ? index_path_env : "build/index_k8192.ivfi16";

    rinha::MappedIndex index;
    std::string error;
    if (!rinha::load_index(index_path, index, &error)) {
      std::cerr << "failed to load index: " << error << "\n";
      return 1;
    }

    rinha::SearchParams params;
    params.base_nprobe = env_u32("BASE_NPROBE", 12);
    params.ambig_nprobe = env_u32("AMBIG_NPROBE", 24);
    params.heuristic_only = env_bool("HEURISTIC_ONLY", false);
    params.exact_fallback = env_bool("USE_EXACT_FALLBACK", false);
    params.fast_path = env_bool("USE_FAST_PATH", false);
    params.bbox_mode = env_bbox_mode();
    params.margin_threshold = env_float("MARGIN_THRESHOLD", 0.0f);
    params.full_warmup = true;

    if (params.full_warmup) rinha::warmup_index(index);

    const char* socket_path = std::getenv("SOCKET_PATH");
    uint16_t port = static_cast<uint16_t>(env_u32("PORT", 8080));
    uint32_t workers = env_u32("WORKERS", 1);
    std::cerr << "fraud-server ready on "
              << (socket_path && *socket_path ? socket_path : (":" + std::to_string(port)))
              << " vectors=" << index.header->total_vectors << " nlist=" << index.header->nlist
              << " base_nprobe=" << params.base_nprobe
              << " ambig_nprobe=" << params.ambig_nprobe
              << " heuristic_only=" << (params.heuristic_only ? "true" : "false")
              << " fast_path=" << (params.fast_path ? "true" : "false")
              << " exact_fallback=" << (params.exact_fallback ? "true" : "false") << "\n";
    if (socket_path && *socket_path) {
      return rinha::run_unix_http_server(index, params, socket_path, workers);
    }
    return rinha::run_http_server(index, params, port, workers);
  } catch (const std::exception& e) {
    std::cerr << "fraud-server failed: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "fraud-server failed with unknown error\n";
    return 1;
  }
}
