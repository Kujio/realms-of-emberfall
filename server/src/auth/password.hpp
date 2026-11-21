#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ef::auth {

// Argon2id cost parameters. Production uses libsodium's INTERACTIVE limits;
// tests use the minimum so they stay fast.
struct PasswordParams {
  std::uint64_t opslimit = 0;
  std::size_t memlimit = 0;
  static PasswordParams interactive();
  static PasswordParams minimum();
};

// Must be called once before hashing. Returns false if libsodium failed to initialize.
bool init_crypto();

// Returns the encoded Argon2id hash string ($argon2id$...), or nullopt on failure (e.g. OOM).
std::optional<std::string> hash_password(std::string_view password, const PasswordParams& params);

// Constant-time verification against an encoded hash.
bool verify_password(const std::string& encoded_hash, std::string_view password);

}  // namespace ef::auth
