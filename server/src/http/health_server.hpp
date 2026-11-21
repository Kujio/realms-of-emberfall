// Minimal HTTP/1.0 health endpoint on its own thread. GET /health returns JSON stats.
// Values are published by the game thread through atomics; no game state is touched here.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace ef::http {

struct HealthStats {
  std::atomic<std::uint32_t> players_online{0};
  std::atomic<std::uint32_t> sessions{0};
  std::atomic<std::uint32_t> server_tick{0};
  std::atomic<std::uint32_t> tick_us_avg{0};
  std::atomic<std::uint32_t> tick_us_max{0};
  std::atomic<std::uint32_t> db_queue{0};
  std::atomic<bool> shutting_down{false};
};

class HealthServer {
 public:
  HealthServer(const HealthStats& stats, std::uint32_t tick_rate, std::string tls_fingerprint);
  ~HealthServer();
  HealthServer(const HealthServer&) = delete;
  HealthServer& operator=(const HealthServer&) = delete;

  // Throws std::runtime_error if the socket cannot be bound.
  void start(const std::string& bind_address, std::uint16_t port);
  void stop();

  // Upper bound on reading one full request (slow-client protection).
  static constexpr std::chrono::milliseconds kRequestDeadline{2000};

  // Builds the JSON body (exposed for tests).
  std::string render_json() const;

 private:
  void run();
  void serve_client(int fd) const;

  const HealthStats& stats_;
  std::uint32_t tick_rate_;
  std::string tls_fingerprint_;
  std::int64_t started_unix_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace ef::http
