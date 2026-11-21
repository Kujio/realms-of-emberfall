#include "net/tls_login_server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <memory>
#include <system_error>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <spdlog/spdlog.h>

#include "net/protocol.gen.hpp"

namespace ef::net {

namespace {

using Clock = std::chrono::steady_clock;

double now_seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

bool wait_fd(int fd, short events, Clock::time_point deadline, const std::atomic<bool>& stop) {
  while (!stop) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) return false;
    pollfd pfd{fd, events, 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(std::min<long long>(left, 200)));
    if (rc > 0) return true;
    if (rc < 0 && errno != EINTR) return false;
  }
  return false;
}

std::string peer_ip(const sockaddr_storage& ss) {
  std::array<char, INET6_ADDRSTRLEN> buf{};
  if (ss.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
    ::inet_ntop(AF_INET, &a->sin_addr, buf.data(), buf.size());
  } else if (ss.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
    ::inet_ntop(AF_INET6, &a->sin6_addr, buf.data(), buf.size());
  }
  return buf.data();
}

struct SslFree {
  void operator()(SSL* s) const { SSL_free(s); }
};

}  // namespace

TlsLoginServer::TlsLoginServer(const TlsIdentity& identity, auth::AuthService& auth, TlsLoginConfig cfg)
    : auth_(auth), cfg_(std::move(cfg)) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (!ctx) throw TlsError("SSL_CTX_new failed");
  ctx_ = ctx;
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);

  std::unique_ptr<BIO, decltype(&BIO_free)> cbio(BIO_new_mem_buf(identity.cert_pem.data(), static_cast<int>(identity.cert_pem.size())), &BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> kbio(BIO_new_mem_buf(identity.key_pem.data(), static_cast<int>(identity.key_pem.size())), &BIO_free);
  X509* cert = cbio ? PEM_read_bio_X509(cbio.get(), nullptr, nullptr, nullptr) : nullptr;
  EVP_PKEY* key = kbio ? PEM_read_bio_PrivateKey(kbio.get(), nullptr, nullptr, nullptr) : nullptr;
  const bool ok = cert && key && SSL_CTX_use_certificate(ctx, cert) == 1 && SSL_CTX_use_PrivateKey(ctx, key) == 1 &&
                  SSL_CTX_check_private_key(ctx) == 1;
  X509_free(cert);
  EVP_PKEY_free(key);
  if (!ok) {
    SSL_CTX_free(ctx);
    ctx_ = nullptr;
    throw TlsError("cannot load TLS identity into the login server");
  }
}

TlsLoginServer::~TlsLoginServer() {
  stop();
  SSL_CTX_free(ctx_);
}

void TlsLoginServer::start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);  // nonblocking: accept() after poll() can race
  if (listen_fd_ < 0) throw TlsError("login: socket() failed");
  const int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.port);
  if (::inet_pton(AF_INET, cfg_.bind_address.c_str(), &addr.sin_addr) != 1) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw TlsError("login: invalid bind address " + cfg_.bind_address);
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 64) != 0) {
    const std::string err = std::system_category().message(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    throw TlsError("login: cannot listen on " + cfg_.bind_address + ":" + std::to_string(cfg_.port) + ": " + err);
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &blen);
  bound_port_ = ntohs(bound.sin_port);
  stop_ = false;
  for (std::size_t i = 0; i < cfg_.workers; ++i) workers_.emplace_back([this] { worker_loop(); });
  acceptor_ = std::thread([this] { accept_loop(); });
  spdlog::info("TLS login service listening on tcp://{}:{}", cfg_.bind_address, bound_port_);
}

