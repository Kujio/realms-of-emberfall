// Ticket store, TLS identity, and the TLS login service over real loopback sockets.
#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <filesystem>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/auth_service.hpp"
#include "db/sqlite_repository.hpp"
#include "net/protocol.gen.hpp"
#include "net/tls.hpp"
#include "net/tls_login_server.hpp"

using namespace ef;
using Clock = auth::TicketStore::Clock;

namespace {

std::filesystem::path temp_dir(const char* name) {
  // ctest runs cases in parallel processes; keep each one's files apart.
  auto p = std::filesystem::temp_directory_path() / (std::string(name) + "_" + std::to_string(::getpid()));
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return p;
}

struct RawResult {
  std::string data;
  bool server_closed = false;  // true = the server closed/reset us; false = our receive timed out
};

// Opens a plain TCP connection, writes raw bytes, and reads until the connection ends.
RawResult raw_exchange(std::uint16_t port, const std::string& bytes) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  timeval tv{3, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  RawResult out;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
    if (!bytes.empty()) ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    char buf[512];
    for (;;) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n > 0) {
        out.data.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      out.server_closed = n == 0 || errno == ECONNRESET;  // EAGAIN = our 3 s timeout fired instead
      break;
    }
  }
  ::close(fd);
  return out;
}

struct LoginFixture {
  std::filesystem::path dir = temp_dir("emberfall_tls_test");
  net::TlsIdentity identity = net::load_or_create_tls_identity((dir / "c.crt").string(), (dir / "c.key").string());
  db::DbWorker db{std::make_unique<db::SqliteRepository>(":memory:"), db::DbWorker::Mode::Threaded};
  auth::TicketStore tickets;
  auth::AuthService auth{db, tickets, auth::PasswordParams::minimum()};
  std::unique_ptr<net::TlsLoginServer> server;

  explicit LoginFixture(net::TlsLoginConfig cfg = {}) {
    REQUIRE(auth::init_crypto());
    cfg.bind_address = "127.0.0.1";
    cfg.port = 0;
    cfg.io_deadline = std::chrono::milliseconds(2000);
    server = std::make_unique<net::TlsLoginServer>(identity, auth, cfg);
    server->start();
  }
  ~LoginFixture() {
    server->stop();
    db.stop_and_flush();
    std::filesystem::remove_all(dir);
  }

  proto::TlsLoginResult login(const std::string& user, const std::string& pass, bool reg, const std::string& nonce) {
    const auto req = proto::encode_packet(proto::TlsLogin{user, pass, reg, nonce});
    const auto resp = net::tls_request("127.0.0.1", server->port(), identity.cert_pem, req, std::chrono::seconds(5));
    proto::TlsLoginResult r;
    REQUIRE(proto::decode_packet(resp, r));
    return r;
  }
};

}  // namespace

TEST_CASE("ticket store: single use, nonce bound, expiring", "[tls]") {
  REQUIRE(auth::init_crypto());
  auth::TicketStore store;
  const auto t0 = Clock::now();
  const db::AccountRecord acct{7, "alice", "secret-hash", false, 0, {}, 0};
  const auto ticket = store.issue(acct, "nonce-a", t0);
  CHECK(ticket.size() == 64);
  CHECK_FALSE(store.redeem(ticket, "nonce-b", t0).has_value());  // wrong connection
  auto got = store.redeem(ticket, "nonce-a", t0);                 // mismatch did not burn it
  REQUIRE(got.has_value());
  CHECK(got->id == 7);
  CHECK(got->password_hash.empty());
  CHECK_FALSE(store.redeem(ticket, "nonce-a", t0).has_value());  // single use

  const auto old = store.issue(acct, "n", t0);
  CHECK_FALSE(store.redeem(old, "n", t0 + auth::TicketStore::kTtl + std::chrono::seconds(1)).has_value());
  CHECK(store.size() == 0);
  CHECK(auth::random_hex_token() != auth::random_hex_token());
}

