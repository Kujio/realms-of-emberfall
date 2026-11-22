#include "http/health_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "core/process_stats.hpp"

namespace ef::http {

namespace {
std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void write_all(int fd, std::string_view data) {
  while (!data.empty()) {
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n <= 0) return;
    data.remove_prefix(static_cast<std::size_t>(n));
  }
}
}  // namespace

HealthServer::HealthServer(const HealthStats& stats, std::uint32_t tick_rate, std::string tls_fingerprint)
    : stats_(stats), tick_rate_(tick_rate), tls_fingerprint_(std::move(tls_fingerprint)), started_unix_(unix_now()) {}

HealthServer::~HealthServer() { stop(); }

void HealthServer::start(const std::string& bind_address, std::uint16_t port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);  // nonblocking: accept() after poll() can race
  if (listen_fd_ < 0) throw std::runtime_error("health: socket() failed");
  const int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw std::runtime_error("health: invalid bind address " + bind_address);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 16) != 0) {
    const std::string err = std::system_category().message(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw std::runtime_error("health: cannot listen on " + bind_address + ":" + std::to_string(port) + ": " + err);
  }
  thread_ = std::thread([this] { run(); });
  spdlog::info("health endpoint listening on http://{}:{}/health", bind_address, port);
}

void HealthServer::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

std::string HealthServer::render_json() const {
  nlohmann::json j;
  j["status"] = stats_.shutting_down.load() ? "shutting_down" : "ok";
  j["players_online"] = stats_.players_online.load();
  j["sessions"] = stats_.sessions.load();
  j["server_tick"] = stats_.server_tick.load();
  j["tick_rate"] = tick_rate_;
  j["tick_ms_avg"] = static_cast<double>(stats_.tick_us_avg.load()) / 1000.0;
  j["tick_ms_max"] = static_cast<double>(stats_.tick_us_max.load()) / 1000.0;
  j["tick_ms_p99"] = static_cast<double>(stats_.tick_us_p99.load()) / 1000.0;
  j["tick_budget_ms"] = tick_rate_ > 0 ? 1000.0 / static_cast<double>(tick_rate_) : 0.0;
  j["cpu_pct"] = static_cast<double>(stats_.cpu_permille.load()) / 10.0;
  j["bytes_sent"] = stats_.bytes_sent.load();
  j["bytes_received"] = stats_.bytes_received.load();
  j["send_bytes_per_s"] = stats_.send_bps.load();
  j["receive_bytes_per_s"] = stats_.receive_bps.load();
  const auto players = stats_.players_online.load();
  j["send_bytes_per_s_per_player"] = players > 0 ? stats_.send_bps.load() / players : 0;
  j["zones_loaded"] = stats_.zones_loaded.load();
  j["lingering"] = stats_.lingering.load();
  static constexpr const char* kPhaseNames[HealthStats::kPhases] = {"players", "mobs", "combat", "world", "interest", "snapshots", "state"};
  nlohmann::json phases = nlohmann::json::object();
  for (std::size_t i = 0; i < HealthStats::kPhases; ++i) phases[kPhaseNames[i]] = static_cast<double>(stats_.phase_us[i].load()) / 1000.0;
  j["tick_phase_ms"] = phases;
  j["db_queue"] = stats_.db_queue.load();
  j["rss_kib"] = process_rss_kib();
  j["uptime_s"] = unix_now() - started_unix_;
  j["tls_fingerprint"] = tls_fingerprint_;
  return j.dump();
}

void HealthServer::run() {
  while (!stop_) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, 200);
    if (rc <= 0) continue;
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) continue;
    serve_client(fd);
    ::close(fd);
  }
}

void HealthServer::serve_client(int fd) const {
  timeval tv{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  // SO_RCVTIMEO only bounds each recv(); a client trickling one byte at a time could otherwise
  // hold this single-threaded endpoint for minutes. Enforce a deadline for the whole request.
  const auto deadline = std::chrono::steady_clock::now() + kRequestDeadline;
  std::array<char, 2048> buf{};
  std::size_t used = 0;
  bool complete = false;
  while (used < buf.size() && !stop_) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) break;
    pollfd pfd{fd, POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(std::min<long long>(left, 200))) <= 0) continue;
    const ssize_t n = ::recv(fd, buf.data() + used, buf.size() - used, 0);
    if (n <= 0) break;
    used += static_cast<std::size_t>(n);
    if (std::string_view(buf.data(), used).find("\r\n\r\n") != std::string_view::npos) {
      complete = true;
      break;
    }
  }
  if (!complete && used < buf.size()) return;  // timed out, disconnected, or shutting down
  const std::string_view req(buf.data(), used);
  std::string body;
  std::string status;
  if (req.starts_with("GET /health ") || req.starts_with("GET /health?")) {
    body = render_json();
    status = "200 OK";
  } else {
    body = R"({"error":"not found"})";
    status = "404 Not Found";
  }
  std::string resp = "HTTP/1.0 " + status + "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  write_all(fd, resp);
}

}  // namespace ef::http
