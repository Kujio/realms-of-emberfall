// Zone side of the social systems (Phase 7): who earns a kill and how a group shares it (experience,
// loot, quest and faction credit), guild tags, /assist, and the GM tools that act inside a zone.
#include <algorithm>
#include <map>

#include "game/zone.hpp"

namespace ef::game {

const Group* Zone::group_of(const PlayerEntity& p) const { return player_groups_ ? player_groups_->group_of(p.char_id) : nullptr; }

std::vector<PlayerEntity*> Zone::group_here(const PlayerEntity& p, const Vec3& where, float range, bool alive_only) {
  std::vector<PlayerEntity*> out;
  if (PlayerEntity* self = find(p.id)) out.push_back(self);
  const Group* g = group_of(p);
  if (!g) return out;
  for (const auto& m : g->members) {
    if (m.char_id == p.char_id) continue;
    PlayerEntity* o = find_by_char(m.char_id);
    if (!o || o->transferring || (alive_only && o->dead)) continue;
    if (horizontal_distance(o->pos, where) > range) continue;
    out.push_back(o);
  }
  return out;
}

PlayerEntity* Zone::kill_credit(const MobEntity& m, PlayerEntity* killer) {
  // Unit keys: a group by its id (negative, so it cannot collide with a character id), a solo player by
  // their character id.
  auto unit_of = [&](std::int64_t cid) -> std::int64_t {
    const Group* g = player_groups_ ? player_groups_->group_of(cid) : nullptr;
    return g ? -static_cast<std::int64_t>(g->id) : cid;
  };
  std::map<std::int64_t, int> damage;
  for (const auto& [cid, dmg] : m.damage_by) damage[unit_of(cid)] += dmg;
  // The unit's best living damager here (or, for a group whose damagers are gone, any living member here).
  auto lead_of = [&](std::int64_t unit) -> PlayerEntity* {
    PlayerEntity* best = nullptr;
    int best_dmg = -1;
    for (const auto& [cid, dmg] : m.damage_by) {
      if (unit_of(cid) != unit) continue;
      PlayerEntity* p = find_by_char(cid);
      if (p && !p->dead && dmg > best_dmg) {
        best = p;
        best_dmg = dmg;
      }
    }
    if (best || unit >= 0) return best;
    const Group* g = player_groups_ ? player_groups_->find(static_cast<std::uint32_t>(-unit)) : nullptr;
    if (!g) return nullptr;
    for (const auto& mem : g->members)
      if (PlayerEntity* p = find_by_char(mem.char_id); p && !p->dead && horizontal_distance(p->pos, m.pos) <= content_.social.group_xp_range)
        return p;
    return nullptr;
  };
  PlayerEntity* winner = nullptr;
  int best = 0;
  for (const auto& [unit, dmg] : damage) {
    if (dmg <= best) continue;
    if (PlayerEntity* p = lead_of(unit)) {
      winner = p;
      best = dmg;
    }
  }
  if (!winner && killer && !killer->dead) winner = killer;
  return winner;
}

std::vector<std::int64_t> Zone::loot_party(const MobEntity& m, std::int64_t top) {
  std::vector<std::int64_t> party;
  PlayerEntity* lead = top != 0 ? find_by_char(top) : nullptr;
  if (!lead) return party;
  const int cap = std::max(1, rules_.economy.loot_party_max);
  for (PlayerEntity* p : group_here(*lead, m.pos, content_.social.group_xp_range, true)) {
    if (static_cast<int>(party.size()) >= cap) break;
    if (p == lead || horizontal_distance(p->pos, m.pos) <= content_.social.group_xp_range) party.push_back(p->char_id);
  }
  return party;
}

void Zone::award_kill_xp(const MobEntity& m, PlayerEntity& winner, ZoneSink& sink) {
  // Every member of the winner's group in this zone takes part; those dead or out of range share nothing.
  std::vector<PlayerEntity*> members{&winner};
  if (const Group* g = group_of(winner))
    for (const auto& mem : g->members)
      if (mem.char_id != winner.char_id)
        if (PlayerEntity* o = find_by_char(mem.char_id); o && !o->transferring) members.push_back(o);
  const bool grouped = group_of(winner) != nullptr;
  std::vector<GroupXpMember> in;
  in.reserve(members.size());
  for (const PlayerEntity* p : members)
    // A solo winner always earns their kill (a long-range archer, a fleeing DoT caster); in a group every
    // member, the winner too, must be near it.
    in.push_back({p->level, !p->dead && ((p == &winner && !grouped) || horizontal_distance(p->pos, m.pos) <= content_.social.group_xp_range)});
  const auto shares = group_xp_shares(m.def->level, in, rules_.xp, rules_.con, content_.social.group_level_window,
                                      content_.social.group_level_fraction, toughness_xp_mult(static_cast<double>(m.def->hp_mult), rules_.xp));
  const bool any = std::any_of(shares.begin(), shares.end(), [](const GroupXpShare& s) { return s.xp > 0; });
  for (std::size_t i = 0; i < members.size(); ++i) {
    PlayerEntity& p = *members[i];
    if (shares[i].xp > 0) award_xp(p, shares[i].xp, sink);
    else if (shares[i].too_low) combat_text(p, "You are too far below your group to share in this experience.", sink);
    else if (!any && !shares[i].out_of_range) combat_text(p, "You gain no experience from such an easy foe.", sink);
  }
}

void Zone::set_guild_tag(EntityId player, const std::string& tag, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p || p->guild_tag == tag) return;
  p->guild_tag = tag;
  respawn_spot_watchers(p->id, sink);
}

bool Zone::assist(EntityId player, EntityId helped, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  const PlayerEntity* h = find(helped);
  if (!p) return false;
  if (!h || h->id == p->id) {
    combat_text(*p, "Assist whom? Target a player or name one: /assist NAME.", sink);
    return false;
  }
  if (horizontal_distance(p->pos, h->pos) > def_.interest_radius) {
    combat_text(*p, h->name + " is too far away to assist.", sink);
    return false;
  }
  if (h->target == 0 || h->target == p->id) {
    combat_text(*p, h->name + " has no target to assist with.", sink);
    return false;
  }
  set_target(p->id, h->target, sink);
  if (p->target != h->target) {
    combat_text(*p, "You cannot see " + h->name + "'s target.", sink);
    return false;
  }
  return true;
}

void Zone::gm_set_level(EntityId player, int level, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p || p->dead) return;
  p->xp = total_xp_for_level(std::clamp(level, 1, rules_.max_level), rules_.xp);
  recompute_level(*p, sink, true);
  p->changed_tick = tick_;
}

void Zone::gm_move(EntityId player, const Destination& to, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p || p->dead) return;
  Destination d = to;
  if (d.zone == def_.id) {
    d.pos.x = std::clamp(d.pos.x, def_.min_x, def_.max_x);
    d.pos.z = std::clamp(d.pos.z, def_.min_z, def_.max_z);
    d.pos.y = ground_at(d.pos.x, d.pos.z);
    d.pos = settle(d.pos, kPlayerRadius);
  }
  move_player_to(*p, d, sink, "gm");
}

bool Zone::player_in_combat(EntityId player) const {
  const PlayerEntity* p = find(player);
  return p && !p->dead && in_combat(*p);
}

void Zone::reset_interest(EntityId player) {
  if (PlayerEntity* p = find(player)) {
    p->known.clear();
    p->sent_tick.clear();
  }
}

void Zone::gm_kill_mob(EntityId mob, ZoneSink& sink) {
  if (MobEntity* m = find_mob(mob); m && m->alive()) kill_mob(*m, nullptr, sink);
}

}  // namespace ef::game
