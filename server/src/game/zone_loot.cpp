// Zone corpses and looting. Mob corpses hold rolled loot for the players who earned it (the loot party:
// the winning group's members nearby, zone_social.cpp); with more than one, coin is split at
// once and items are shared by round robin or need-before-greed rolls. Player corpses hold the dead
// character's coin, worn gear and pack, and persist (through ZoneSink) for an hour.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

}  // namespace

bool CorpseEntity::shared_with(std::int64_t char_id) const { return std::find(party.begin(), party.end(), char_id) != party.end(); }

CorpseEntity& Zone::make_mob_corpse(const MobEntity& m, std::int64_t loot_char, bool with_loot) {
  CorpseEntity c;
  c.id = new_id();
  c.name = m.def->name + "'s corpse";
  c.model = m.def->model;
  c.scale = m.def->scale;
  c.level = m.def->level;
  c.pos = m.pos;
  c.yaw = m.yaw;
  c.loot_char_id = loot_char;
  if (const LootTable* t = with_loot ? content_.find_loot(m.def->loot) : nullptr) {
    c.coin = static_cast<std::uint64_t>(rng_.range(t->coin_min, t->coin_max));
    for (auto& s : roll_loot(*t, rng_)) c.items.push_back({std::move(s), kNoSlot, 0, 0});
  }
  const float life = c.empty() ? rules_.death.empty_mob_corpse_seconds : rules_.death.mob_corpse_seconds;
  c.expires_tick = tick_ + ticks(life);
  const EntityId id = c.id;
  grid_.insert(id, c.pos.x, c.pos.z);
  return corpses_.emplace(id, std::move(c)).first->second;
}

void Zone::start_rolls(CorpseEntity& c, ZoneSink& sink) {
  if (c.party.size() <= 1) return;
  // Coin: split among the party at once.
  if (c.coin > 0) {
    std::vector<PlayerEntity*> here;
    for (std::int64_t cid : c.party)
      if (PlayerEntity* p = find_by_char(cid)) here.push_back(p);
    const auto shares = split_coin(c.coin, here.size());
    for (std::size_t i = 0; i < here.size(); ++i) {
      if (shares[i] == 0) continue;
      here[i]->coin += shares[i];
      send(sink, *here[i], proto::ChatMessage{kChatSystem, "", "You receive " + format_coin(shares[i]) + " as your share of the coin."});
      mark_inventory(*here[i]);
    }
    if (!here.empty()) c.coin = 0;
  }
  // A group's leader picks its rule; a loot party without a group follows its lead's own setting.
  const PlayerEntity* lead = find_by_char(c.party.front());
  LootRule rule = lead ? lead->loot_rule : LootRule::NeedGreed;
  if (const Group* g = lead ? group_of(*lead) : nullptr) rule = g->loot_rule;
  const auto owners = round_robin(c.items.size(), c.party, static_cast<std::size_t>(rng_.range(0, static_cast<int>(c.party.size()) - 1)));
  c.free_tick = tick_ + ticks(rules_.economy.round_robin_free_seconds);
  for (std::size_t i = 0; i < c.items.size(); ++i) {
    CorpseItem& it = c.items[i];
    const ItemDef* d = content_.find_item(it.stack.item_id);
    if (rule == LootRule::NeedGreed && d && worth_rolling(*d)) {
      LootRollState r;
      r.id = next_roll_++;
      if (next_roll_ == 0) next_roll_ = 1;
      r.corpse = c.id;
      r.index = i;
      r.deadline = tick_ + ticks(rules_.economy.loot_roll_seconds);
      for (std::int64_t cid : c.party) {
        PlayerEntity* p = find_by_char(cid);
        if (!p) continue;
        r.entries.push_back({cid, RollChoice::Pass, 0});
        r.answered.push_back(false);
        const bool need = p->race_def && p->class_def && usable_by(*d, *p->race_def, *p->class_def);
        send(sink, *p, proto::LootRoll{r.id, c.id, d->id, static_cast<std::uint16_t>(std::clamp(it.stack.count, 0, 65535)),
                                       ms_of(ticks(rules_.economy.loot_roll_seconds), dt_), need});
      }
      it.roll = r.id;
      rolls_.emplace(r.id, std::move(r));
    } else {
      it.owner = owners[i];
    }
  }
}