void TlsLoginServer::stop() {
  {
    // Set the flag under the queue mutex: a worker that has checked the predicate but not yet
    // started waiting would otherwise miss the notification and block join() forever.
    std::lock_guard lock(q_mu_);
    stop_ = true;
  }
  q_cv_.notify_all();
  if (acceptor_.joinable()) acceptor_.join();
  for (auto& w : workers_)
    if (w.joinable()) w.join();
  workers_.clear();
  {
    std::lock_guard lock(q_mu_);
    for (const auto& j : queue_) ::close(j.fd);
    queue_.clear();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

bool TlsLoginServer::allow_ip(const std::string& ip) {
  std::lock_guard lock(ip_mu_);
  if (per_ip_.size() > 10000) per_ip_.clear();  // crude bound under address-spraying floods
  auto it = per_ip_.try_emplace(ip, cfg_.attempts_per_second_per_ip, cfg_.attempt_burst_per_ip).first;
  return it->second.consume(1.0, now_seconds());
}

void TlsLoginServer::accept_loop() {
  while (!stop_) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    if (::poll(&pfd, 1, 200) <= 0) continue;
    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&ss), &slen, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) continue;
    std::string ip = peer_ip(ss);
    if (!allow_ip(ip)) {
      spdlog::warn("TLS login: rate limit hit for {}", ip);
      ::close(fd);
      continue;
    }
    std::lock_guard lock(q_mu_);
    if (queue_.size() >= cfg_.queue_capacity) {
      ::close(fd);  // overloaded: shed load rather than queue unbounded work
      continue;
    }
    queue_.push_back({fd, std::move(ip)});
    q_cv_.notify_one();
  }
}

void TlsLoginServer::worker_loop() {
  for (;;) {
    Job job{};
    {
      std::unique_lock lock(q_mu_);
      q_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_) return;
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    handle(job.fd, job.ip);
    ::close(job.fd);
  }
}

void TlsLoginServer::handle(int fd, const std::string& ip) {
  const SigpipeGuard no_sigpipe;
  const auto deadline = Clock::now() + cfg_.io_deadline;
  std::unique_ptr<SSL, SslFree> ssl(SSL_new(ctx_));
  if (!ssl) return;
  SSL_set_fd(ssl.get(), fd);

  auto drive = [&](auto&& op) -> int {
    for (;;) {
      const int rc = op();
      if (rc > 0) return rc;
      const int e = SSL_get_error(ssl.get(), rc);
      if (e == SSL_ERROR_WANT_READ && wait_fd(fd, POLLIN, deadline, stop_)) continue;
      if (e == SSL_ERROR_WANT_WRITE && wait_fd(fd, POLLOUT, deadline, stop_)) continue;
      ERR_clear_error();
      return -1;
    }
  };
  auto read_exact = [&](std::uint8_t* dst, std::size_t len) -> bool {
    std::size_t got = 0;
    while (got < len) {
      const int n = drive([&] { return SSL_read(ssl.get(), dst + got, static_cast<int>(len - got)); });
      if (n < 0) return false;
      got += static_cast<std::size_t>(n);
    }
    return true;
  };
  auto send_result = [&](const proto::TlsLoginResult& r) {
    const auto pkt = proto::encode_packet(r);
    const auto out = frame(pkt);
    std::size_t sent = 0;
    while (sent < out.size()) {
      const int n = drive([&] { return SSL_write(ssl.get(), out.data() + sent, static_cast<int>(out.size() - sent)); });
      if (n < 0) return;
      sent += static_cast<std::size_t>(n);
    }
    SSL_shutdown(ssl.get());
  };

  if (drive([&] { return SSL_accept(ssl.get()); }) < 0) {
    spdlog::debug("TLS login: handshake with {} failed or timed out", ip);
    return;
  }
  std::array<std::uint8_t, 2> hdr{};
  if (!read_exact(hdr.data(), hdr.size())) return;
  const std::size_t len = static_cast<std::size_t>(hdr[0]) | (static_cast<std::size_t>(hdr[1]) << 8);
  if (len == 0 || len > kMaxTlsFrame) {
    spdlog::warn("TLS login: bad frame length {} from {}", len, ip);
    return;
  }
  std::vector<std::uint8_t> body(len);
  if (!read_exact(body.data(), len)) return;

  proto::TlsLogin req;
  if (!proto::decode_packet(body, req)) {
    spdlog::warn("TLS login: malformed request from {}", ip);
    send_result({false, "Malformed login request.", ""});
    return;
  }
  auto outcome = auth_.authenticate({req.username, req.password, req.is_register, req.session_nonce}, cfg_.auth_timeout);
  if (outcome.ok) {
    ++ok_;
    spdlog::info("TLS login: '{}' {} from {}", outcome.username, req.is_register ? "registered" : "signed in", ip);
  } else {
    ++failed_;
    spdlog::info("TLS login: failed for '{}' from {}: {}", outcome.username, ip, outcome.message);
  }
  send_result({outcome.ok, outcome.message, outcome.ticket});
}

}  // namespace ef::net
