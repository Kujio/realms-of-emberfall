// Faction standing (Phase 6): pure rules. A character's standing with a faction is the faction's
// default, plus a race adjustment, plus whatever their deeds have earned; it is clamped to
// [kFactionMin, kFactionMax]. The regard tiers decide what mobs and NPCs of the faction do: below
// Dubious, merchants and other services refuse you; at Threatening and below, its members attack on
// sight (a mob still needs an aggro radius to notice you).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ef::game {

inline constexpr int kFactionMin = -2000;
inline constexpr int kFactionMax = 2000;

enum class Regard : std::uint8_t { Ally = 0, Warmly, Kindly, Amiably, Indifferent, Apprehensive, Dubious, Threatening, Scowls };
inline constexpr std::uint8_t kNoRegard = 255;  // TargetInfo: not a faction member

Regard regard_of(int standing);
// "regards you as an ally", "glares at you threateningly", ...
const char* regard_text(Regard r);
bool attacks_on_sight(Regard r);  // Threatening and Scowls
bool refuses_service(Regard r);   // Dubious and worse
int clamp_standing(int standing);

// The standings a character carries: only factions whose standing has moved from the default.
class FactionStandings {
 public:
  // Earned standing (0 if untouched).
  int earned(std::string_view faction) const;
  // Adds `delta` to the earned part; returns the new earned value.
  int add(std::string_view faction, int delta);
  void set(std::string_view faction, int earned);
  const std::vector<std::pair<std::string, int>>& all() const { return earned_; }

 private:
  std::vector<std::pair<std::string, int>> earned_;
};

}  // namespace ef::game
