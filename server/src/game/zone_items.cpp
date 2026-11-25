// Zone item intents: moving and equipping, destroying, using (consumables, clickies, scrolls), worn
// gear stats and visuals, weapon procs, vendors, and the bank. The rules themselves are in items.cpp;
// every change marks the character for saving (one snapshot = one atomic save).
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001B3ULL; }

std::uint16_t u16(int v) { return static_cast<std::uint16_t>(std::clamp(v, 0, 65535)); }

bool pack_slot(SlotCode s) { return slot_container(s) == kContGeneral || (is_bag_content(s) && !in_bank(s)); }

}  // namespace

// ---------------------------------------------------------------- helpers

std::string Zone::item_name(const std::string& id) const {
  const ItemDef* d = content_.find_item(id);
  return d ? d->name : id;
}

const ItemDef* Zone::worn(const PlayerEntity& p, EquipSlot slot) const {
  const auto& s = p.inv.equip[static_cast<std::size_t>(slot)];
  return s.empty() ? nullptr : content_.find_item(s.item_id);
}

Wearer Zone::wearer_of(const PlayerEntity& p) const {
  const SkillCap* dw = p.class_def ? p.class_def->skill("dual_wield") : nullptr;
  return {p.race_def, p.class_def, p.level, dw && skill_cap(*dw, p.level) > 0};
}

MoveContext Zone::move_context(const PlayerEntity& p) const {
  MoveContext ctx;
  ctx.wearer = wearer_of(p);
  ctx.bank_open = p.banking != 0 && npc_in_reach(p, p.banking, "banker") != nullptr;
  ctx.bank_slots = std::clamp(rules_.economy.bank_slots, 1, static_cast<int>(kBankSlots));
  return ctx;
}

void Zone::mark_inventory(PlayerEntity& p) {
  p.inv_dirty = true;
  p.save_dirty = true;
}

void Zone::tell(EntityId player, const std::string& text, ZoneSink& sink) {
  if (PlayerEntity* p = find(player)) send(sink, *p, proto::ChatMessage{kChatSystem, "", text});
}

bool Zone::in_trade(EntityId player) const {
  const PlayerEntity* p = find(player);
  return p && p->trade != 0;
}

bool Zone::items_locked(PlayerEntity& p, ZoneSink& sink) {
  if (p.dead) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are dead."}), true;
  if (p.trade != 0) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You cannot do that while trading."}), true;
  return false;
}

const NpcEntity* Zone::npc_in_reach(const PlayerEntity& p, EntityId npc, const char* role) const {
  auto it = npcs_.find(npc);
  if (it == npcs_.end()) return nullptr;
  if (role && it->second.def->role != role) return nullptr;
  if (!std::binary_search(p.known.begin(), p.known.end(), npc)) return nullptr;
  if (horizontal_distance(p.pos, it->second.pos) > rules_.economy.npc_range) return nullptr;
  return &it->second;
}

std::string Zone::held_item(const PlayerEntity& p, EquipSlot slot) const {
  const auto& s = p.inv.equip[static_cast<std::size_t>(slot)];
  return s.empty() ? std::string() : s.item_id;
}

// ---------------------------------------------------------------- gear

void Zone::gear_changed(PlayerEntity& p, ZoneSink& sink) {
  refresh_stats(p);
  send_character_sheet(p, sink, false);
  broadcast_equipment(p, sink);
  p.stats_hash = 0;  // max hp/mana may have changed
}

std::string Zone::equipment_sig(const PlayerEntity& p) const {
  std::string sig;
  for (EquipSlot s : {EquipSlot::Primary, EquipSlot::Secondary, EquipSlot::Ranged, EquipSlot::Chest, EquipSlot::Head}) {
    sig += held_item(p, s);
    sig += '|';
  }
  return sig;
}

