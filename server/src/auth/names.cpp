#include "auth/names.hpp"

#include "net/byte_buffer.hpp"

namespace ef::auth {

namespace {
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
char to_lower(char c) { return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }
char to_upper(char c) { return is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c; }
}  // namespace

std::string normalize_username(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) out.push_back(to_lower(c));
  return out;
}

std::optional<std::string> validate_username(std::string_view n) {
  if (n.size() < kUsernameMin || n.size() > kUsernameMax) return "Username must be 3-16 characters.";
  for (char c : n) {
    if (!is_lower(c) && !is_digit(c) && c != '_') return "Username may only use letters, digits, and underscores.";
  }
  if (!is_lower(n.front())) return "Username must start with a letter.";
  return std::nullopt;
}

std::optional<std::string> validate_password(std::string_view p) {
  if (p.size() < kPasswordMin || p.size() > kPasswordMax) return "Password must be 8-64 characters.";
  return std::nullopt;
}

std::string normalize_character_name(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (std::size_t i = 0; i < name.size(); ++i) out.push_back(i == 0 ? to_upper(name[i]) : to_lower(name[i]));
  return out;
}

std::optional<std::string> validate_character_name(std::string_view n) {
  if (n.size() < kCharNameMin || n.size() > kCharNameMax) return "Name must be 4-15 letters.";
  for (std::size_t i = 0; i < n.size(); ++i) {
    const char c = n[i];
    if (i == 0 ? !is_upper(c) : !is_lower(c)) return "Name may only contain letters (A-Z).";
  }
  return std::nullopt;
}

std::string sanitize_chat(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pending_space = false;
  for (char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pending_space = !out.empty();
      continue;
    }
    if (c < 0x20 || c == 0x7F) continue;
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    out.push_back(ch);
  }
  out.resize(net::utf8_truncate_len(out, kChatMax));
  if (!net::is_valid_utf8(out)) return {};
  return out;
}

}  // namespace ef::auth
