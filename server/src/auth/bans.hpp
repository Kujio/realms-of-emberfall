// Account bans and mutes (Phase 7), pure: whether one is in force and what the player is told.
// `until` is unix seconds; kBanForever (-1) never ends; 0 means none.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ef::auth {

inline constexpr std::int64_t kBanForever = -1;

bool restriction_active(std::int64_t until, std::int64_t now);
// The login refusal for a banned account, or nullopt when it may log in.
std::optional<std::string> ban_refusal(std::int64_t banned_until, const std::string& reason, std::int64_t now);
// "You are muted for another 25 minutes." (or nullopt when not muted).
std::optional<std::string> mute_notice(std::int64_t muted_until, std::int64_t now);
// The stored `until` for a restriction of `seconds` from `now` (kBanForever stays forever).
std::int64_t restriction_until(std::int64_t seconds, std::int64_t now);

}  // namespace ef::auth
