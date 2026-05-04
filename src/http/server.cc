#include "http/server.h"

#include <arpa/inet.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "vectorize/fraud_vector.h"

namespace rinha {
namespace {

constexpr std::string_view kFallbackBody = "{\"approved\":true,\"fraud_score\":0.0}";
constexpr std::string_view kReadyBody = "ready\n";
constexpr std::string_view kNotFoundBody = "not found\n";
constexpr std::string_view kResponses[6] = {
    "{\"approved\":true,\"fraud_score\":0.0}",  "{\"approved\":true,\"fraud_score\":0.2}",
    "{\"approved\":true,\"fraud_score\":0.4}",  "{\"approved\":false,\"fraud_score\":0.6}",
    "{\"approved\":false,\"fraud_score\":0.8}", "{\"approved\":false,\"fraud_score\":1.0}",
};

#ifdef __linux__
inline char kListenerSentinel = 0;
inline void* const kListenerToken = &kListenerSentinel;

/** Per-connection state used by the Linux epoll server. */
struct Conn {
  /// Client socket file descriptor.
  int fd = -1;

  /// Accumulated request bytes until a complete HTTP request is available.
  std::array<char, 32768> request{};

  /// Number of valid bytes in `request`.
  size_t request_len = 0;

  /// Pre-rendered HTTP response buffer.
  std::array<char, 512> response{};

  /// Number of valid bytes in `response`.
  size_t response_len = 0;

  /// Number of response bytes already sent.
  size_t written = 0;
};

/** Switches a file descriptor to non-blocking mode. */
bool set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

/** Writes an entire buffer to a blocking socket, retrying interrupted sends. */
bool write_all(int fd, const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

/** Detects whether request headers ask to close the connection. */
bool contains_close(std::string_view headers) {
  size_t pos = headers.find("Connection:");
  if (pos == std::string_view::npos) pos = headers.find("connection:");
  if (pos == std::string_view::npos) return false;
  const size_t line_end = headers.find('\n', pos);
  std::string_view line = headers.substr(
      pos, line_end == std::string_view::npos ? headers.size() - pos : line_end - pos);
  return line.find("close") != std::string_view::npos ||
         line.find("Close") != std::string_view::npos;
}

/** Extracts `Content-Length` from request headers, returning zero when absent. */
size_t content_length(std::string_view headers) {
  size_t pos = headers.find("Content-Length:");
  if (pos == std::string_view::npos) pos = headers.find("content-length:");
  if (pos == std::string_view::npos) return 0;
  pos += 15;
  while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
  size_t value = 0;
  while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
    value = value * 10 + static_cast<size_t>(headers[pos] - '0');
    ++pos;
  }
  return value;
}

/** Parses and scores a fraud request body, returning a precomputed JSON response body. */
std::string_view response_for(const MappedIndex& index, const SearchParams& params,
                              std::string_view body) {
  FraudRequest req;
  std::array<float, kPaddedDim> query{};
  if (!parse_fraud_request(body, req) || !vectorize_request(req, query)) return kFallbackBody;
  if (params.heuristic_only) return kResponses[heuristic_fraud_count(query)];
  if (params.fast_path) {
    const int fast_frauds = fast_path_fraud_count(query);
    if (fast_frauds >= 0) return kResponses[fast_frauds];
  }
  SearchResult result = search_index(index, query, params);
  if (result.fraud_count > 5) return kFallbackBody;
  return kResponses[result.fraud_count];
}

/** Sends a complete HTTP response on a blocking socket. */
bool send_response(int fd, int status, std::string_view content_type, std::string_view body,
                   bool close_conn) {
  char header[256];
  const char* status_text = status == 200 ? "OK" : "Not Found";
  int n = std::snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %.*s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        status, status_text, static_cast<int>(content_type.size()),
                        content_type.data(), body.size(), close_conn ? "close" : "keep-alive");
  return n > 0 && write_all(fd, header, static_cast<size_t>(n)) &&
         write_all(fd, body.data(), body.size());
}

#ifdef __linux__
/** Removes a connection from epoll, closes it and releases its state. */
void close_conn(int epoll_fd, Conn* conn) {
  if (!conn) return;
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
  close(conn->fd);
  delete conn;
}

/** Re-arms a connection for the requested epoll events. */
void arm_conn(int epoll_fd, Conn* conn, uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.ptr = conn;
  epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

/** Builds an HTTP response into the connection buffer. */
bool prepare_response(Conn* conn, int status, std::string_view content_type,
                      std::string_view body) {
  const char* status_text = status == 200 ? "OK" : "Not Found";
  int n =
      std::snprintf(conn->response.data(), conn->response.size(),
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %.*s\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%.*s",
                    status, status_text, static_cast<int>(content_type.size()), content_type.data(),
                    body.size(), static_cast<int>(body.size()), body.data());
  if (n <= 0 || static_cast<size_t>(n) > conn->response.size()) return false;
  conn->response_len = static_cast<size_t>(n);
  conn->written = 0;
  return true;
}

/**
 * Attempts to parse a full request currently buffered on a connection.
 *
 * @return `true` when a response is ready to write.
 */
bool try_prepare_request(Conn* conn, const MappedIndex& index, const SearchParams& params) {
  std::string_view buffered(conn->request.data(), conn->request_len);
  const size_t header_end = buffered.find("\r\n\r\n");
  if (header_end == std::string::npos) return false;

  std::string_view headers(conn->request.data(), header_end + 4);
  const size_t len = content_length(headers);
  if (conn->request_len < header_end + 4 + len) return false;

  std::string_view request(conn->request.data(), header_end);
  const size_t first_line_end = request.find("\r\n");
  std::string_view first_line =
      request.substr(0, first_line_end == std::string_view::npos ? request.size() : first_line_end);

  if (first_line.starts_with("GET /ready")) {
    prepare_response(conn, 200, "text/plain", kReadyBody);
  } else if (first_line.starts_with("POST /fraud-score")) {
    std::string_view body(conn->request.data() + header_end + 4, len);
    prepare_response(conn, 200, "application/json", response_for(index, params, body));
  } else {
    prepare_response(conn, 404, "text/plain", kNotFoundBody);
  }
  return true;
}

/** Flushes a prepared response, re-arming for EPOLLOUT when the socket would block. */
void write_ready(int epoll_fd, Conn* conn) {
  while (conn->written < conn->response_len) {
    ssize_t n = send(conn->fd, conn->response.data() + conn->written,
                     conn->response_len - conn->written, MSG_NOSIGNAL);
    if (n > 0) {
      conn->written += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EINTR)) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      arm_conn(epoll_fd, conn, EPOLLOUT);
      return;
    }
    close_conn(epoll_fd, conn);
    return;
  }
  close_conn(epoll_fd, conn);
}