TEST_CASE("TLS identity is generated once, persisted, and reloaded", "[tls]") {
  const auto dir = temp_dir("emberfall_tls_identity");
  const auto cert = (dir / "tls" / "server.crt").string();
  const auto key = (dir / "tls" / "server.key").string();
  const auto first = net::load_or_create_tls_identity(cert, key);
  CHECK(first.fingerprint.size() == 95);  // 32 bytes as AB:CD:...
  CHECK(net::cert_fingerprint(first.cert_pem) == first.fingerprint);
  struct stat st {};
  REQUIRE(::stat(key.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);

  const auto again = net::load_or_create_tls_identity(cert, key);
  CHECK(again.fingerprint == first.fingerprint);

  std::filesystem::remove(key);  // half an identity must not be silently replaced
  CHECK_THROWS_AS(net::load_or_create_tls_identity(cert, key), net::TlsError);
  std::filesystem::remove_all(dir);
  CHECK(net::cert_fingerprint("not a cert").empty());
}

TEST_CASE("TLS login service registers, signs in, and issues bound tickets", "[tls]") {
  LoginFixture f;
  auto reg = f.login("tlsuser", "password123", true, "nonce-1");
  REQUIRE(reg.ok);
  CHECK(reg.ticket.size() == 64);
  CHECK(f.tickets.redeem(reg.ticket, "nonce-1", Clock::now()).has_value());

  auto dup = f.login("tlsuser", "password123", true, "nonce-2");
  CHECK_FALSE(dup.ok);
  CHECK(dup.message.find("taken") != std::string::npos);

  auto bad = f.login("tlsuser", "wrong-password", false, "nonce-3");
  CHECK_FALSE(bad.ok);
  CHECK(bad.ticket.empty());

  auto good = f.login("TLSUSER", "password123", false, "nonce-4");
  CHECK(good.ok);

  auto invalid = f.login("x", "password123", false, "nonce-5");
  CHECK_FALSE(invalid.ok);
  CHECK(f.server->logins_ok() == 2);
}

TEST_CASE("TLS client refuses a server whose certificate is not the pinned one", "[tls]") {
  LoginFixture f;
  const auto other_dir = temp_dir("emberfall_tls_other");
  const auto other = net::load_or_create_tls_identity((other_dir / "o.crt").string(), (other_dir / "o.key").string());
  const auto req = proto::encode_packet(proto::TlsLogin{"someone", "password123", true, "n"});
  CHECK_THROWS_AS(net::tls_request("127.0.0.1", f.server->port(), other.cert_pem, req, std::chrono::seconds(3)), net::TlsError);
  std::filesystem::remove_all(other_dir);
}

TEST_CASE("TLS login service survives garbage, oversized frames, and silent clients", "[tls][fuzz]") {
  LoginFixture f;
  const auto http = raw_exchange(f.server->port(), "GET / HTTP/1.0\r\n\r\n");
  CHECK(http.data.find("HTTP") == std::string::npos);
  CHECK(http.server_closed);
  const auto bad_handshake = raw_exchange(f.server->port(), std::string(5000, '\x16'));
  CHECK(bad_handshake.data.empty());
  CHECK(bad_handshake.server_closed);
  // Connect and say nothing: the server must close us at its 2 s I/O deadline, before our own
  // 3 s receive timeout ends the wait (which would report server_closed == false).
  CHECK(raw_exchange(f.server->port(), "").server_closed);
  // An oversized frame inside a valid TLS session is rejected without a response.
  std::vector<std::uint8_t> huge(net::kMaxTlsFrame + 10, 0x41);
  CHECK_THROWS_AS(net::tls_request("127.0.0.1", f.server->port(), f.identity.cert_pem, huge, std::chrono::seconds(3)), net::TlsError);
  // A malformed (but small) request gets a clear refusal.
  std::vector<std::uint8_t> junk{10, 0, 1, 2, 3};
  auto resp = net::tls_request("127.0.0.1", f.server->port(), f.identity.cert_pem, junk, std::chrono::seconds(3));
  proto::TlsLoginResult r;
  REQUIRE(proto::decode_packet(resp, r));
  CHECK_FALSE(r.ok);
  // And it still serves real logins afterwards.
  CHECK(f.login("survivor", "password123", true, "n").ok);
}

TEST_CASE("TLS login service rate-limits per IP", "[tls]") {
  net::TlsLoginConfig cfg;
  cfg.attempt_burst_per_ip = 3;
  cfg.attempts_per_second_per_ip = 0.001;
  LoginFixture f(cfg);
  int ok = 0;
  int refused = 0;
  for (int i = 0; i < 6; ++i) {
    try {
      f.login("ratelimited", "password123", true, "n");
      ++ok;
    } catch (const net::TlsError&) {
      ++refused;
    }
  }
  CHECK(ok == 3);
  CHECK(refused == 3);
}
