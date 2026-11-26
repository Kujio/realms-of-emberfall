// Raid lockouts (Phase 15), pure and unit-tested. A raider who shares the kill of a raid dungeon's boss
// is locked out of that boss's treasure for a while (social.raid.lockout_hours): at the next kill they
// still fight and earn quest and faction credit, but get none of its coin or items. A lockout runs on
// real time (unix seconds), so it lapses while the character is offline, and it is saved with the
// character as JSON ({"boss mob id": expires_at}).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ef::game {

class Lockouts {
 public:
  // When the lockout from `boss` ends (unix seconds), or 0 if there is none still running at `now`.
  std::int64_t until(std::string_view boss, std::int64_t now) const;
  bool locked(std::string_view boss, std::int64_t now) const { return until(boss, now) != 0; }
  // Locks the character out of `boss` until `until`; refused (false) while a lockout from it still
  // runs, which is never extended. Lapsed lockouts are dropped first.
  bool lock(std::string_view boss, std::int64_t until, std::int64_t now);
  // Drops every lockout that has ended by `now`.
  void prune(std::int64_t now);
  // The running lockouts, soonest to end first.
  std::vector<std::pair<std::string, std::int64_t>> active(std::int64_t now) const;
  bool empty() const { return entries_.empty(); }

  std::string to_json() const;
  // Unknown or broken text gives no lockouts; ids are capped at 64 characters and the list at 64 entries.
  static Lockouts from_json(std::string_view text);

 private:
  std::vector<std::pair<std::string, std::int64_t>> entries_;  // boss mob id -> expires_at
};

// "2h 5m", "45m", "1m" (at least a minute), "3d 4h" for a span in seconds.
std::string format_span(std::int64_t seconds);

}  // namespace ef::game
