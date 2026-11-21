#include "net/tls.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <csignal>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace ef::net {

namespace {

template <auto Fn>
struct Deleter {
  template <class T>
  void operator()(T* p) const {
    Fn(p);
  }
};
using BioPtr = std::unique_ptr<BIO, Deleter<BIO_free_all>>;
using X509Ptr = std::unique_ptr<X509, Deleter<X509_free>>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY_free>>;
using BnPtr = std::unique_ptr<BIGNUM, Deleter<BN_free>>;
using CtxPtr = std::unique_ptr<SSL_CTX, Deleter<SSL_CTX_free>>;
using SslPtr = std::unique_ptr<SSL, Deleter<SSL_free>>;

std::string openssl_error() {
  std::array<char, 256> buf{};
  const unsigned long e = ERR_get_error();
  if (e == 0) return "unknown OpenSSL error";
  ERR_error_string_n(e, buf.data(), buf.size());
  return buf.data();
}

std::string bio_to_string(BIO* bio) {
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  return len > 0 ? std::string(data, static_cast<std::size_t>(len)) : std::string();
}

X509Ptr parse_cert(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio) return nullptr;
  return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

PkeyPtr parse_key(const std::string& pem) {
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  if (!bio) return nullptr;
  return PkeyPtr(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::string fingerprint_of(X509* cert) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> md{};
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), md.data(), &len) != 1) return {};
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned int i = 0; i < len; ++i) {
    if (i) out.push_back(':');
    out.push_back(kHex[md[i] >> 4]);
    out.push_back(kHex[md[i] & 0xF]);
  }
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_file(const std::string& path, const std::string& data, mode_t mode) {
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent);
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) throw TlsError("cannot write " + path);
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n <= 0) {
      ::close(fd);
      throw TlsError("cannot write " + path);
    }
    off += static_cast<std::size_t>(n);
  }
  ::fchmod(fd, mode);
  ::close(fd);
}

void add_ext(X509* cert, int nid, const char* value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  if (!ext) throw TlsError("cannot build certificate extension: " + openssl_error());
  X509_add_ext(cert, ext, -1);
  X509_EXTENSION_free(ext);
}

TlsIdentity generate_identity() {
  PkeyPtr key(EVP_EC_gen("P-256"));
  if (!key) throw TlsError("cannot generate EC key: " + openssl_error());
  X509Ptr cert(X509_new());
  if (!cert) throw TlsError("X509_new failed");
  X509_set_version(cert.get(), 2);
  BnPtr serial(BN_new());
  BN_rand(serial.get(), 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);
  BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(cert.get()));
  X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
  X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 3650);
  X509_set_pubkey(cert.get(), key.get());
  X509_NAME* name = X509_get_subject_name(cert.get());
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Realms of Emberfall"), -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(kTlsServerName), -1, -1, 0);
  X509_set_issuer_name(cert.get(), name);
  add_ext(cert.get(), NID_basic_constraints, "critical,CA:TRUE");
  add_ext(cert.get(), NID_key_usage, "critical,digitalSignature,keyCertSign");
  add_ext(cert.get(), NID_ext_key_usage, "serverAuth");
  add_ext(cert.get(), NID_subject_alt_name, (std::string("DNS:") + kTlsServerName).c_str());
  add_ext(cert.get(), NID_subject_key_identifier, "hash");
  if (X509_sign(cert.get(), key.get(), EVP_sha256()) == 0) throw TlsError("cannot sign certificate: " + openssl_error());

  BioPtr cbio(BIO_new(BIO_s_mem()));
  BioPtr kbio(BIO_new(BIO_s_mem()));
  PEM_write_bio_X509(cbio.get(), cert.get());
  PEM_write_bio_PrivateKey(kbio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
  return {bio_to_string(cbio.get()), bio_to_string(kbio.get()), fingerprint_of(cert.get())};
}

// Waits for fd readiness until the deadline. Returns false on timeout/error.
bool wait_fd(int fd, short events, std::chrono::steady_clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
  if (left <= 0) return false;
  pollfd pfd{fd, events, 0};
  return ::poll(&pfd, 1, static_cast<int>(left)) > 0;
}

}  // namespace

SigpipeGuard::SigpipeGuard() {
  sigset_t pending;
  sigemptyset(&pending);
  sigpending(&pending);
  was_pending_ = sigismember(&pending, SIGPIPE) == 1;
  sigset_t block;
  sigset_t old;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &block, &old);
  was_blocked_ = sigismember(&old, SIGPIPE) == 1;
}

SigpipeGuard::~SigpipeGuard() {
  if (!was_pending_) {
    sigset_t pending;
    sigemptyset(&pending);
    sigpending(&pending);
    if (sigismember(&pending, SIGPIPE) == 1) {
      sigset_t pipe;
      sigemptyset(&pipe);
      sigaddset(&pipe, SIGPIPE);
      const timespec zero{0, 0};
      while (sigtimedwait(&pipe, nullptr, &zero) == -1 && errno == EINTR) {
      }
    }
  }
  if (!was_blocked_) {
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGPIPE);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
  }
}