void Zone::broadcast_equipment(PlayerEntity& p, ZoneSink& sink) {
  const std::string main = held_item(p, EquipSlot::Primary);
  const std::string off = held_item(p, EquipSlot::Secondary);
  const std::string ranged = held_item(p, EquipSlot::Ranged);
  // Chest and head choose the outfit and helmet other players see (Phase 9).
  const std::string chest = held_item(p, EquipSlot::Chest);
  const std::string head = held_item(p, EquipSlot::Head);
  std::string sig = equipment_sig(p);
  if (sig == p.held_sig) return;
  p.held_sig = std::move(sig);
  const proto::EntityEquipment msg{p.id, main, off, ranged, chest, head};
  proto::encode_packet_into(msg, send_buf_);
  for (auto& [oid, o] : players_)
    if (oid == p.id || std::binary_search(o.known.begin(), o.known.end(), p.id)) sink.zone_send(o, proto::EntityEquipment::kChannel, send_buf_);
}

void Zone::send_character_sheet(PlayerEntity& p, ZoneSink& sink, bool force) {
  const DerivedStats& d = p.stats;
  const float buff_haste = p.buff_total(BuffStat::AttackSpeedPct);
  const int haste = static_cast<int>(std::lround(std::max(static_cast<float>(d.haste), std::max(0.0f, buff_haste)) + std::min(0.0f, buff_haste)));
  auto res = [&](ResistType t) {
    return static_cast<std::int32_t>(d.resists[static_cast<std::size_t>(t)] + resist_bonus(p.buffs, t));
  };
  proto::CharacterSheet s;
  s.str = u16(d.stats.str);
  s.sta = u16(d.stats.sta);
  s.agi = u16(d.stats.agi);
  s.dex = u16(d.stats.dex);
  s.wis = u16(d.stats.wis);
  s.intel = u16(d.stats.intel);
  s.cha = u16(d.stats.cha);
  s.resist_magic = res(ResistType::Magic);
  s.resist_fire = res(ResistType::Fire);
  s.resist_cold = res(ResistType::Cold);
  s.resist_poison = res(ResistType::Poison);
  s.resist_disease = res(ResistType::Disease);
  s.haste = haste;
  s.weapon_damage = u16(static_cast<int>(std::lround(d.weapon_damage)));
  s.weapon_delay_ms = static_cast<std::uint32_t>(std::lround(d.weapon_delay * 1000.0f));
  s.offhand_damage = u16(static_cast<int>(std::lround(d.offhand_damage)));
  s.offhand_delay_ms = static_cast<std::uint32_t>(std::lround(d.offhand_delay * 1000.0f));
  s.ac = u16(d.ac);
  s.max_hp = d.max_hp;
  s.max_mana = d.max_mana;
  s.max_endurance = d.max_endurance;
  s.crit_pct = (d.crit_chance + p.buff_total(BuffStat::CritChancePct) / 100.0f) * 100.0f;
  // Compare what would be sent (buffs to resists and crit included), not just the derived stats.
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::int64_t v : {std::int64_t{s.str}, std::int64_t{s.sta}, std::int64_t{s.agi}, std::int64_t{s.dex}, std::int64_t{s.wis},
                         std::int64_t{s.intel}, std::int64_t{s.cha}, std::int64_t{s.resist_magic}, std::int64_t{s.resist_fire},
                         std::int64_t{s.resist_cold}, std::int64_t{s.resist_poison}, std::int64_t{s.resist_disease}, std::int64_t{s.haste},
                         std::int64_t{s.weapon_damage}, std::int64_t{s.weapon_delay_ms}, std::int64_t{s.offhand_damage},
                         std::int64_t{s.offhand_delay_ms}, std::int64_t{s.ac}, std::int64_t{s.max_hp}, std::int64_t{s.max_mana},
                         std::int64_t{s.max_endurance}, static_cast<std::int64_t>(std::lround(s.crit_pct * 100.0f))})
    h = mix(h, static_cast<std::uint64_t>(v));
  if (!force && h == p.sheet_hash) return;
  p.sheet_hash = h;
  send(sink, p, s);
}

