#include "game/factions.hpp"

#include <algorithm>

namespace ef::game {

Regard regard_of(int standing) {
  if (standing >= 1100) return Regard::Ally;
  if (standing >= 750) return Regard::Warmly;
  if (standing >= 500) return Regard::Kindly;
  if (standing >= 100) return Regard::Amiably;
  if (standing > -100) return Regard::Indifferent;
  if (standing > -500) return Regard::Apprehensive;
  if (standing > -750) return Regard::Dubious;
  if (standing > -1000) return Regard::Threatening;
  return Regard::Scowls;
}

const char* regard_text(Regard r) {
  switch (r) {
    case Regard::Ally: return "regards you as an ally";
    case Regard::Warmly: return "looks upon you warmly";
    case Regard::Kindly: return "kindly considers you";
    case Regard::Amiably: return "judges you amiably";
    case Regard::Indifferent: return "regards you indifferently";
    case Regard::Apprehensive: return "looks your way apprehensively";
    case Regard::Dubious: return "glowers at you dubiously";
    case Regard::Threatening: return "glares at you threateningly";
    case Regard::Scowls: return "scowls at you, ready to attack";
  }
  return "regards you indifferently";
}

bool attacks_on_sight(Regard r) { return r == Regard::Threatening || r == Regard::Scowls; }
bool refuses_service(Regard r) { return r == Regard::Dubious || r == Regard::Threatening || r == Regard::Scowls; }
int clamp_standing(int standing) { return std::clamp(standing, kFactionMin, kFactionMax); }

int FactionStandings::earned(std::string_view faction) const {
  for (const auto& [id, v] : earned_)
    if (id == faction) return v;
  return 0;
}

int FactionStandings::add(std::string_view faction, int delta) {
  for (auto& [id, v] : earned_)
    if (id == faction) {
      v = std::clamp(v + delta, 2 * kFactionMin, 2 * kFactionMax);
      return v;
    }
  earned_.emplace_back(std::string(faction), std::clamp(delta, 2 * kFactionMin, 2 * kFactionMax));
  return earned_.back().second;
}

void FactionStandings::set(std::string_view faction, int earned) {
  for (auto& [id, v] : earned_)
    if (id == faction) {
      v = earned;
      return;
    }
  earned_.emplace_back(std::string(faction), earned);
}

}  // namespace ef::game
