// Pure validation rules for account names, passwords, character names, and chat text.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ef::auth {

inline constexpr std::size_t kUsernameMin = 3;
inline constexpr std::size_t kUsernameMax = 16;
inline constexpr std::size_t kPasswordMin = 8;
inline constexpr std::size_t kPasswordMax = 64;
inline constexpr std::size_t kCharNameMin = 4;
inline constexpr std::size_t kCharNameMax = 15;
inline constexpr std::size_t kChatMax = 200;

// Lowercases ASCII letters. Usernames are case-insensitive.
std::string normalize_username(std::string_view name);

// Each validator returns a player-facing error message, or nullopt if valid.
std::optional<std::string> validate_username(std::string_view normalized);
std::optional<std::string> validate_password(std::string_view password);

// "aLDRIC" -> "Aldric". Only meaningful for ASCII letter names.
std::string normalize_character_name(std::string_view name);
std::optional<std::string> validate_character_name(std::string_view normalized);

// Strips control characters, collapses whitespace runs, trims, and caps length (UTF-8 safe).
std::string sanitize_chat(std::string_view text);

}  // namespace ef::auth
