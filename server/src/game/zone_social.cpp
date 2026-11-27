// Zone side of the social systems (Phase 7): who earns a kill and how a group shares it (experience,
// loot, quest and faction credit), guild tags, /assist, and the GM tools that act inside a zone.
// Phase 14: a raid (whole groups joined together) shares a kill as one party.
#include <algorithm>
#include <cmath>
#include <map>

#include "game/zone.hpp"

namespace ef::game {

const Group* Zone::group_of(const PlayerEntity& p) const { return player_groups_ ? player_groups_->group_of(p.char_id) : nullptr; }

const Raid* Zone::raid_of(const PlayerEntity& p) const {
  return player_groups_ && player_raids_ ? player_raids_->raid_of(*player_groups_, p.char_id) : nullptr;
}

// Phase 17: zone lines keep everyone who is not in a raid out of a raid dungeon, but a player can still be
// inside without one: they logged in there, their raid disbanded or dropped their group, or a game master
// sent them in. They get raid evict_seconds to join a raid (or leave), then are moved out. Game masters stay.
void Zone::raid_entry_step(PlayerEntity& p, ZoneSink& sink) {
  if (!def_.raid || !player_raids_ || p.gm || p.dead || p.transferring) {  // (no raid registry: a bare test zone)
    p.raid_evict_tick = 0;
    return;
  }
  if (raid_of(p)) {
    if (p.raid_evict_tick != 0) send(sink, p, proto::ChatMessage{kChatSystem, "", "You are in a raid now: you may stay in " + def_.name + "."});
    p.raid_evict_tick = 0;
    return;
  }
  const float grace = content_.social.raid_evict_seconds;
  if (p.raid_evict_tick == 0) {
    p.raid_evict_tick = tick_ + std::max<std::uint32_t>(1, ticks(grace));
    send(sink, p, proto::ChatMessage{kChatSystem, "", def_.name + " is a raid dungeon and you are not in a raid: join one within " +
                                                         std::to_string(static_cast<int>(std::lround(grace))) + " seconds or you will be moved out."});
    return;
  }
  if (tick_ < p.raid_evict_tick) return;
  p.raid_evict_tick = 0;
  Destination out;
  if (!raid_exit(p, out)) return;  // nowhere to go (content validation rules this out)
  send(sink, p, proto::ChatMessage{kChatSystem, "", "You are not in a raid, and are moved out of " + def_.name + "."});
  move_player_to(p, out, sink, "raid exit");
}

bool Zone::raid_exit(const PlayerEntity& p, Destination& out) {
  for (const auto& l : def_.zone_lines) {
    const ZoneDef* to = content_.find_zone(l.to);
    const ZoneEntryDef* e = to && !to->raid ? to->find_entry(l.entry) : nullptr;
    if (!e) continue;
    out = {to->id, {e->at.x, to->ground_at(e->at.x, e->at.z), e->at.z}, e->at.yaw};
    return true;
  }
  return resolve_destination(p, "bind", out) && out.zone != def_.id;
}

std::vector<PlayerEntity*> Zone::allies_here(const PlayerEntity& p, const Vec3& where, float range, bool alive_only) {
  const Raid* r = raid_of(p);
  if (!r) return group_here(p, where, range, alive_only);
  std::vector<PlayerEntity*> out;
  if (PlayerEntity* self = find(p.id)) out.push_back(self);
  for (const std::int64_t cid : RaidRegistry::members(*r, *player_groups_)) {
    if (cid == p.char_id) continue;
    PlayerEntity* o = find_by_char(cid);
    if (!o || o->transferring || (alive_only && o->dead)) continue;
    if (horizontal_distance(o->pos, where) > range) continue;
    out.push_back(o);
  }
  return out;
}

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
  // Unit keys: a raid by its id (far below every group key), a group by its id (negative, so it cannot
  // collide with a character id), a solo player by their character id.
  constexpr std::int64_t kRaidKey = -(std::int64_t{1} << 40);
  auto unit_of = [&](std::int64_t cid) -> std::int64_t {
    const Group* g = player_groups_ ? player_groups_->group_of(cid) : nullptr;
    if (g && player_raids_)
      if (const Raid* r = player_raids_->raid_of_group(g->id)) return kRaidKey - static_cast<std::int64_t>(r->id);
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
    // A group or raid whose damagers are gone: any living member near the kill.
    std::vector<std::int64_t> members;
    if (unit <= kRaidKey) {
      const Raid* r = player_raids_ ? player_raids_->find(static_cast<std::uint32_t>(kRaidKey - unit)) : nullptr;
      if (r) members = RaidRegistry::members(*r, *player_groups_);
    } else if (const Group* g = player_groups_ ? player_groups_->find(static_cast<std::uint32_t>(-unit)) : nullptr) {
      for (const auto& mem : g->members) members.push_back(mem.char_id);
    }
    for (const std::int64_t cid : members)
      if (PlayerEntity* p = find_by_char(cid); p && !p->dead && horizontal_distance(p->pos, m.pos) <= content_.social.group_xp_range)
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
  // A raid's party is everyone in it at the kill (its winner too only when near it); a group's is capped
  // at loot_party_max and always includes the winner.
  const bool raid = raid_of(*lead) != nullptr;
  const int cap = raid ? content_.social.raid_max_groups * content_.social.group_max : std::max(1, rules_.economy.loot_party_max);
  for (PlayerEntity* p : allies_here(*lead, m.pos, content_.social.group_xp_range, true)) {
    if (static_cast<int>(party.size()) >= cap) break;
    if ((p == lead && !raid) || horizontal_distance(p->pos, m.pos) <= content_.social.group_xp_range) party.push_back(p->char_id);
  }
  return party;
}

void Zone::award_kill_xp(const MobEntity& m, PlayerEntity& winner, ZoneSink& sink) {
  // Every member of the winner's group (or raid) in this zone takes part; those dead or out of range
  // share nothing. A raid's bonus counts no more members than a full group's, so a raid kill pays out no
  // more in all than a group kill would.
  std::vector<PlayerEntity*> members{&winner};
  const Raid* raid = raid_of(winner);
  std::vector<std::int64_t> others;
  if (raid) others = RaidRegistry::members(*raid, *player_groups_);
  else if (const Group* g = group_of(winner))
    for (const auto& mem : g->members) others.push_back(mem.char_id);
  for (const std::int64_t cid : others)
    if (cid != winner.char_id)
      if (PlayerEntity* o = find_by_char(cid); o && !o->transferring) members.push_back(o);
  const bool grouped = group_of(winner) != nullptr;
  std::vector<GroupXpMember> in;
  in.reserve(members.size());
  for (const PlayerEntity* p : members)
    // A solo winner always earns their kill (a long-range archer, a fleeing DoT caster); in a group every
    // member, the winner too, must be near it.
    in.push_back({p->level, !p->dead && ((p == &winner && !grouped) || horizontal_distance(p->pos, m.pos) <= content_.social.group_xp_range)});
  const auto shares = group_xp_shares(m.def->level, in, rules_.xp, rules_.con, content_.social.group_level_window,
                                      content_.social.group_level_fraction, toughness_xp_mult(static_cast<double>(m.def->hp_mult), rules_.xp),
                                      raid ? static_cast<std::size_t>(content_.social.group_max) : 0);
  const bool any = std::any_of(shares.begin(), shares.end(), [](const GroupXpShare& s) { return s.xp > 0; });
  for (std::size_t i = 0; i < members.size(); ++i) {
    PlayerEntity& p = *members[i];
    if (shares[i].xp > 0) award_xp(p, shares[i].xp, sink);
    else if (shares[i].too_low)
      combat_text(p, raid ? "You are too far below your raid to share in this experience." : "You are too far below your group to share in this experience.", sink);
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

void Zone::reset_input_seq(EntityId player, bool has_floor, std::uint32_t floor) {
  PlayerEntity* p = find(player);
  if (!p) return;
  p->queue_head = 0;
  p->queue_size = 0;
  p->has_seq = has_floor;
  p->last_queued_seq = floor;
  p->has_pred = false;
}

bool Zone::player_in_combat(EntityId player) const {
  const PlayerEntity* p = find(player);
  return p && !p->dead && in_combat(*p);
}

void Zone::reset_interest(EntityId player) {
  if (PlayerEntity* p = find(player)) {
    p->known.clear();
    p->known_ref.clear();
  }
}

void Zone::gm_kill_mob(EntityId mob, ZoneSink& sink) {
  if (MobEntity* m = find_mob(mob); m && m->alive()) kill_mob(*m, nullptr, sink);
}

}  // namespace ef::game
