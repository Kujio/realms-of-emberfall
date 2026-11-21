#include "auth/tickets.hpp"

#include <vector>

#include <sodium.h>

namespace ef::auth {

std::string random_hex_token(std::size_t bytes) {
  std::vector<unsigned char> raw(bytes);
  randombytes_buf(raw.data(), raw.size());
  std::string hex(bytes * 2 + 1, '\0');
  sodium_bin2hex(hex.data(), hex.size(), raw.data(), raw.size());
  hex.pop_back();  // drop the terminating NUL
  return hex;
}

void TicketStore::purge_expired_locked(Clock::time_point now) {
  for (auto it = tickets_.begin(); it != tickets_.end();) {
    if (it->second.expires <= now) it = tickets_.erase(it);
    else ++it;
  }
}

std::string TicketStore::issue(const db::AccountRecord& account, const std::string& session_nonce, Clock::time_point now) {
  std::lock_guard lock(mu_);
  if (tickets_.size() >= kMaxTickets / 2) purge_expired_locked(now);
  if (tickets_.size() >= kMaxTickets) tickets_.erase(tickets_.begin());  // hard cap under abuse
  std::string ticket = random_hex_token();
  db::AccountRecord copy = account;
  copy.password_hash.clear();  // never keep hashes around longer than needed
  tickets_[ticket] = Entry{std::move(copy), session_nonce, now + kTtl};
  return ticket;
}

std::optional<db::AccountRecord> TicketStore::redeem(const std::string& ticket, const std::string& session_nonce,
                                                     Clock::time_point now) {
  std::lock_guard lock(mu_);
  auto it = tickets_.find(ticket);
  if (it == tickets_.end()) return std::nullopt;
  if (it->second.expires <= now) {
    tickets_.erase(it);
    return std::nullopt;
  }
  const auto& expected = it->second.nonce;
  if (expected.size() != session_nonce.size() || sodium_memcmp(expected.data(), session_nonce.data(), expected.size()) != 0) {
    return std::nullopt;
  }
  auto account = std::move(it->second.account);
  tickets_.erase(it);
  return account;
}

std::size_t TicketStore::size() const {
  std::lock_guard lock(mu_);
  return tickets_.size();
}

}  // namespace ef::auth
