// Factions in the zone (Phase 6): standing (default + race + earned, an illusion's race counting),
// kill consequences, refusals by service NPCs, and the standings list the client shows.
#include <algorithm>

#include "game/zone.hpp"

namespace ef::game {

int Zone::standing(const PlayerEntity& p, std::string_view faction) const {
  const FactionDef* f = content_.find_faction(faction);
  if (!f) return 0;
  // An illusion of another race fools a faction's eyes (classic: walk into a city in disguise).
  std::string_view race = p.race;
  if (const std::string* ill = illusion_of(p); ill && content_.find_race(*ill)) race = *ill;
  return f->standing_for(race, p.factions.earned(faction));
}

Regard Zone::regard(const PlayerEntity& p, std::string_view faction) const { return regard_of(standing(p, faction)); }

void Zone::adjust_faction(EntityId player, std::string_view faction, int delta, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  const FactionDef* f = content_.find_faction(faction);
  if (!p || !f || delta == 0) return;
  const int before = standing(*p, faction);
  p->factions.add(faction, delta);
  const int after = standing(*p, faction);
  p->save_dirty = true;
  p->factions_dirty = true;
  std::string text;
  if (after == before) text = "Your standing with " + f->name + " could not possibly get any " + (delta > 0 ? "better." : "worse.");
  else text = "Your standing with " + f->name + (after > before ? " got better." : " got worse.");
  send(sink, *p, proto::ChatMessage{kChatSystem, "", text});
}

void Zone::faction_hits(PlayerEntity& p, const FactionDef& f, ZoneSink& sink) {
  for (const auto& [other, delta] : f.hits) adjust_faction(p.id, other, delta, sink);
}

bool Zone::npc_refuses(PlayerEntity& p, const NpcDef& n, ZoneSink& sink) {
  if (n.faction.empty() || !content_.find_faction(n.faction)) return false;
  const Regard r = regard(p, n.faction);
  if (!refuses_service(r)) return false;
  send(sink, p, proto::ChatMessage{kChatSay, n.name, attacks_on_sight(r) ? "Guards! Seize this villain!" : "I will not deal with the likes of you."});
  return true;
}

void Zone::send_factions(PlayerEntity& p, ZoneSink& sink) {
  p.factions_dirty = false;
  proto::FactionStandings msg;
  auto add = [&](const FactionDef& f) {
    if (msg.factions.size() >= 64) return;
    for (const auto& have : msg.factions)
      if (have.id == f.id) return;
    const int s = standing(p, f.id);
    msg.factions.push_back({f.id, f.name.substr(0, 48), s, static_cast<std::uint8_t>(regard_of(s))});
  };
  // Every faction the player has moved, and every one their race starts out liked or disliked by.
  for (const auto& [id, earned] : p.factions.all())
    if (const FactionDef* f = content_.find_faction(id)) add(*f);
  std::string_view race = p.race;
  if (const std::string* ill = illusion_of(p); ill && content_.find_race(*ill)) race = *ill;
  for (const auto& f : content_.factions)
    for (const auto& [r, v] : f.race_mods)
      if (r == race && v != 0) add(f);
  send(sink, p, msg);
}

}  // namespace ef::game
