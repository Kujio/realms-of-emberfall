// Zone corpses and looting. Mob corpses hold rolled loot for whoever earned it; player corpses
// hold the dead character's coin and pack and persist (through ZoneSink) for an hour.
#include <algorithm>

#include "game/zone.hpp"

namespace ef::game {

CorpseEntity& Zone::make_mob_corpse(const MobEntity& m, std::int64_t loot_char) {
  CorpseEntity c;
  c.id = new_id();
  c.name = m.def->name + "'s corpse";
  c.model = m.def->model;
  c.scale = m.def->scale;
  c.level = m.def->level;
  c.pos = m.pos;
  c.yaw = m.yaw;
  c.loot_char_id = loot_char;
  if (const LootTable* t = content_.find_loot(m.def->loot)) {
    c.coin = static_cast<std::uint64_t>(rng_.range(t->coin_min, t->coin_max));
    for (const auto& d : t->drops) {
      if (static_cast<int>(c.items.size()) >= t->max_drops) break;
      if (rng_.chance(d.chance)) c.items.push_back({d.item, rng_.range(d.min, d.max)});
    }
  }
  const float life = c.empty() ? rules_.death.empty_mob_corpse_seconds : rules_.death.mob_corpse_seconds;
  c.expires_tick = tick_ + ticks(life);
  const EntityId id = c.id;
  grid_.insert(id, c.pos.x, c.pos.z);
  return corpses_.emplace(id, std::move(c)).first->second;
}

EntityId Zone::add_corpse(const CorpseInit& init) {
  const Bounds b = bounds();
  CorpseEntity c;
  c.id = new_id();
  c.db_id = init.db_id;
  c.player_corpse = true;
  c.owner_char_id = init.owner_char_id;
  c.loot_char_id = init.owner_char_id;
  c.owner_name = init.owner_name;
  c.name = init.owner_name + "'s corpse";
  c.model = "humanoid";
  c.level = init.level;
  c.pos = {std::clamp(init.pos.x, b.min_x, b.max_x), def_.ground_y, std::clamp(init.pos.z, b.min_z, b.max_z)};
  c.yaw = init.yaw;
  c.coin = init.coin;
  for (const auto& s : init.items)
    if (!s.empty() && content_.find_item(s.item_id)) c.items.push_back(s);
  c.xp_lost = init.xp_lost;
  c.expires_tick = tick_ + std::max<std::uint32_t>(1, ticks(init.seconds_left));
  const EntityId id = c.id;
  grid_.insert(id, c.pos.x, c.pos.z);
  corpses_.emplace(id, std::move(c));
  return id;
}

bool Zone::give_item(PlayerEntity& p, ItemStackState& stack) {
  const ItemDef* def = content_.find_item(stack.item_id);
  const int max_stack = def ? std::max(1, def->stack) : 1;
  for (auto& s : p.inventory) {  // top up existing stacks first
    if (stack.count <= 0) break;
    if (s.empty() || s.item_id != stack.item_id || s.count >= max_stack) continue;
    const int move = std::min(stack.count, max_stack - s.count);
    s.count += move;
    stack.count -= move;
  }
  for (auto& s : p.inventory) {
    if (stack.count <= 0) break;
    if (!s.empty()) continue;
    const int move = std::min(stack.count, max_stack);
    s = {stack.item_id, move};
    stack.count -= move;
  }
  return stack.count <= 0;
}

void Zone::send_loot_window(PlayerEntity& p, const CorpseEntity& c, ZoneSink& sink) {
  proto::LootWindow w;
  w.corpse_id = c.id;
  w.name = c.name;
  w.coin = c.coin;
  for (std::size_t i = 0; i < c.items.size() && w.items.size() < 32; ++i) {
    const auto& s = c.items[i];
    if (s.empty()) continue;
    const ItemDef* def = content_.find_item(s.item_id);
    w.items.push_back({static_cast<std::uint8_t>(i), s.item_id, def ? def->name : s.item_id,
                       static_cast<std::uint16_t>(std::clamp(s.count, 0, 65535))});
  }
  send(sink, p, w);
}

void Zone::loot_open(EntityId player, EntityId corpse_id, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  CorpseEntity* c = find_corpse(corpse_id);
  auto refuse = [&](const char* why) { send(sink, p, proto::LootClosed{corpse_id, why}); };
  if (!c) return refuse("There is no corpse there to loot.");
  if (p.dead) return refuse("You can't loot while you are dead.");
  if (horizontal_distance(p.pos, c->pos) > rules_.combat.loot_range) return refuse("You are too far away to loot that corpse.");
  if (c->loot_char_id != 0 && c->loot_char_id != p.char_id)
    return refuse(c->player_corpse ? "You may not loot someone else's corpse." : "You did not earn the right to loot that corpse.");
  if (c->opened_by != 0 && c->opened_by != p.id) return refuse("Someone is already looting that corpse.");
  if (p.looting != 0 && p.looting != c->id) close_loot(p, sink, "");
  c->opened_by = p.id;
  p.looting = c->id;
  if (c->coin > 0) {
    p.coin += c->coin;
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You receive " + format_coin(c->coin) + " from the corpse."});
    c->coin = 0;
    send_inventory(p, sink);
    sink.zone_corpse_looted(*c, p);
  }
  send_loot_window(p, *c, sink);
}

void Zone::loot_take(EntityId player, EntityId corpse_id, std::uint8_t slot, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  CorpseEntity* c = find_corpse(corpse_id);
  if (!c || p.looting != corpse_id) {
    send(sink, p, proto::LootClosed{corpse_id, "You are not looting that corpse."});
    return;
  }
  bool moved = false;
  bool full = false;
  for (std::size_t i = 0; i < c->items.size(); ++i) {
    if (slot != kLootAll && i != slot) continue;
    ItemStackState& s = c->items[i];
    if (s.empty()) continue;
    const int before = s.count;
    const std::string id = s.item_id;
    const bool all = give_item(p, s);
    if (s.count != before) {
      moved = true;
      const ItemDef* def = content_.find_item(id);
      const int got = before - s.count;
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You have looted " + (got > 1 ? std::to_string(got) + " x " : std::string()) +
                                                            (def ? def->name : id) + "."});
    }
    if (!all) {
      full = true;
      break;
    }
  }
  if (full) send(sink, p, proto::ChatMessage{kChatSystem, "", "Your pack is full."});
  if (moved) {
    // Drop emptied stacks so slot numbers stay meaningful only for what is left.
    send_inventory(p, sink);
    sink.zone_corpse_looted(*c, p);
  }
  if (c->empty() && !c->player_corpse) close_loot(p, sink, "");
  else send_loot_window(p, *c, sink);
}