void Zone::try_proc(PlayerEntity& p, const ItemDef* weapon, MobEntity& m, ZoneSink& sink) {
  if (!weapon || weapon->proc.effect.empty() || !m.alive()) return;
  if (!rng_.chance(proc_chance(weapon->proc.rate, weapon->delay))) return;
  const AbilityDef* fx = content_.find_item_effect(weapon->proc.effect);
  if (!fx) return;
  resolve_ability(p, *fx, m.id, sink);
}

// ---------------------------------------------------------------- moving, destroying, using

void Zone::move_item(EntityId player, SlotCode from, SlotCode to, int count, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) {
    mark_inventory(p);
    return;
  }
  const MoveContext ctx = move_context(p);
  if ((in_bank(from) || in_bank(to)) && !ctx.bank_open) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You must be at a banker to use your bank."});
    p.inv_dirty = true;
    return;
  }
  // A clicky being cast from must not move out from under the spell.
  if (p.cast.casting() && p.cast.item_slot != kNoSlot && (p.cast.item_slot == from || p.cast.item_slot == to)) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You are using that item."});
    p.inv_dirty = true;
    return;
  }
  std::string why;
  const auto r = game::move_item(p.inv, content_, ctx, from, to, count, why);
  if (r == MoveOutcome::Refused) {
    if (!why.empty()) send(sink, p, proto::ChatMessage{kChatSystem, "", why});
    p.inv_dirty = true;  // the client may have drawn the move already: resync it
    return;
  }
  mark_inventory(p);
  if (slot_container(from) == kContEquip || slot_container(to) == kContEquip) gear_changed(p, sink);
}

void Zone::destroy_item(EntityId player, SlotCode slot, std::string_view item_id, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  const MoveContext ctx = move_context(p);
  ItemStackState* s = slot_valid(p.inv, slot, content_, ctx.bank_slots) ? p.inv.at(slot) : nullptr;
  if (!s || s->empty() || s->item_id != item_id) return send(sink, p, proto::ChatMessage{kChatSystem, "", "There is nothing like that there."});
  if (in_bank(slot) && !ctx.bank_open) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You must be at a banker to use your bank."});
  if (bag_has_items(p.inv, slot)) return send(sink, p, proto::ChatMessage{kChatSystem, "", "Empty the bag first."});
  if (p.cast.casting() && p.cast.item_slot == slot) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are using that item."});
  const std::string name = item_name(s->item_id);
  const int n = s->count;
  *s = {};
  send(sink, p, proto::ChatMessage{kChatSystem, "", "You destroy " + (n > 1 ? std::to_string(n) + " x " : std::string()) + name + "."});
  mark_inventory(p);
  if (slot_container(slot) == kContEquip) gear_changed(p, sink);
}

void Zone::use_item(EntityId player, SlotCode slot, EntityId target, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  const MoveContext ctx = move_context(p);
  const ItemStackState* s = slot_valid(p.inv, slot, content_, ctx.bank_slots) ? p.inv.at(slot) : nullptr;
  if (!s || s->empty() || in_bank(slot)) return send(sink, p, proto::ChatMessage{kChatSystem, "", "There is nothing there to use."});
  const ItemDef* def = content_.find_item(s->item_id);
  if (!def) return;
  if (def->kind == "scroll") return scribe_spell(player, slot, sink);
  if (!def->click.effect.empty()) return use_item_effect(p, *def, slot, target, sink);
  if (def->equippable() && slot_container(slot) != kContEquip) {
    const SlotCode dest = auto_equip_slot(p.inv, *def, ctx.wearer);
    if (dest == kNoSlot) {
      std::string why;
      for (std::size_t i = 0; i < kEquipSlotCount && why.empty(); ++i)
        if (def->fits(static_cast<EquipSlot>(i))) can_wear(*def, static_cast<EquipSlot>(i), ctx.wearer, why);
      return send(sink, p, proto::ChatMessage{kChatSystem, "", why.empty() ? "You cannot equip that." : why});
    }
    return move_item(player, slot, dest, 0, sink);
  }
  send(sink, p, proto::ChatMessage{kChatSystem, "", "You can't use " + def->name + "."});
}

