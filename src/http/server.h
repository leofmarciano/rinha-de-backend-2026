#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ann/index.h"

namespace rinha {

/**
 * Handles one complete HTTP request already loaded in memory.
 *
 * This is the testable request/response contract used by the socket servers internally. It returns
 * an empty string when the request is incomplete.
 */
std::string handle_http_request(const MappedIndex& index, const SearchParams& params,
                                std::string_view request_bytes);

/**
 * Runs the HTTP fraud scoring server on a TCP port.
 *
 * @param index Mapped ANN index shared by all workers.
 * @param params Search tuning parameters.
 * @param port TCP port to bind.
 * @param workers Number of accept loops to start; zero is treated as one by the implementation.
 * @return Process-style exit code.
 */
int run_http_server(const MappedIndex& index, const SearchParams& params, uint16_t port,
                    uint32_t workers);

/**
 * Runs the HTTP fraud scoring server on a Unix domain socket.
 *
 * The protocol is still HTTP/1.1; only the transport changes.
 */
int run_unix_http_server(const MappedIndex& index, const SearchParams& params,
                         std::string_view socket_path, uint32_t workers);

}  // namespace rinha