/** Reads available request bytes and prepares/writes a response once complete. */
void read_ready(int epoll_fd, Conn* conn, const MappedIndex& index, const SearchParams& params) {
  while (true) {
    if (conn->request_len >= conn->request.size()) {
      close_conn(epoll_fd, conn);
      return;
    }
    ssize_t n = recv(conn->fd, conn->request.data() + conn->request_len,
                     conn->request.size() - conn->request_len, 0);
    if (n > 0) {
      conn->request_len += static_cast<size_t>(n);
      if (try_prepare_request(conn, index, params)) {
        write_ready(epoll_fd, conn);
        return;
      }
      continue;
    }
    if (n == 0) {
      close_conn(epoll_fd, conn);
      return;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    close_conn(epoll_fd, conn);
    return;
  }
}

/** Accepts all currently pending client sockets and registers them in epoll. */
void accept_ready(int epoll_fd, int server) {
  while (true) {
    int client = accept4(server, nullptr, nullptr, SOCK_NONBLOCK);
    if (client < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      return;
    }

    auto* conn = new Conn();
    conn->fd = client;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = conn;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev) != 0) {
      close(client);
      delete conn;
    }
  }
}

/** Runs the Linux epoll-based HTTP event loop across worker threads. */
int run_epoll_server(int server, const MappedIndex& index, const SearchParams& params,
                     uint32_t workers) {
  if (workers == 0) workers = 1;
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (uint32_t i = 0; i < workers; ++i) {
    threads.emplace_back([server, &index, &params] {
      int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
      if (epoll_fd < 0) return;

      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLEXCLUSIVE;
      ev.data.ptr = kListenerToken;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server, &ev) != 0) {
        close(epoll_fd);
        return;
      }

      std::array<epoll_event, 1024> events{};
      while (true) {
        int n = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);
        if (n < 0) {
          if (errno == EINTR) continue;
          break;
        }
        for (int i = 0; i < n; ++i) {
          void* tag = events[static_cast<size_t>(i)].data.ptr;
          if (tag == kListenerToken) {
            accept_ready(epoll_fd, server);
            continue;
          }
          auto* conn = static_cast<Conn*>(tag);
          const uint32_t flags = events[static_cast<size_t>(i)].events;
          if (flags & EPOLLIN) {
            read_ready(epoll_fd, conn, index, params);
          } else if (flags & EPOLLOUT) {
            write_ready(epoll_fd, conn);
          } else if (flags & (EPOLLERR | EPOLLHUP)) {
            close_conn(epoll_fd, conn);
          }
        }
      }
      close(epoll_fd);
    });
  }
  for (auto& thread : threads) thread.join();
  return 0;
}
#endif