void Zone::use_item_effect(PlayerEntity& p, const ItemDef& item, SlotCode slot, EntityId target, ZoneSink& sink) {
  const AbilityDef* a = content_.find_item_effect(item.click.effect);
  if (!a) return;
  if (item.req_level > p.level)
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "You must be level " + std::to_string(item.req_level) + " to use " + item.name + "."});
  if (item.equippable() && (!class_allowed(item, p.class_id) || !race_allowed(item, p.race)))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "You cannot use " + item.name + "."});
  if (!can_act(p, *a, sink)) return;
  EntityId tid = 0;
  std::string why;
  if (!resolve_target(p, *a, target, tid, why, false)) return ability_fail(p, *a, why, sink);
  if (p.feigned) stand_from_feign(p, sink);
  break_stealth(p, !a->beneficial(), sink);
  if (item.click.cast_time > 0.0f) {
    const std::uint32_t cast_ticks = ticks(item.click.cast_time);
    p.cast = begin_cast(*a, tid, -1, tick_, cast_ticks, p.pos);
    p.cast.item_slot = slot;
    p.cast.item_id = item.id;
    stand(p);
    p.changed_tick = tick_;
    broadcast(p.pos, proto::CastStart{p.id, tid, a->id, item.name, ms_of(cast_ticks, dt_), static_cast<std::uint8_t>(a->resist)}, sink);
    return;
  }
  pay_and_lock(p, *a, sink);
  if (item.click.recast > a->cooldown) set_cooldown(p, a->id, tick_ + ticks(item.click.recast));
  if (!a->beneficial()) mark_combat(p);
  if (item.kind == "consumable") {
    ItemStackState* s = p.inv.at(slot);
    if (s && !s->empty() && --s->count <= 0) *s = {};
    mark_inventory(p);
  }
  resolve_ability(p, *a, tid, sink);
  const std::uint32_t cd = std::max(ticks(a->cooldown), ticks(item.click.recast));
  send(sink, p, proto::AbilityResult{true, a->id, "", ms_of(cd, dt_), a->gcd ? ms_of(ticks(rules_.combat.gcd_seconds), dt_) : 0});
}

bool Zone::give(EntityId player, const std::string& item_id, int count, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  const ItemDef* d = content_.find_item(item_id);
  if (!p || !d || count < 1) return false;
  bool all = true;
  while (count > 0) {
    ItemStackState s{item_id, std::min(count, std::max(1, d->stack)), 0};
    count -= s.count;
    if (!give_item(*p, s)) all = false;
  }
  (void)sink;
  return all;
}

// ---------------------------------------------------------------- vendors

Zone::VendorState& Zone::vendor_state(const NpcEntity& n) {
  auto it = vendors_.find(n.id);
  if (it != vendors_.end()) return it->second;
  VendorState v;
  for (const auto& st : n.def->stock) {
    const ItemDef* d = content_.find_item(st.item);
    if (!d) continue;
    v.stock.push_back({d, st.count, st.count, ticks(st.restock), 0});
  }
  return vendors_.emplace(n.id, std::move(v)).first->second;
}

void Zone::restock_vendors() {
  for (auto& [id, v] : vendors_)
    for (auto& s : v.stock)
      if (s.max > 0 && s.count < s.max && tick_ >= s.next_restock) {
        ++s.count;
        s.next_restock = tick_ + std::max<std::uint32_t>(1, s.restock_ticks);
      }
}