std::string cert_fingerprint(const std::string& cert_pem) {
  auto cert = parse_cert(cert_pem);
  return cert ? fingerprint_of(cert.get()) : std::string();
}

TlsIdentity load_or_create_tls_identity(const std::string& cert_path, const std::string& key_path) {
  const bool have_cert = std::filesystem::exists(cert_path);
  const bool have_key = std::filesystem::exists(key_path);
  if (have_cert != have_key) {
    throw TlsError("TLS certificate/key mismatch: found only " + (have_cert ? cert_path : key_path) +
                   ". Restore the missing file or delete both to generate a new identity (clients will see a new fingerprint).");
  }
  if (!have_cert) {
    TlsIdentity id = generate_identity();
    write_file(key_path, id.key_pem, 0600);
    write_file(cert_path, id.cert_pem, 0644);
    return id;
  }
  TlsIdentity id{read_file(cert_path), read_file(key_path), {}};
  auto cert = parse_cert(id.cert_pem);
  auto key = parse_key(id.key_pem);
  if (!cert || !key) throw TlsError("cannot parse TLS certificate/key at " + cert_path + " / " + key_path);
  if (X509_check_private_key(cert.get(), key.get()) != 1) throw TlsError("TLS key does not match certificate " + cert_path);
  id.fingerprint = fingerprint_of(cert.get());
  return id;
}

std::vector<std::uint8_t> frame(std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> out;
  out.reserve(payload.size() + 2);
  out.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
  out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::uint8_t> tls_request(const std::string& host, std::uint16_t port, const std::string& pinned_cert_pem,
                                      std::span<const std::uint8_t> request, std::chrono::milliseconds timeout) {
  const SigpipeGuard no_sigpipe;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto pinned = parse_cert(pinned_cert_pem);
  if (!pinned) throw TlsError("pinned certificate does not parse");

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) throw TlsError("cannot resolve " + host);
  int fd = -1;
  for (addrinfo* ai = res; ai && fd < 0; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) continue;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
      ::close(fd);
      fd = -1;
      continue;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    if (!wait_fd(fd, POLLOUT, deadline) || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
      ::close(fd);
      fd = -1;
    }
  }
  ::freeaddrinfo(res);
  if (fd < 0) throw TlsError("cannot connect to " + host + ":" + std::to_string(port));
  struct FdGuard {
    int fd;
    ~FdGuard() { ::close(fd); }
  } guard{fd};

  CtxPtr ctx(SSL_CTX_new(TLS_client_method()));
  if (!ctx) throw TlsError("SSL_CTX_new failed");
  SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
  X509_STORE_add_cert(SSL_CTX_get_cert_store(ctx.get()), pinned.get());
  SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
  SslPtr ssl(SSL_new(ctx.get()));
  SSL_set_fd(ssl.get(), fd);
  SSL_set1_host(ssl.get(), kTlsServerName);
  // SSL_set_tlsext_host_name() is a macro with a C-style cast; call what it expands to.
  SSL_ctrl(ssl.get(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, const_cast<char*>(kTlsServerName));

  // Drives a non-blocking SSL call until it completes or the deadline passes.
  auto drive = [&](auto&& op) -> int {
    for (;;) {
      const int rc = op();
      if (rc > 0) return rc;
      const int e = SSL_get_error(ssl.get(), rc);
      if (e == SSL_ERROR_WANT_READ && wait_fd(fd, POLLIN, deadline)) continue;
      if (e == SSL_ERROR_WANT_WRITE && wait_fd(fd, POLLOUT, deadline)) continue;
      return -1;
    }
  };
  if (drive([&] { return SSL_connect(ssl.get()); }) < 0) {
    const long vr = SSL_get_verify_result(ssl.get());
    throw TlsError(vr != X509_V_OK ? std::string("server certificate rejected: ") + X509_verify_cert_error_string(vr)
                                   : "TLS handshake failed: " + openssl_error());
  }
  X509Ptr peer(SSL_get1_peer_certificate(ssl.get()));
  if (!peer || X509_cmp(peer.get(), pinned.get()) != 0) throw TlsError("server presented a different certificate than the pinned one");

  const auto out = frame(request);
  std::size_t sent = 0;
  while (sent < out.size()) {
    const int n = drive([&] { return SSL_write(ssl.get(), out.data() + sent, static_cast<int>(out.size() - sent)); });
    if (n < 0) throw TlsError("TLS write failed");
    sent += static_cast<std::size_t>(n);
  }
  auto read_exact = [&](std::uint8_t* dst, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
      const int n = drive([&] { return SSL_read(ssl.get(), dst + got, static_cast<int>(len - got)); });
      if (n < 0) throw TlsError("TLS read failed or timed out");
      got += static_cast<std::size_t>(n);
    }
  };
  std::array<std::uint8_t, 2> hdr{};
  read_exact(hdr.data(), hdr.size());
  const std::size_t len = static_cast<std::size_t>(hdr[0]) | (static_cast<std::size_t>(hdr[1]) << 8);
  if (len == 0 || len > kMaxTlsFrame) throw TlsError("bad TLS response frame");
  std::vector<std::uint8_t> body(len);
  read_exact(body.data(), len);
  SSL_shutdown(ssl.get());
  return body;
}

}  // namespace ef::net
