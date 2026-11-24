// Single-use login tickets. The TLS login service issues a ticket after verifying a password;
// the client then presents it on its ENet connection. A ticket is bound to the session nonce the
// server gave that ENet connection, so a ticket sniffed off the wire cannot be used from another
// connection. Thread-safe: issued on TLS worker threads, redeemed on the game thread.
// Phase 10: each ticket also carries a random session key. The client receives it over TLS with
// the ticket, and the game connection seals every packet with it once the ticket is redeemed.
#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "db/repository.hpp"
#include "net/packet_crypto.hpp"

namespace ef::auth {

// 32 random bytes as 64 lowercase hex chars (libsodium CSPRNG).
std::string random_hex_token(std::size_t bytes = 32);

class TicketStore {
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds kTtl{30};
  static constexpr std::size_t kMaxTickets = 10000;

  struct Issued {
    std::string ticket;
    net::SessionKey session_key{};
  };
  struct Redeemed {
    db::AccountRecord account;
    net::SessionKey session_key{};
  };

  // Returns the new ticket and its session key. Expired tickets are purged opportunistically.
  Issued issue(const db::AccountRecord& account, const std::string& session_nonce, Clock::time_point now);

  // Consumes the ticket if it exists, has not expired, and was issued for `session_nonce`.
  // A nonce mismatch does not consume the ticket (so a sniffer can't burn a legitimate login).
  std::optional<Redeemed> redeem(const std::string& ticket, const std::string& session_nonce, Clock::time_point now);

  std::size_t size() const;

 private:
  struct Entry {
    db::AccountRecord account;
    std::string nonce;
    net::SessionKey session_key{};
    Clock::time_point expires;
  };
  void purge_expired_locked(Clock::time_point now);

  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> tickets_;
};

}  // namespace ef::auth