void Zone::send_vendor_window(PlayerEntity& p, const NpcEntity& n, ZoneSink& sink) {
  VendorState& v = vendor_state(n);
  const int cha = p.stats.stats.cha;
  proto::VendorWindow w;
  w.npc_id = n.id;
  w.name = n.def->name;
  w.greeting = n.def->greeting;
  w.sell_permille = static_cast<std::uint16_t>(std::min<std::uint64_t>(65535, vendor_sell_price(1000, cha, rules_.economy)));
  for (const auto& s : v.stock) {
    if (w.entries.size() >= 128) break;
    w.entries.push_back({s.item->id, static_cast<std::uint32_t>(std::min<std::uint64_t>(0xFFFFFFFFu, vendor_buy_price(s.item->value, cha, rules_.economy))),
                         s.count, false});
  }
  // One row per item sold back, newest first, with every stack of it counted together.
  std::vector<std::pair<std::string, int>> rows;
  for (auto it = v.buyback.rbegin(); it != v.buyback.rend(); ++it) {
    auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.first == it->item_id; });
    if (row == rows.end()) rows.emplace_back(it->item_id, it->count);
    else row->second += it->count;
  }
  for (const auto& [id, total] : rows) {
    if (w.entries.size() >= 160) break;
    const ItemDef* d = content_.find_item(id);
    if (!d) continue;
    w.entries.push_back({d->id, static_cast<std::uint32_t>(std::min<std::uint64_t>(0xFFFFFFFFu, vendor_buy_price(d->value, cha, rules_.economy))),
                         total, true});
  }
  send(sink, p, w);
}

void Zone::vendor_open(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  auto it = npcs_.find(npc);
  if (it == npcs_.end() || it->second.def->role != "vendor" || !std::binary_search(p.known.begin(), p.known.end(), npc))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "That is not a merchant."});
  const NpcEntity* n = npc_in_reach(p, npc, "vendor");
  if (!n) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are too far away to trade with " + it->second.def->name + "."});
  if (npc_refuses(p, *n->def, sink)) return;
  p.vendor = npc;
  send_vendor_window(p, *n, sink);
}

void Zone::vendor_buy(EntityId player, EntityId npc, std::string_view item_id, int count, bool buyback, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  const NpcEntity* n = npc_in_reach(p, npc, "vendor");
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (!n) return say("You are too far away from that merchant.");
  VendorState& v = vendor_state(*n);
  const ItemDef* d = content_.find_item(item_id);
  if (!d) return say("The merchant does not have that.");
  count = std::clamp(count, 1, std::max(1, d->stack));
  int* available = nullptr;
  int unlimited = -1;
  int buyback_total = 0;
  VendorStockState* line = nullptr;
  if (buyback) {
    for (const auto& b : v.buyback)
      if (b.item_id == item_id) buyback_total += b.count;
    if (buyback_total <= 0) return say("The merchant does not have that any more.");
    available = &buyback_total;
  } else {
    for (auto& s : v.stock)
      if (s.item->id == item_id) line = &s;
    if (!line) return say(n->def->name + " does not sell that.");
    available = line->count < 0 ? &unlimited : &line->count;
  }
  if (*available == 0) return say(d->name + " is sold out. Check back later.");
  if (*available > 0) count = std::min(count, *available);
  if (d->lore && (count > 1 || count_item(p.inv, d->id) > 0)) return say(d->name + " is lore: you may only have one.");
  const std::uint64_t each = vendor_buy_price(d->value, p.stats.stats.cha, rules_.economy);
  const std::uint64_t total = each * static_cast<std::uint64_t>(count);
  if (p.coin < total) return say("You cannot afford " + (count > 1 ? std::to_string(count) + " x " : std::string()) + d->name + " (" + format_coin(total) + ").");
  ItemStackState stack{d->id, count, 0};
  if (!can_give(p.inv, content_, stack)) return say("You do not have room for that.");
  give_item(p, stack);
  p.coin -= total;
  if (buyback) {
    // Take from the newest stacks first, as many as were bought.
    int left = count;
    for (auto it = v.buyback.rbegin(); it != v.buyback.rend() && left > 0; ++it) {
      if (it->item_id != item_id) continue;
      const int take = std::min(left, it->count);
      it->count -= take;
      left -= take;
    }
    v.buyback.erase(std::remove_if(v.buyback.begin(), v.buyback.end(), [](const ItemStackState& s) { return s.empty(); }), v.buyback.end());
  } else if (*available > 0) {
    const bool was_full = line && line->count == line->max;
    *available -= count;
    if (line && was_full) line->next_restock = tick_ + std::max<std::uint32_t>(1, line->restock_ticks);
  }
  say("You buy " + (count > 1 ? std::to_string(count) + " x " : std::string()) + d->name + " for " + format_coin(total) + ".");
  mark_inventory(p);
  p.vendor = npc;
  send_vendor_window(p, *n, sink);
}