void Zone::loot_roll_choice(EntityId player, std::uint32_t roll_id, RollChoice choice, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  auto it = rolls_.find(roll_id);
  if (!p || it == rolls_.end()) return;
  LootRollState& r = it->second;
  for (std::size_t i = 0; i < r.entries.size(); ++i) {
    if (r.entries[i].char_id != p->char_id || r.answered[i]) continue;
    if (choice == RollChoice::Need) {
      const CorpseEntity* c = find_corpse(r.corpse);
      const ItemDef* d = c && r.index < c->items.size() ? content_.find_item(c->items[r.index].stack.item_id) : nullptr;
      if (d && !(p->race_def && p->class_def && usable_by(*d, *p->race_def, *p->class_def))) {
        send(sink, *p, proto::ChatMessage{kChatSystem, "", "You cannot use " + d->name + ", so you roll greed."});
        choice = RollChoice::Greed;
      }
    }
    r.entries[i].choice = choice;
    r.answered[i] = true;
  }
  if (std::all_of(r.answered.begin(), r.answered.end(), [](bool b) { return b; })) finish_roll(roll_id, sink);
}

void Zone::finish_roll(std::uint32_t roll_id, ZoneSink& sink) {
  auto rit = rolls_.find(roll_id);
  if (rit == rolls_.end()) return;
  LootRollState r = std::move(rit->second);
  rolls_.erase(rit);
  CorpseEntity* c = find_corpse(r.corpse);
  if (!c || r.index >= c->items.size() || c->items[r.index].roll != roll_id) return;
  CorpseItem& it = c->items[r.index];
  it.roll = 0;
  const std::string name = item_name(it.stack.item_id);
  const int winner = resolve_rolls(r.entries, rng_);
  std::string text;
  if (winner < 0) {
    it.owner = 0;  // everyone passed: anyone in the party may take it
    text = "Everyone passed on " + name + ".";
  } else {
    const RollEntry& w = r.entries[static_cast<std::size_t>(winner)];
    it.owner = w.char_id;
    it.won = true;
    const PlayerEntity* wp = find_by_char(w.char_id);
    text = (wp ? wp->name : std::string("Someone")) + " wins " + name + " (" + (w.choice == RollChoice::Need ? "need" : "greed") + " " +
           std::to_string(w.roll) + ").";
  }
  for (const auto& e : r.entries)
    if (PlayerEntity* p = find_by_char(e.char_id)) send(sink, *p, proto::ChatMessage{kChatSystem, "", text});
  // The winner gets it straight away when there is room (and they are free to receive it).
  if (it.owner != 0)
    if (PlayerEntity* wp = find_by_char(it.owner); wp && !wp->dead && wp->trade == 0) {
      const ItemDef* d = content_.find_item(it.stack.item_id);
      if (!(d && lore_conflict(wp->inv, *d)) && can_give(wp->inv, content_, it.stack)) {
        give_item(*wp, it.stack);
        send(sink, *wp, proto::ChatMessage{kChatSystem, "", "You receive " + name + "."});
        it.stack = {};
        sink.zone_items_awarded(*wp);
      }
    }
  if (c->opened_by != 0)
    if (PlayerEntity* looter = find(c->opened_by)) send_loot_window(*looter, *c, sink);
}

void Zone::tick_rolls(ZoneSink& sink) {
  std::vector<std::uint32_t> due;
  for (const auto& [id, r] : rolls_)
    if (tick_ >= r.deadline || !find_corpse(r.corpse)) due.push_back(id);
  for (std::uint32_t id : due) finish_roll(id, sink);
}

void Zone::set_loot_rule(EntityId player, LootRule rule, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p) return;
  p->loot_rule = rule;
  send(sink, *p, proto::ChatMessage{kChatSystem, "", rule == LootRule::RoundRobin ? "Loot you lead will be shared round robin."
                                                                                    : "Loot you lead will be rolled need before greed."});
}

bool Zone::may_take(const PlayerEntity& p, const CorpseEntity& c, const CorpseItem& it) const {
  if (it.empty()) return false;
  if (c.player_corpse) return c.owner_char_id == p.char_id;
  if (it.roll != 0) return false;
  if (it.owner == 0 || it.owner == p.char_id) return true;
  // Round-robin reservations lapse after a while; an item won in a roll stays its winner's.
  return !it.won && tick_ >= c.free_tick && c.shared_with(p.char_id);
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
  c.pos = {std::clamp(init.pos.x, b.min_x, b.max_x), 0.0f, std::clamp(init.pos.z, b.min_z, b.max_z)};
  c.pos.y = def_.ground_at(c.pos.x, c.pos.z);
  c.yaw = init.yaw;
  c.coin = init.coin;
  for (const auto& s : init.items)
    if (!s.empty() && content_.find_item(s.stack.item_id)) c.items.push_back({s.stack, s.origin, 0, 0});
  c.xp_lost = init.xp_lost;
  c.expires_tick = tick_ + std::max<std::uint32_t>(1, ticks(init.seconds_left));
  const EntityId id = c.id;
  grid_.insert(id, c.pos.x, c.pos.z);
  corpses_.emplace(id, std::move(c));
  return id;
}

