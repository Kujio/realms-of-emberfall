#include "auth/auth_service.hpp"

#include <future>

#include "auth/names.hpp"

namespace ef::auth {

namespace {
constexpr std::size_t kNonceMax = 64;
}

AuthService::AuthService(db::DbWorker& db, TicketStore& tickets, PasswordParams params)
    : db_(db), tickets_(tickets), params_(params) {
  auto dummy = hash_password("not-a-real-password-just-timing", params_);
  dummy_hash_ = std::make_shared<const std::string>(dummy.value_or(std::string()));
}

LoginOutcome AuthService::authenticate(const LoginRequest& req, std::chrono::milliseconds timeout) {
  LoginOutcome out;
  const std::string username = normalize_username(req.username);
  out.username = username;
  if (auto err = validate_username(username)) {
    out.message = *err;
    return out;
  }
  if (auto err = validate_password(req.password)) {
    out.message = *err;
    return out;
  }
  if (req.session_nonce.empty() || req.session_nonce.size() > kNonceMax) {
    out.message = "Invalid session. Please reconnect.";
    return out;
  }

  struct Result {
    bool ok = false;
    std::string message;
    db::AccountRecord account;
  };
  auto promise = std::make_shared<std::promise<Result>>();
  auto future = promise->get_future();
  const PasswordParams params = params_;
  auto dummy = dummy_hash_;
  const std::string password = req.password;
  const bool is_register = req.is_register;
  db_.post([promise, username, password, params, dummy, is_register](db::Repository& repo, db::DbWorker&) {
    Result r;
    if (is_register) {
      auto hash = hash_password(password, params);
      std::int64_t id = 0;
      const auto st = hash ? repo.create_account(username, *hash, id) : db::DbStatus::Error;
      if (st == db::DbStatus::Ok) {
        r.ok = true;
        r.account = {id, username, {}, false};
      } else {
        r.message = st == db::DbStatus::Duplicate ? "That username is already taken." : "Registration failed. Please try again.";
      }
    } else {
      auto acct = repo.find_account(username);
      if (acct && verify_password(acct->password_hash, password)) {
        r.ok = true;
        r.account = *acct;
      } else {
        if (!acct) verify_password(*dummy, password);
        r.message = "Invalid username or password.";
      }
    }
    promise->set_value(std::move(r));
  });

  if (future.wait_for(timeout) != std::future_status::ready) {
    out.message = "The server is busy. Please try again.";
    return out;
  }
  Result r;
  try {
    r = future.get();
  } catch (const std::future_error&) {  // job dropped (shutdown) or threw before answering
    out.message = "The server is busy. Please try again.";
    return out;
  }
  if (!r.ok) {
    out.message = r.message;
    return out;
  }
  out.ok = true;
  out.message = is_register ? "Account created." : "Signed in.";
  out.ticket = tickets_.issue(r.account, req.session_nonce, TicketStore::Clock::now());
  return out;
}

}  // namespace ef::auth