void Zone::loot_close(EntityId player, EntityId corpse_id, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (p && p->looting == corpse_id && corpse_id != 0) close_loot(*p, sink, "");
}

void Zone::close_loot(PlayerEntity& p, ZoneSink& sink, const std::string& message) {
  const EntityId cid = p.looting;
  if (cid == 0) return;
  p.looting = 0;
  send(sink, p, proto::LootClosed{cid, message});
  CorpseEntity* c = find_corpse(cid);
  if (!c) return;
  c->opened_by = 0;
  // An emptied mob corpse vanishes; player corpses stay until they decay (a resurrection
  // target from Phase 3 on).
  if (c->empty() && !c->player_corpse) remove_corpse(cid, sink);
}

void Zone::remove_corpse(EntityId id, ZoneSink& sink) {
  auto it = corpses_.find(id);
  if (it == corpses_.end()) return;
  if (it->second.opened_by != 0)
    if (PlayerEntity* p = find(it->second.opened_by)) {
      p->looting = 0;
      send(sink, *p, proto::LootClosed{id, "The corpse has decayed."});
    }
  for (auto& [pid, p] : players_)
    if (p.target == id) {
      p.target = 0;
      send_target_info(p, sink);
    }
  sink.zone_corpse_removed(it->second);
  grid_.remove(id);
  corpses_.erase(it);
}

void Zone::expire_corpses(ZoneSink& sink) {
  scratch_dead_.clear();
  for (const auto& [id, c] : corpses_)
    if (c.expires_tick <= tick_) scratch_dead_.push_back(id);
  for (EntityId id : scratch_dead_) remove_corpse(id, sink);
}

}  // namespace ef::game