void Zone::vendor_sell(EntityId player, EntityId npc, SlotCode slot, int count, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  const NpcEntity* n = npc_in_reach(p, npc, "vendor");
  if (!n) return say("You are too far away from that merchant.");
  const MoveContext ctx = move_context(p);
  ItemStackState* s = pack_slot(slot) && slot_valid(p.inv, slot, content_, ctx.bank_slots) ? p.inv.at(slot) : nullptr;
  if (!s || s->empty()) return say("Sell from your pack: take it off or out of the bank first.");
  const ItemDef* d = content_.find_item(s->item_id);
  if (!d) return say("The merchant does not want that.");
  if (p.cast.casting() && p.cast.item_slot == slot) return say("You are using that item.");
  if (d->summoned || d->value <= 0) return say(n->def->name + " is not interested in " + d->name + ".");
  if (d->is_bag() && bag_has_items(p.inv, slot)) return say("Empty the bag before selling it.");
  const int n_sold = count <= 0 ? s->count : std::min(count, s->count);
  const std::uint64_t each = vendor_sell_price(d->value, p.stats.stats.cha, rules_.economy);
  if (each == 0) return say(n->def->name + " is not interested in " + d->name + ".");
  const std::uint64_t total = each * static_cast<std::uint64_t>(n_sold);
  s->count -= n_sold;
  if (s->count <= 0) *s = {};
  p.coin += total;
  if (!d->no_trade && rules_.economy.buyback_slots > 0) {
    VendorState& v = vendor_state(*n);
    if (!v.buyback.empty() && v.buyback.back().item_id == d->id && v.buyback.back().count + n_sold <= std::max(1, d->stack))
      v.buyback.back().count += n_sold;
    else
      v.buyback.push_back({d->id, n_sold, 0});
    while (static_cast<int>(v.buyback.size()) > rules_.economy.buyback_slots) v.buyback.erase(v.buyback.begin());
  }
  say("You sell " + (n_sold > 1 ? std::to_string(n_sold) + " x " : std::string()) + d->name + " for " + format_coin(total) + ".");
  mark_inventory(p);
  p.vendor = npc;
  send_vendor_window(p, *n, sink);
}

// ---------------------------------------------------------------- bank

void Zone::bank_open(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  auto it = npcs_.find(npc);
  if (it == npcs_.end() || it->second.def->role != "banker" || !std::binary_search(p.known.begin(), p.known.end(), npc))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "That is not a banker."});
  const NpcEntity* n = npc_in_reach(p, npc, "banker");
  if (!n) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are too far away from " + it->second.def->name + "."});
  if (npc_refuses(p, *n->def, sink)) return;
  p.banking = npc;
  send(sink, p, proto::BankWindow{n->id, n->def->name, static_cast<std::uint8_t>(std::clamp(rules_.economy.bank_slots, 1, 16))});
  p.inv_dirty = true;
}

