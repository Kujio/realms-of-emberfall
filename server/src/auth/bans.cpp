#include "auth/bans.hpp"

#include "game/commands.hpp"

namespace ef::auth {

bool restriction_active(std::int64_t until, std::int64_t now) { return until == kBanForever || until > now; }

std::optional<std::string> ban_refusal(std::int64_t banned_until, const std::string& reason, std::int64_t now) {
  if (!restriction_active(banned_until, now)) return std::nullopt;
  std::string msg = banned_until == kBanForever ? "This account is banned."
                                                : "This account is banned for another " + game::describe_duration(banned_until - now) + ".";
  if (!reason.empty()) msg += " Reason: " + reason;
  return msg;
}

std::optional<std::string> mute_notice(std::int64_t muted_until, std::int64_t now) {
  if (!restriction_active(muted_until, now)) return std::nullopt;
  if (muted_until == kBanForever) return "You have been muted by a game master.";
  return "You have been muted by a game master for another " + game::describe_duration(muted_until - now) + ".";
}

std::int64_t restriction_until(std::int64_t seconds, std::int64_t now) {
  if (seconds == kBanForever) return kBanForever;
  return now + (seconds > 0 ? seconds : 0);
}

}  // namespace ef::auth