bool Zone::give_item(PlayerEntity& p, ItemStackState& stack) {
  const int before = stack.count;
  const bool all = game::give_item(p.inv, content_, stack);
  if (stack.count != before) mark_inventory(p);
  return all;
}

bool Zone::restore_item(PlayerEntity& p, CorpseItem& it) {
  const ItemDef* d = content_.find_item(it.stack.item_id);
  const SlotCode o = it.origin;
  const MoveContext ctx = move_context(p);
  if (o != kNoSlot && !in_bank(o) && slot_valid(p.inv, o, content_, ctx.bank_slots)) {
    ItemStackState* dst = p.inv.at(o);
    bool ok = dst && dst->empty();
    if (ok && slot_container(o) == kContEquip) {
      std::string why;
      const auto slot = static_cast<EquipSlot>(slot_index(o));
      ok = d && can_wear(*d, slot, ctx.wearer, why);
      const ItemDef* pri = worn(p, EquipSlot::Primary);
      if (ok && slot == EquipSlot::Secondary && pri && pri->two_handed()) ok = false;
      if (ok && d->two_handed() && !p.inv.equip[static_cast<std::size_t>(EquipSlot::Secondary)].empty()) ok = false;
    }
    if (ok && is_bag_content(o) && d && d->is_bag()) ok = false;
    if (ok) {
      *dst = it.stack;
      it.stack = {};
      mark_inventory(p);
      return true;
    }
  }
  return give_item(p, it.stack);
}

void Zone::send_loot_window(PlayerEntity& p, const CorpseEntity& c, ZoneSink& sink) {
  proto::LootWindow w;
  w.corpse_id = c.id;
  w.name = c.name;
  w.coin = c.coin;
  for (std::size_t i = 0; i < c.items.size() && w.items.size() < 32; ++i) {
    const auto& it = c.items[i];
    if (it.empty()) continue;
    std::string owner;
    if (!c.player_corpse) {
      if (it.roll != 0) owner = "(rolling)";
      else if (it.owner != 0 && it.owner != p.char_id && (it.won || tick_ < c.free_tick)) {
        const PlayerEntity* o = find_by_char(it.owner);
        owner = o ? o->name : "another party member";
      }
    }
    w.items.push_back({static_cast<std::uint8_t>(i), it.stack.item_id, static_cast<std::uint16_t>(std::clamp(it.stack.count, 0, 65535)), owner,
                       may_take(p, c, it)});
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
  if (p.trade != 0) return refuse("You can't loot while trading.");
  if (horizontal_distance(p.pos, c->pos) > rules_.combat.loot_range) return refuse("You are too far away to loot that corpse.");
  const bool allowed = c->player_corpse ? c->owner_char_id == p.char_id
                                        : (c->party.size() > 1 ? c->shared_with(p.char_id) : (c->loot_char_id == 0 || c->loot_char_id == p.char_id));
  if (!allowed) return refuse(c->player_corpse ? "You may not loot someone else's corpse." : "You did not earn the right to loot that corpse.");
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
  bool gear = false;
  for (std::size_t i = 0; i < c->items.size(); ++i) {
    if (slot != kLootAll && i != slot) continue;
    CorpseItem& it = c->items[i];
    if (it.empty()) continue;
    if (!may_take(p, *c, it)) {
      if (slot != kLootAll) send(sink, p, proto::ChatMessage{kChatSystem, "", it.roll != 0 ? "That item is being rolled for." : "That item is not yours to take."});
      continue;
    }
    const ItemDef* def = content_.find_item(it.stack.item_id);
    if (!c->player_corpse && def && lore_conflict(p.inv, *def)) {
      send(sink, p, proto::ChatMessage{kChatSystem, "", def->name + " is lore: you already have one."});
      continue;
    }
    const int before = it.stack.count;
    const std::string id = it.stack.item_id;
    const bool equip_slot = it.origin != kNoSlot && slot_container(it.origin) == kContEquip;
    const bool all = c->player_corpse ? restore_item(p, it) : give_item(p, it.stack);
    if (it.stack.count != before) {
      moved = true;
      if (all && equip_slot && p.inv.at(it.origin) && p.inv.at(it.origin)->item_id == id) gear = true;
      const int got = before - it.stack.count;
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
    if (gear) gear_changed(p, sink);
    send_inventory(p, sink);
    p.save_dirty = false;  // the corpse write below saves the looter together with the corpse
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
