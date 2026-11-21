// TLS login service (TCP). Accepts one framed TlsLogin per connection, verifies or registers the
// account through AuthService, and answers with a TlsLoginResult carrying a single-use ticket.
// Runs on its own accept thread plus a small worker pool; never touches game state directly.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "auth/auth_service.hpp"
#include "net/rate_limiter.hpp"
#include "net/tls.hpp"

struct ssl_ctx_st;

namespace ef::net {

struct TlsLoginConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 7779;               // 0 = pick a free port (tests)
  std::size_t workers = 4;
  std::size_t queue_capacity = 64;
  std::chrono::milliseconds io_deadline{10000};    // handshake + request + response
  std::chrono::milliseconds auth_timeout{15000};   // waiting on the DB worker
  double attempts_per_second_per_ip = 0.2;         // 12/min sustained
  double attempt_burst_per_ip = 10.0;
};

class TlsLoginServer {
 public:
  TlsLoginServer(const TlsIdentity& identity, auth::AuthService& auth, TlsLoginConfig cfg);
  ~TlsLoginServer();
  TlsLoginServer(const TlsLoginServer&) = delete;
  TlsLoginServer& operator=(const TlsLoginServer&) = delete;

  // Binds and starts threads. Throws TlsError on failure.
  void start();
  void stop();
  std::uint16_t port() const { return bound_port_; }

  std::uint64_t logins_ok() const { return ok_.load(); }
  std::uint64_t logins_failed() const { return failed_.load(); }

 private:
  struct Job {
    int fd;
    std::string ip;
  };
  void accept_loop();
  void worker_loop();
  void handle(int fd, const std::string& ip);
  bool allow_ip(const std::string& ip);

  ssl_ctx_st* ctx_ = nullptr;
  auth::AuthService& auth_;
  TlsLoginConfig cfg_;
  int listen_fd_ = -1;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread acceptor_;
  std::vector<std::thread> workers_;

  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::deque<Job> queue_;

  std::mutex ip_mu_;
  std::unordered_map<std::string, TokenBucket> per_ip_;

  std::atomic<std::uint64_t> ok_{0};
  std::atomic<std::uint64_t> failed_{0};
};

}  // namespace ef::net
