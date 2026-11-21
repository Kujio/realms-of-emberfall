#include <catch2/catch_test_macros.hpp>

#include "auth/names.hpp"
#include "auth/password.hpp"

using namespace ef::auth;

TEST_CASE("argon2id hash and verify", "[auth]") {
  REQUIRE(init_crypto());
  auto h = hash_password("correct horse battery", PasswordParams::minimum());
  REQUIRE(h.has_value());
  CHECK(h->starts_with("$argon2id$"));
  CHECK(verify_password(*h, "correct horse battery"));
  CHECK_FALSE(verify_password(*h, "wrong password"));
  CHECK_FALSE(verify_password("", "anything"));
  CHECK_FALSE(verify_password("not a hash", "anything"));
  auto h2 = hash_password("correct horse battery", PasswordParams::minimum());
  CHECK(*h != *h2);  // salted
}

TEST_CASE("usernames", "[auth]") {
  CHECK(normalize_username("MixedCase_1") == "mixedcase_1");
  CHECK_FALSE(validate_username("player_1").has_value());
  CHECK(validate_username("ab").has_value());
  CHECK(validate_username("1player").has_value());
  CHECK(validate_username("bad name").has_value());
  CHECK(validate_username("waytoolongusername123").has_value());
}

TEST_CASE("passwords", "[auth]") {
  CHECK(validate_password("short").has_value());
  CHECK_FALSE(validate_password("longenough").has_value());
  CHECK(validate_password(std::string(65, 'x')).has_value());
}

TEST_CASE("character names", "[auth]") {
  CHECK(normalize_character_name("aLDRIC") == "Aldric");
  CHECK_FALSE(validate_character_name("Aldric").has_value());
  CHECK(validate_character_name("Al").has_value());
  CHECK(validate_character_name("Aldric2").has_value());
  CHECK(validate_character_name("Al Dric").has_value());
  CHECK(validate_character_name("Abcdefghijklmnop").has_value());
}

TEST_CASE("chat sanitization", "[auth]") {
  CHECK(sanitize_chat("  hello \t\n world  ") == "hello world");
  CHECK(sanitize_chat("bell\x07 char") == "bell char");
  CHECK(sanitize_chat("   ").empty());
  CHECK(sanitize_chat(std::string(500, 'a')).size() == kChatMax);
}