/** Handles one client with the portable blocking socket implementation. */
void handle_client(int client, const MappedIndex& index, const SearchParams& params) {
  std::string buffer;
  buffer.reserve(32768);
  char chunk[8192];

  while (true) {
    size_t header_end = buffer.find("\r\n\r\n");
    while (header_end == std::string::npos) {
      ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        close(client);
        return;
      }
      buffer.append(chunk, static_cast<size_t>(n));
      if (buffer.size() > 1 << 20) {
        close(client);
        return;
      }
      header_end = buffer.find("\r\n\r\n");
    }

    std::string_view headers(buffer.data(), header_end + 4);
    const size_t len = content_length(headers);
    while (buffer.size() < header_end + 4 + len) {
      ssize_t n = recv(client, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        close(client);
        return;
      }
      buffer.append(chunk, static_cast<size_t>(n));
    }

    std::string_view request(buffer.data(), buffer.size());
    const size_t first_line_end = request.find("\r\n");
    std::string_view first_line = request.substr(0, first_line_end);
    bool close_conn = true;

    bool ok = true;
    if (first_line.starts_with("GET /ready")) {
      ok = send_response(client, 200, "text/plain", kReadyBody, close_conn);
    } else if (first_line.starts_with("POST /fraud-score")) {
      std::string_view body(buffer.data() + header_end + 4, len);
      ok = send_response(client, 200, "application/json", response_for(index, params, body),
                         close_conn);
    } else {
      ok = send_response(client, 404, "text/plain", kNotFoundBody, true);
      close_conn = true;
    }

    if (!ok || close_conn) {
      close(client);
      return;
    }
    buffer.erase(0, header_end + 4 + len);
  }
}

/** Accept loop used by the portable non-Linux server path. */
void accept_loop(int server, const MappedIndex& index, const SearchParams& params) {
  while (true) {
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    int client = accept(server, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (client < 0) {
      if (errno == EINTR) continue;
      continue;
    }
    handle_client(client, index, params);
  }
}

}  // namespace

std::string handle_http_request(const MappedIndex& index, const SearchParams& params,
                                std::string_view request_bytes) {
  const size_t header_end = request_bytes.find("\r\n\r\n");
  if (header_end == std::string_view::npos) return {};

  std::string_view headers = request_bytes.substr(0, header_end + 4);
  const size_t len = content_length(headers);
  if (request_bytes.size() < header_end + 4 + len) return {};

  const size_t first_line_end = request_bytes.find("\r\n");
  std::string_view first_line = request_bytes.substr(
      0, first_line_end == std::string_view::npos ? header_end : first_line_end);

  int status = 200;
  std::string_view content_type = "text/plain";
  std::string_view body = kReadyBody;
  bool close_conn = true;

  if (first_line.starts_with("GET /ready")) {
    body = kReadyBody;
  } else if (first_line.starts_with("POST /fraud-score")) {
    content_type = "application/json";
    body = response_for(index, params, request_bytes.substr(header_end + 4, len));
  } else {
    status = 404;
    body = kNotFoundBody;
  }

  close_conn = close_conn || contains_close(headers);
  const char* status_text = status == 200 ? "OK" : "Not Found";
  char header[256];
  int n = std::snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %.*s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: %s\r\n"
                        "\r\n",
                        status, status_text, static_cast<int>(content_type.size()),
                        content_type.data(), body.size(), close_conn ? "close" : "keep-alive");
  if (n <= 0) return {};

  std::string response(header, static_cast<size_t>(n));
  response.append(body);
  return response;
}

/** Starts serving on an already bound socket. */
int run_bound_server(int server, const MappedIndex& index, const SearchParams& params,
                     uint32_t workers) {
  if (listen(server, 1024) != 0) {
    std::cerr << "listen failed: " << std::strerror(errno) << "\n";
    close(server);
    return 1;
  }

#ifdef __linux__
  if (!set_nonblocking(server)) {
    std::cerr << "failed to make listener nonblocking: " << std::strerror(errno) << "\n";
    close(server);
    return 1;
  }
  int rc = run_epoll_server(server, index, params, workers);
  close(server);
  return rc;
#else
  if (workers == 0) workers = 1;
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (uint32_t i = 0; i < workers; ++i) {
    threads.emplace_back([server, &index, &params] { accept_loop(server, index, params); });
  }
  for (auto& thread : threads) thread.join();
  close(server);
  return 0;
#endif
}

int run_http_server(const MappedIndex& index, const SearchParams& params, uint16_t port,
                    uint32_t workers) {
  std::signal(SIGPIPE, SIG_IGN);

  int server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) {
    std::cerr << "socket failed: " << std::strerror(errno) << "\n";
    return 1;
  }
  int one = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
  setsockopt(server, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "bind failed: " << std::strerror(errno) << "\n";
    close(server);
    return 1;
  }
  return run_bound_server(server, index, params, workers);
}

int run_unix_http_server(const MappedIndex& index, const SearchParams& params,
                         std::string_view socket_path, uint32_t workers) {
  std::signal(SIGPIPE, SIG_IGN);
  std::string path(socket_path);
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) {
    std::cerr << "invalid unix socket path\n";
    return 1;
  }

  int server = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server < 0) {
    std::cerr << "unix socket failed: " << std::strerror(errno) << "\n";
    return 1;
  }

  unlink(path.c_str());
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.data(), path.size());
  if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "unix bind failed: " << std::strerror(errno) << "\n";
    close(server);
    return 1;
  }
#ifdef __linux__
  chmod(path.c_str(), 0666);
#endif
  return run_bound_server(server, index, params, workers);
}

}  // namespace rinha