void Zone::bank_coin(EntityId player, bool deposit, std::uint64_t amount, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return;
  if (p.banking == 0 || !npc_in_reach(p, p.banking, "banker")) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You must be at a banker."});
  std::uint64_t& from = deposit ? p.coin : p.bank_coin;
  std::uint64_t& to = deposit ? p.bank_coin : p.coin;
  amount = std::min(amount, from);
  if (amount == 0) return;
  from -= amount;
  to += amount;
  send(sink, p, proto::ChatMessage{kChatSystem, "", std::string(deposit ? "You deposit " : "You withdraw ") + format_coin(amount) + "."});
  mark_inventory(p);
}

void Zone::check_windows(PlayerEntity& p, ZoneSink& sink) {
  if (p.banking != 0 && (p.dead || !npc_in_reach(p, p.banking, "banker"))) {
    p.banking = 0;
    send(sink, p, proto::BankWindow{0, "", 0});
  }
  if (p.vendor != 0 && (p.dead || !npc_in_reach(p, p.vendor, "vendor"))) {
    p.vendor = 0;
    proto::VendorWindow closed;
    send(sink, p, closed);
  }
  if (p.crafting != 0 && (p.dead || !npc_in_reach(p, p.crafting, "station"))) {
    p.crafting = 0;
    send(sink, p, proto::CraftWindow{0, "", ""});
  }
}

// ---------------------------------------------------------------- auctions (zone side)

bool Zone::at_auctioneer(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return false;
  PlayerEntity& p = *pp;
  if (items_locked(p, sink)) return false;
  auto it = npcs_.find(npc);
  if (it == npcs_.end() || it->second.def->role != "auctioneer" || !std::binary_search(p.known.begin(), p.known.end(), npc))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "That is not an auctioneer."}), false;
  if (!npc_in_reach(p, npc, "auctioneer"))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are too far away from " + it->second.def->name + "."}), false;
  if (npc_refuses(p, *it->second.def, sink)) return false;
  return true;
}

bool Zone::take_for_auction(EntityId player, SlotCode slot, std::string_view item_id, std::uint64_t fee, ItemStackState& out,
                            ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return false;
  PlayerEntity& p = *pp;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  const MoveContext ctx = move_context(p);
  ItemStackState* s = pack_slot(slot) && slot_valid(p.inv, slot, content_, ctx.bank_slots) ? p.inv.at(slot) : nullptr;
  if (!s || s->empty()) return say("List items from your pack."), false;
  if (s->item_id != item_id) return say("That slot holds something else now; choose the item again."), false;
  const ItemDef* d = content_.find_item(s->item_id);
  if (!d || d->no_trade || d->summoned) return say("That cannot be sold on the board."), false;
  if (d->is_bag() && bag_has_items(p.inv, slot)) return say("Empty the bag before listing it."), false;
  if (p.cast.casting() && p.cast.item_slot == slot) return say("You are using that item."), false;
  if (p.coin < fee) return say("The listing fee is " + format_coin(fee) + ", which you cannot afford."), false;
  out = {s->item_id, s->count, 0};
  *s = {};
  p.coin -= fee;
  mark_inventory(p);
  return true;
}

bool Zone::give_from_auction(EntityId player, const ItemStackState& stack, std::uint64_t price, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return false;
  PlayerEntity& p = *pp;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  const ItemDef* d = content_.find_item(stack.item_id);
  if (!d) return say("That item no longer exists."), false;
  if (p.coin < price) return say("You cannot afford that (" + format_coin(price) + ")."), false;
  if (lore_conflict(p.inv, *d)) return say(d->name + " is lore: you already have one."), false;
  if (!can_give(p.inv, content_, stack)) return say("You do not have room for " + d->name + "."), false;
  ItemStackState copy = stack;
  give_item(p, copy);
  p.coin -= price;
  mark_inventory(p);
  return true;
}

void Zone::add_coin(EntityId player, std::uint64_t amount, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p || amount == 0) return;
  p->coin += amount;
  mark_inventory(*p);
  (void)sink;
}

}  // namespace ef::game
