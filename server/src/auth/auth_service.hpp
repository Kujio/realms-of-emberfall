// Password verification and registration for the TLS login service. Blocking: callers are TLS
// worker threads. Hashing and DB access run on the DB worker thread; this waits for the result.
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "auth/password.hpp"
#include "auth/tickets.hpp"
#include "db/db_worker.hpp"

namespace ef::auth {

struct LoginRequest {
  std::string username;
  std::string password;
  bool is_register = false;
  std::string session_nonce;
};

struct LoginOutcome {
  bool ok = false;
  std::string message;
  std::string ticket;
  std::string username;  // normalized, for logging
};

class AuthService {
 public:
  AuthService(db::DbWorker& db, TicketStore& tickets, PasswordParams params);

  LoginOutcome authenticate(const LoginRequest& req, std::chrono::milliseconds timeout);

 private:
  db::DbWorker& db_;
  TicketStore& tickets_;
  PasswordParams params_;
  std::shared_ptr<const std::string> dummy_hash_;  // equalizes timing for unknown usernames
};

}  // namespace ef::auth
