#include "game/items.hpp"

#include <algorithm>
#include <cmath>

namespace ef::game {

namespace {

bool listed(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

int armor_rank(const std::string& a) {
  for (std::size_t i = 0; i < kArmorTypes.size(); ++i)
    if (a == kArmorTypes[i]) return static_cast<int>(i);
  return -1;
}

std::array<ItemStackState, kMaxBagSlots>* bag_array(Inventory& inv, SlotCode bag_slot) {
  const std::uint8_t c = slot_container(bag_slot);
  const std::uint8_t i = slot_index(bag_slot);
  if (c == kContGeneral && i < kGeneralSlots) return &inv.general_bags[i];
  if (c == kContBank && i < kBankSlots) return &inv.bank_bags[i];
  return nullptr;
}

const std::array<ItemStackState, kMaxBagSlots>* bag_array(const Inventory& inv, SlotCode bag_slot) {
  return bag_array(const_cast<Inventory&>(inv), bag_slot);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

bool top_level_container(SlotCode s) {
  const std::uint8_t c = slot_container(s);
  return c == kContGeneral || c == kContBank;
}

}  // namespace

// ---------------------------------------------------------------- layout

ItemStackState* Inventory::at(SlotCode s) {
  const std::uint8_t c = slot_container(s);
  const std::uint8_t i = slot_index(s);
  if (c == kContEquip) return i < equip.size() ? &equip[i] : nullptr;
  if (c == kContGeneral) return i < general.size() ? &general[i] : nullptr;
  if (c == kContBank) return i < bank.size() ? &bank[i] : nullptr;
  if (c >= kContGeneralBag && c < kContGeneralBag + kGeneralSlots) return i < kMaxBagSlots ? &general_bags[c - kContGeneralBag][i] : nullptr;
  if (c >= kContBankBag && c < kContBankBag + kBankSlots) return i < kMaxBagSlots ? &bank_bags[c - kContBankBag][i] : nullptr;
  return nullptr;
}

const ItemStackState* Inventory::at(SlotCode s) const { return const_cast<Inventory*>(this)->at(s); }  // NOLINT

void Inventory::for_each(const std::function<void(SlotCode, const ItemStackState&)>& fn) const {
  const_cast<Inventory*>(this)->for_each_mut([&](SlotCode s, ItemStackState& st) { fn(s, st); });  // NOLINT
}

void Inventory::for_each_mut(const std::function<void(SlotCode, ItemStackState&)>& fn) {
  for (std::size_t i = 0; i < equip.size(); ++i)
    if (!equip[i].empty()) fn(slot_code(kContEquip, static_cast<std::uint8_t>(i)), equip[i]);
  for (std::size_t g = 0; g < general.size(); ++g) {
    if (!general[g].empty()) fn(general_code(g), general[g]);
    for (std::size_t i = 0; i < kMaxBagSlots; ++i)
      if (!general_bags[g][i].empty()) fn(bag_code(false, g, i), general_bags[g][i]);
  }
  for (std::size_t b = 0; b < bank.size(); ++b) {
    if (!bank[b].empty()) fn(bank_code(b), bank[b]);
    for (std::size_t i = 0; i < kMaxBagSlots; ++i)
      if (!bank_bags[b][i].empty()) fn(bag_code(true, b, i), bank_bags[b][i]);
  }
}

void Inventory::clear() { *this = Inventory{}; }

bool is_bag_content(SlotCode s) {
  const std::uint8_t c = slot_container(s);
  return (c >= kContGeneralBag && c < kContGeneralBag + kGeneralSlots) || (c >= kContBankBag && c < kContBankBag + kBankSlots);
}

SlotCode bag_parent(SlotCode s) {
  const std::uint8_t c = slot_container(s);
  if (c >= kContGeneralBag && c < kContGeneralBag + kGeneralSlots) return general_code(c - kContGeneralBag);
  if (c >= kContBankBag && c < kContBankBag + kBankSlots) return bank_code(c - kContBankBag);
  return kNoSlot;
}

bool in_bank(SlotCode s) {
  const std::uint8_t c = slot_container(s);
  return c == kContBank || (c >= kContBankBag && c < kContBankBag + kBankSlots);
}

int bag_capacity(const Inventory& inv, SlotCode bag_slot, const Content& c) {
  if (!top_level_container(bag_slot)) return 0;
  const ItemStackState* s = inv.at(bag_slot);
  if (!s || s->empty()) return 0;
  const ItemDef* d = c.find_item(s->item_id);
  return d && d->is_bag() ? std::clamp(d->bag_slots, 0, static_cast<int>(kMaxBagSlots)) : 0;
}

bool bag_has_items(const Inventory& inv, SlotCode s) {
  const auto* arr = top_level_container(s) ? bag_array(inv, s) : nullptr;
  return arr && std::any_of(arr->begin(), arr->end(), [](const ItemStackState& x) { return !x.empty(); });
}

bool slot_valid(const Inventory& inv, SlotCode s, const Content& c, int bank_slots) {
  if (!inv.at(s)) return false;
  const std::uint8_t cont = slot_container(s);
  const std::uint8_t i = slot_index(s);
  if (cont == kContBank) return i < std::clamp(bank_slots, 0, static_cast<int>(kBankSlots));
  if (is_bag_content(s)) {
    const SlotCode parent = bag_parent(s);
    if (slot_container(parent) == kContBank && slot_index(parent) >= std::clamp(bank_slots, 0, static_cast<int>(kBankSlots))) return false;
    return i < bag_capacity(inv, parent, c);
  }
  return true;
}

// ---------------------------------------------------------------- restrictions

bool armor_allowed(const std::string& armor, const std::string& class_armor) {
  const int a = armor_rank(armor);
  return a >= 0 && a <= armor_rank(class_armor);
}

bool class_allowed(const ItemDef& it, const std::string& class_id) { return it.classes.empty() || listed(it.classes, class_id); }
bool race_allowed(const ItemDef& it, const std::string& race_id) { return it.races.empty() || listed(it.races, race_id); }

bool usable_by(const ItemDef& it, const RaceDef& race, const ClassDef& cls) {
  if (!it.equippable() || !class_allowed(it, cls.id) || !race_allowed(it, race.id)) return false;
  if (it.kind == "armor" && !armor_allowed(it.armor, cls.armor)) return false;
  if ((it.kind == "weapon" || it.kind == "shield") && !listed(cls.weapons, it.weapon)) return false;
  return true;
}

bool can_wear(const ItemDef& it, EquipSlot slot, const Wearer& w, std::string& why) {
  if (!it.equippable()) return why = it.name + " cannot be worn.", false;
  if (!it.fits(slot)) return why = it.name + " does not fit there.", false;
  if (w.cls && !class_allowed(it, w.cls->id)) return why = "Your class cannot use " + it.name + ".", false;
  if (w.race && !race_allowed(it, w.race->id)) return why = "Your race cannot use " + it.name + ".", false;
  if (it.req_level > w.level) return why = "You must be level " + std::to_string(it.req_level) + " to use " + it.name + ".", false;
  if (w.cls && it.kind == "armor" && !armor_allowed(it.armor, w.cls->armor))
    return why = "You cannot wear " + it.armor + " armor.", false;
  if (w.cls && (it.kind == "weapon" || it.kind == "shield") && !listed(w.cls->weapons, it.weapon))
    return why = "You are not trained to use " + it.name + ".", false;
  if (slot == EquipSlot::Secondary && it.kind == "weapon" && !w.dual_wield)
    return why = "You cannot wield a weapon in your off hand.", false;
  return true;
}

// ---------------------------------------------------------------- stats from gear

GearTotals gear_totals(const Inventory& inv, const Content& c) {
  GearTotals g;
  for (const auto& s : inv.equip) {
    if (s.empty()) continue;
    const ItemDef* d = c.find_item(s.item_id);
    if (!d) continue;
    g.ac += d->ac;
    g.hp += d->hp;
    g.mana += d->mana;
    g.endurance += d->endurance;
    g.stats.str += d->stats.str;
    g.stats.sta += d->stats.sta;
    g.stats.agi += d->stats.agi;
    g.stats.dex += d->stats.dex;
    g.stats.wis += d->stats.wis;
    g.stats.intel += d->stats.intel;
    g.stats.cha += d->stats.cha;
    for (std::size_t r = 0; r < kResistCount; ++r) g.resists[r] += d->resists[r];
    g.haste = std::max(g.haste, d->haste);
  }
  return g;
}

DerivedStats apply_gear(const DerivedStats& base, const GearTotals& g) {
  DerivedStats d = base;
  auto stat = [](int a, int b) { return std::clamp(a + b, 1, 255); };
  d.stats = {stat(base.stats.str, g.stats.str), stat(base.stats.sta, g.stats.sta), stat(base.stats.agi, g.stats.agi),
             stat(base.stats.dex, g.stats.dex), stat(base.stats.wis, g.stats.wis), stat(base.stats.intel, g.stats.intel),
             stat(base.stats.cha, g.stats.cha)};
  d.ac = std::max(0, base.ac + g.ac);
  d.max_hp = std::max(1, base.max_hp + g.hp);
  d.max_mana = base.max_mana > 0 ? std::max(0, base.max_mana + g.mana) : 0;
  d.max_endurance = std::max(0, base.max_endurance + g.endurance);
  for (std::size_t r = 0; r < kResistCount; ++r) d.resists[r] = base.resists[r] + g.resists[r];
  d.haste = g.haste;
  d.crit_chance = base.crit_chance + static_cast<float>(std::max(0, d.stats.dex - base.stats.dex)) / 2000.0f;
  return d;
}

HandWeapon main_hand(const Inventory& inv, const ClassDef& cls, int level, const Content& c) {
  HandWeapon h;
  const auto& s = inv.equip[static_cast<std::size_t>(EquipSlot::Primary)];
  const ItemDef* d = s.empty() ? nullptr : c.find_item(s.item_id);
  if (d && d->kind == "weapon" && d->damage > 0) {
    h.item = d;
    h.damage = std::max(1.0f, static_cast<float>(d->damage) * cls.combat.weapon_mult);
    h.delay = std::max(0.5f, d->delay);
    return h;
  }
  const float L = static_cast<float>(std::max(1, level));
  h.damage = std::max(1.0f, (cls.combat.weapon_damage_base + cls.combat.weapon_damage_per_level * L) * cls.combat.unarmed_mult);
  h.delay = cls.combat.weapon_delay;
  return h;
}

HandWeapon off_hand(const Inventory& inv, const ClassDef& cls, int level, const Content& c) {
  const auto& sec = inv.equip[static_cast<std::size_t>(EquipSlot::Secondary)];
  const auto& pri = inv.equip[static_cast<std::size_t>(EquipSlot::Primary)];
  const ItemDef* d = sec.empty() ? nullptr : c.find_item(sec.item_id);
  if (d && d->kind == "weapon" && d->damage > 0) {
    HandWeapon h;
    h.item = d;
    h.damage = std::max(1.0f, static_cast<float>(d->damage) * cls.combat.weapon_mult);
    h.delay = std::max(0.5f, d->delay);
    return h;
  }
  if (sec.empty() && pri.empty()) return main_hand(inv, cls, level, c);  // two bare fists
  HandWeapon none;
  none.damage = 0.0f;
  return none;
}

float swing_delay(float weapon_delay, int item_haste_pct, float buff_attack_speed_pct) {
  // Positive haste: the larger of item and spell haste. Slows (negative spell haste) apply as well.
  const float item = static_cast<float>(std::clamp(item_haste_pct, 0, 100));
  const float buff_haste = std::max(0.0f, buff_attack_speed_pct);
  const float slow = std::min(0.0f, buff_attack_speed_pct);
  const float speed = std::clamp(1.0f + (std::max(item, buff_haste) + slow) / 100.0f, 0.1f, 3.0f);
  return std::max(0.2f, weapon_delay / speed);
}

float proc_chance(float rate, float delay) { return std::clamp(rate * std::max(0.5f, delay) / 3.0f, 0.0f, 0.95f); }

// ---------------------------------------------------------------- prices

std::uint64_t vendor_buy_price(int value, int cha, const EconomyRules& r) {
  if (value <= 0) return 0;
  const float mult = std::clamp(r.buy_base - r.buy_per_cha * static_cast<float>(cha - 75), r.min_buy, r.max_buy);
  return static_cast<std::uint64_t>(std::max(1.0, std::ceil(static_cast<double>(value) * static_cast<double>(mult))));
}

std::uint64_t vendor_sell_price(int value, int cha, const EconomyRules& r) {
  if (value <= 0) return 0;
  const float mult = std::clamp(r.sell_base + r.sell_per_cha * static_cast<float>(cha - 75), r.min_sell, r.max_sell);
  const auto paid = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::floor(static_cast<double>(value) * static_cast<double>(mult))));
  // Never as much as the vendor would charge for it (no buy-sell loop).
  const std::uint64_t buy = vendor_buy_price(value, cha, r);
  return buy > 1 ? std::min(paid, buy - 1) : 0;
}

std::uint64_t auction_fee(std::uint64_t price, const EconomyRules& r) {
  const double pct = static_cast<double>(std::clamp(r.auction_fee_pct, 0.0f, 100.0f)) / 100.0;
  const auto fee = static_cast<std::uint64_t>(std::ceil(static_cast<double>(price) * pct));
  return std::max<std::uint64_t>(fee, static_cast<std::uint64_t>(std::max(0, r.auction_min_fee)));
}

// ---------------------------------------------------------------- placing

int count_item(const Inventory& inv, const std::string& item_id) {
  int n = 0;
  inv.for_each([&](SlotCode, const ItemStackState& s) {
    if (s.item_id == item_id) n += s.count;
  });
  return n;
}

bool lore_conflict(const Inventory& inv, const ItemDef& it) { return it.lore && count_item(inv, it.id) > 0; }

namespace {

// Candidate pack slots in placement order: pack slots, then every bag slot in the pack.
template <class F>
void each_pack_slot(const Inventory& inv, const Content& c, bool include_bags, F&& fn) {
  for (std::size_t g = 0; g < kGeneralSlots; ++g)
    if (!fn(general_code(g))) return;
  if (!include_bags) return;
  for (std::size_t g = 0; g < kGeneralSlots; ++g) {
    const int cap = bag_capacity(inv, general_code(g), c);
    for (int i = 0; i < cap; ++i)
      if (!fn(bag_code(false, g, static_cast<std::size_t>(i)))) return;
  }
}

bool place(Inventory& inv, const Content& c, ItemStackState& stack, bool dry_run) {
  const ItemDef* def = c.find_item(stack.item_id);
  const int max_stack = def ? std::max(1, def->stack) : 1;
  const bool is_bag = def && def->is_bag();
  int left = stack.count;
  // Top up matching stacks first (pack, then bags).
  if (max_stack > 1) {
    each_pack_slot(inv, c, true, [&](SlotCode code) {
      if (left <= 0) return false;
      ItemStackState* s = inv.at(code);
      if (!s->empty() && s->item_id == stack.item_id && s->count < max_stack) {
        const int move = std::min(left, max_stack - s->count);
        left -= move;
        if (!dry_run) {
          s->count += move;
          // A topped-up summoned stack vanishes with its oldest part.
          if (stack.expires_tick != 0) s->expires_tick = s->expires_tick == 0 ? stack.expires_tick : std::min(s->expires_tick, stack.expires_tick);
        }
      }
      return true;
    });
  }
  // Then empty slots: the pack first, then bags (never bags inside bags).
  each_pack_slot(inv, c, !is_bag, [&](SlotCode code) {
    if (left <= 0) return false;
    ItemStackState* s = inv.at(code);
    if (s->empty()) {
      const int move = std::min(left, max_stack);
      left -= move;
      if (!dry_run) *s = {stack.item_id, move, stack.expires_tick};
    }
    return true;
  });
  if (!dry_run) stack.count = left;
  return left <= 0;
}

}  // namespace

bool give_item(Inventory& inv, const Content& c, ItemStackState& stack) {
  if (stack.empty()) return true;
  return place(inv, c, stack, false);
}

bool can_give(const Inventory& inv, const Content& c, const ItemStackState& stack) {
  if (stack.empty()) return true;
  ItemStackState copy = stack;
  return place(const_cast<Inventory&>(inv), c, copy, true);  // NOLINT: dry run never writes
}

// ---------------------------------------------------------------- moving

namespace {

// Can stack `s` (item `d`) sit in slot `to`? Equipment checks wear rules and the two-hander pairing
// (against `inv` as it will be, given that `vacating` is being emptied by the same move).
bool fits_slot(const Inventory& inv, const ItemDef* d, const ItemStackState& s, SlotCode to, const MoveContext& ctx,
               SlotCode vacating, std::string& why) {
  const std::uint8_t cont = slot_container(to);
  if (cont == kContEquip) {
    if (!d) return why = "That item cannot be worn.", false;
    const auto slot = static_cast<EquipSlot>(slot_index(to));
    if (!can_wear(*d, slot, ctx.wearer, why)) return false;
    if (s.count > 1 && d->kind != "ammo") return why = "You can only wear one of those.", false;
    if (slot == EquipSlot::Primary && d->two_handed()) {
      const SlotCode sec = equip_code(EquipSlot::Secondary);
      if (!inv.equip[static_cast<std::size_t>(EquipSlot::Secondary)].empty() && vacating != sec)
        return why = "You must empty your off hand to wield a two-handed weapon.", false;
    }
    return true;
  }
  if (is_bag_content(to) && d && d->is_bag()) return why = "Bags cannot go inside bags.", false;
  return true;  // pack and bank slots hold anything
}

bool two_hander_in_primary(const Inventory& inv, const Content& c) {
  const auto& pri = inv.equip[static_cast<std::size_t>(EquipSlot::Primary)];
  if (pri.empty()) return false;
  const ItemDef* d = c.find_item(pri.item_id);
  return d && d->two_handed();
}

}  // namespace

MoveOutcome move_item(Inventory& inv, const Content& c, const MoveContext& ctx, SlotCode from, SlotCode to, int count, std::string& why) {
  if (from == to) return why = "", MoveOutcome::Refused;
  if (!slot_valid(inv, from, c, ctx.bank_slots) || !slot_valid(inv, to, c, ctx.bank_slots)) return why = "You can't put that there.", MoveOutcome::Refused;
  ItemStackState* src = inv.at(from);
  ItemStackState* dst = inv.at(to);
  if (src->empty()) return why = "There is nothing there.", MoveOutcome::Refused;
  if ((in_bank(from) || in_bank(to)) && !ctx.bank_open) return why = "You must be at a banker to use your bank.", MoveOutcome::Refused;
  const ItemDef* sd = c.find_item(src->item_id);
  const ItemDef* dd = dst->empty() ? nullptr : c.find_item(dst->item_id);
  const int max_stack = sd ? std::max(1, sd->stack) : 1;
  const int want = count <= 0 || count >= src->count ? src->count : count;
  // A bag never moves into its own contents (or any bag).
  if (sd && sd->is_bag() && !top_level_container(to)) return why = "Bags can only go in your pack or bank slots.", MoveOutcome::Refused;
  if (bag_parent(to) == from || bag_parent(from) == to) {
    // Moving something into the bag at `from` from inside it, or the bag into itself.
    if (sd && sd->is_bag()) return why = "A bag cannot go inside itself.", MoveOutcome::Refused;
    if (dd && dd->is_bag()) return why = "A bag cannot go inside itself.", MoveOutcome::Refused;
  }

  // Merge onto a matching stack.
  if (dd && dst->item_id == src->item_id && max_stack > 1) {
    if (dst->count >= max_stack) {
      if (want != src->count) return why = "That stack is full.", MoveOutcome::Refused;
      // Full target: fall through to a swap (no visible change, but harmless).
    } else {
      if (!fits_slot(inv, sd, *src, to, ctx, kNoSlot, why)) return MoveOutcome::Refused;
      const int move = std::min(want, max_stack - dst->count);
      dst->count += move;
      if (src->expires_tick != 0) dst->expires_tick = dst->expires_tick == 0 ? src->expires_tick : std::min(dst->expires_tick, src->expires_tick);
      src->count -= move;
      if (src->count <= 0) *src = {};
      return MoveOutcome::Moved;
    }
  }

  // Split part of a stack into an empty slot.
  if (want < src->count) {
    if (!dst->empty()) return why = "Split a stack into an empty slot.", MoveOutcome::Refused;
    if (slot_container(to) == kContEquip && sd && sd->kind != "ammo") return why = "You can only wear one of those.", MoveOutcome::Refused;
    if (!fits_slot(inv, sd, *src, to, ctx, kNoSlot, why)) return MoveOutcome::Refused;
    *dst = {src->item_id, want, src->expires_tick};
    src->count -= want;
    return MoveOutcome::Moved;
  }

  // Whole stack: into an empty slot, or swap.
  if (!fits_slot(inv, sd, *src, to, ctx, from, why)) return MoveOutcome::Refused;
  if (!dst->empty()) {
    if (dd && dd->is_bag() && !top_level_container(from)) return why = "Bags can only go in your pack or bank slots.", MoveOutcome::Refused;
    if (!fits_slot(inv, dd, *dst, from, ctx, to, why)) return MoveOutcome::Refused;
  }
  // Two-handed pairing after the move: a two-hander in the primary with anything in the secondary is refused.
  const bool touches_hands = from == equip_code(EquipSlot::Primary) || from == equip_code(EquipSlot::Secondary) ||
                             to == equip_code(EquipSlot::Primary) || to == equip_code(EquipSlot::Secondary);
  std::swap(*src, *dst);
  if (touches_hands && two_hander_in_primary(inv, c) && !inv.equip[static_cast<std::size_t>(EquipSlot::Secondary)].empty()) {
    std::swap(*src, *dst);
    return why = "A two-handed weapon needs both hands free.", MoveOutcome::Refused;
  }
  // Bag contents travel with their bag.
  if ((sd && sd->is_bag()) || (dd && dd->is_bag())) {
    auto* a = bag_array(inv, from);
    auto* b = bag_array(inv, to);
    if (a && b) std::swap(*a, *b);
  }
  return MoveOutcome::Moved;
}

std::vector<std::pair<SlotCode, std::string>> recommended_kit(const Content& c, const RaceDef& race, const ClassDef& cls, int level) {
  std::vector<std::pair<SlotCode, std::string>> kit;
  auto ok = [&](const ItemDef& it) {
    return it.req_level <= level && it.source.empty() && !it.summoned && (it.rarity == "common" || it.rarity == "uncommon") && usable_by(it, race, cls);
  };
  auto stat_sum = [](const ItemDef& it) {
    return it.ac * 3 + it.hp + it.mana + it.stats.str + it.stats.sta + it.stats.agi + it.stats.dex + it.stats.wis + it.stats.intel + it.stats.cha;
  };
  // Armor and jewelry: per slot, the highest-level fit (then the most stats).
  for (std::size_t i = 0; i < kEquipSlotCount; ++i) {
    const auto slot = static_cast<EquipSlot>(i);
    if (slot == EquipSlot::Primary || slot == EquipSlot::Secondary || slot == EquipSlot::Ranged || slot == EquipSlot::Ammo) continue;
    const ItemDef* best = nullptr;
    for (const auto& it : c.items) {
      if (!it.fits(slot) || !ok(it) || it.kind == "weapon") continue;
      if (!best || it.req_level > best->req_level || (it.req_level == best->req_level && stat_sum(it) > stat_sum(*best))) best = &it;
    }
    if (best) kit.emplace_back(equip_code(slot), best->id);
  }
  auto dps = [](const ItemDef& it) { return static_cast<float>(it.damage) / std::max(0.5f, it.delay); };
  const ItemDef* one = nullptr;
  const ItemDef* two = nullptr;
  const ItemDef* shield = nullptr;
  const ItemDef* bow = nullptr;
  const ItemDef* arrows = nullptr;
  for (const auto& it : c.items) {
    if (!ok(it)) continue;
    if (it.kind == "weapon" && it.fits(EquipSlot::Primary)) {
      const ItemDef*& slot = it.two_handed() ? two : one;
      if (!slot || dps(it) > dps(*slot)) slot = &it;
    } else if (it.kind == "shield") {
      if (!shield || it.ac > shield->ac) shield = &it;
    } else if (it.kind == "weapon" && it.weapon == "archery") {
      if (!bow || dps(it) > dps(*bow)) bow = &it;
    } else if (it.kind == "ammo") {
      if (!arrows || it.damage > arrows->damage) arrows = &it;
    }
  }
  const SkillCap* dwc = cls.skill("dual_wield");
  const bool dual = dwc && skill_cap(*dwc, level) > 0;
  const bool fists = cls.combat.unarmed_mult >= 1.0f;  // monks: bare fists unless a weapon beats them
  const float fist_dps = (cls.combat.weapon_damage_base + cls.combat.weapon_damage_per_level * static_cast<float>(std::max(1, level))) *
                         cls.combat.unarmed_mult / std::max(0.5f, cls.combat.weapon_delay) / std::max(0.01f, cls.combat.weapon_mult);
  const float s1 = one ? dps(*one) : 0.0f;
  const float s2 = two ? dps(*two) : 0.0f;
  if (fists && std::max(s1, s2) <= fist_dps) {
    // keep both hands free
  } else if (dual && one && s1 * 1.5f >= s2) {
    kit.emplace_back(equip_code(EquipSlot::Primary), one->id);
    kit.emplace_back(equip_code(EquipSlot::Secondary), one->id);
  } else if (two && (s2 > s1 * 1.2f || (!shield && s2 > s1))) {
    kit.emplace_back(equip_code(EquipSlot::Primary), two->id);
  } else if (one) {
    kit.emplace_back(equip_code(EquipSlot::Primary), one->id);
    if (shield) kit.emplace_back(equip_code(EquipSlot::Secondary), shield->id);
  }
  if (bow && std::find(cls.weapons.begin(), cls.weapons.end(), "archery") != cls.weapons.end()) {
    kit.emplace_back(equip_code(EquipSlot::Ranged), bow->id);
    if (arrows) kit.emplace_back(equip_code(EquipSlot::Ammo), arrows->id);
  }
  return kit;
}

AmmoCheck check_ammo(const Inventory& inv, const Content& c, std::string_view weapon) {
  const ItemStackState& rs = inv.equip[static_cast<std::size_t>(EquipSlot::Ranged)];
  const ItemDef* ranged = rs.empty() ? nullptr : c.find_item(rs.item_id);
  if (!ranged || ranged->weapon != weapon) return AmmoCheck::NoWeapon;
  const ItemStackState& as = inv.equip[static_cast<std::size_t>(EquipSlot::Ammo)];
  const ItemDef* ammo = as.empty() ? nullptr : c.find_item(as.item_id);
  if (!ammo) return AmmoCheck::NoAmmo;
  return ammo->kind == "ammo" && ammo->weapon == weapon ? AmmoCheck::Ok : AmmoCheck::WrongAmmo;
}

int spend_ammo(Inventory& inv) {
  ItemStackState& s = inv.equip[static_cast<std::size_t>(EquipSlot::Ammo)];
  if (s.empty()) return -1;
  if (--s.count <= 0) {
    s = {};
    return 0;
  }
  return s.count;
}

SlotCode auto_equip_slot(const Inventory& inv, const ItemDef& it, const Wearer& w) {
  SlotCode fallback = kNoSlot;
  for (std::size_t i = 0; i < kEquipSlotCount; ++i) {
    const auto slot = static_cast<EquipSlot>(i);
    std::string why;
    if (!it.fits(slot) || !can_wear(it, slot, w, why)) continue;
    if (inv.equip[i].empty()) return equip_code(slot);
    if (fallback == kNoSlot) fallback = equip_code(slot);
  }
  return fallback;
}

}  // namespace ef::game
