#include "auth/password.hpp"

#include <array>

#include <sodium.h>

namespace ef::auth {

PasswordParams PasswordParams::interactive() {
  return {crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE};
}

PasswordParams PasswordParams::minimum() {
  return {crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN};
}

bool init_crypto() { return sodium_init() >= 0; }

std::optional<std::string> hash_password(std::string_view password, const PasswordParams& params) {
  std::array<char, crypto_pwhash_STRBYTES> out{};
  if (crypto_pwhash_str_alg(out.data(), password.data(), password.size(), params.opslimit, params.memlimit,
                            crypto_pwhash_ALG_ARGON2ID13) != 0) {
    return std::nullopt;
  }
  return std::string(out.data());
}

bool verify_password(const std::string& encoded_hash, std::string_view password) {
  if (encoded_hash.empty() || encoded_hash.size() >= crypto_pwhash_STRBYTES) return false;
  return crypto_pwhash_str_verify(encoded_hash.c_str(), password.data(), password.size()) == 0;
}

}  // namespace ef::auth
