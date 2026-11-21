// TLS identity (self-signed, pinned by clients) and a small blocking TLS client used by the bot
// and tests. Built on OpenSSL 3.
#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ef::net {

// Name in the certificate's CN/SAN. Clients verify against this name (the certificate itself is
// pinned), so it does not need to match the server's DNS name or IP.
inline constexpr const char* kTlsServerName = "emberfall-server";

// Largest TLS login frame either side accepts (u16 length prefix + packet).
inline constexpr std::size_t kMaxTlsFrame = 4096;

struct TlsIdentity {
  std::string cert_pem;
  std::string key_pem;
  std::string fingerprint;  // SHA-256 of the certificate DER, "AB:CD:..."
};

class TlsError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Loads the certificate and key, or, when neither file exists, generates a self-signed P-256
// identity valid for 10 years and writes it (key with mode 0600). Throws TlsError if only one
// file exists, the files don't parse, or the key doesn't match the certificate.
TlsIdentity load_or_create_tls_identity(const std::string& cert_path, const std::string& key_path);

// SHA-256 fingerprint of a PEM certificate ("AB:CD:..."), or "" if it doesn't parse.
std::string cert_fingerprint(const std::string& cert_pem);

// Blocks SIGPIPE on the calling thread for its lifetime and discards any SIGPIPE raised meanwhile,
// so writing to a peer that hung up returns an error instead of killing the process.
class SigpipeGuard {
 public:
  SigpipeGuard();
  ~SigpipeGuard();
  SigpipeGuard(const SigpipeGuard&) = delete;
  SigpipeGuard& operator=(const SigpipeGuard&) = delete;

 private:
  bool was_pending_ = false;
  bool was_blocked_ = false;
};

// u16 little-endian length prefix + payload.
std::vector<std::uint8_t> frame(std::span<const std::uint8_t> payload);

// Connects to host:port, requires the server to present exactly `pinned_cert_pem`, sends one
// framed request, and returns the payload of the framed response. Throws TlsError on any failure.
std::vector<std::uint8_t> tls_request(const std::string& host, std::uint16_t port, const std::string& pinned_cert_pem,
                                      std::span<const std::uint8_t> request, std::chrono::milliseconds timeout);

}  // namespace ef::net
