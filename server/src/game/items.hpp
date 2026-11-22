// Pure item rules: inventory layout and slot codes, equip restrictions, stats from worn gear, weapon
// numbers, prices, and validated item moves. No networking or entity state, so it is unit-tested directly;
// the zone calls these and then persists the character (every move is one atomic character save).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/content.hpp"
#include "game/rules.hpp"

namespace ef::game {

// ---------------------------------------------------------------- inventory layout

// A slot code is (container << 8) | index. Containers: 0 equipment, 1 general (pack), 2 bank,
// 0x10 + g the bag in general slot g, 0x20 + b the bag in bank slot b.
using SlotCode = std::uint16_t;
inline constexpr std::uint8_t kContEquip = 0x00;
inline constexpr std::uint8_t kContGeneral = 0x01;
inline constexpr std::uint8_t kContBank = 0x02;
inline constexpr std::uint8_t kContGeneralBag = 0x10;
inline constexpr std::uint8_t kContBankBag = 0x20;
inline constexpr std::size_t kGeneralSlots = 8;
inline constexpr std::size_t kBankSlots = 16;
inline constexpr std::size_t kMaxBagSlots = 16;
inline constexpr std::size_t kTradeSlots = 8;
inline constexpr SlotCode kNoSlot = 0xFFFF;

constexpr SlotCode slot_code(std::uint8_t container, std::uint8_t index) {
  return static_cast<SlotCode>((static_cast<unsigned>(container) << 8) | index);
}
constexpr std::uint8_t slot_container(SlotCode s) { return static_cast<std::uint8_t>(s >> 8); }
constexpr std::uint8_t slot_index(SlotCode s) { return static_cast<std::uint8_t>(s & 0xFF); }
constexpr SlotCode equip_code(EquipSlot e) { return slot_code(kContEquip, static_cast<std::uint8_t>(e)); }
constexpr SlotCode general_code(std::size_t i) { return slot_code(kContGeneral, static_cast<std::uint8_t>(i)); }
constexpr SlotCode bank_code(std::size_t i) { return slot_code(kContBank, static_cast<std::uint8_t>(i)); }
constexpr SlotCode bag_code(bool bank, std::size_t parent, std::size_t i) {
  return slot_code(static_cast<std::uint8_t>((bank ? kContBankBag : kContGeneralBag) + parent), static_cast<std::uint8_t>(i));
}

struct ItemStackState {
  std::string item_id;
  int count = 0;
  std::uint32_t expires_tick = 0;  // summoned items: the tick they vanish (0 = never)
  bool empty() const { return count <= 0; }
};

// Everything a character carries: worn gear, the 8-slot pack, the bank, and the bags in pack and bank.
// Bag contents belong to the slot holding the bag and move with it.
struct Inventory {
  std::array<ItemStackState, kEquipSlotCount> equip{};
  std::array<ItemStackState, kGeneralSlots> general{};
  std::array<ItemStackState, kBankSlots> bank{};
  std::array<std::array<ItemStackState, kMaxBagSlots>, kGeneralSlots> general_bags{};
  std::array<std::array<ItemStackState, kMaxBagSlots>, kBankSlots> bank_bags{};

  // The stack at a structurally valid code (capacity not checked), else nullptr.
  ItemStackState* at(SlotCode s);
  const ItemStackState* at(SlotCode s) const;
  // Every non-empty stack with its code: equipment, pack, pack bags, then bank and bank bags.
  void for_each(const std::function<void(SlotCode, const ItemStackState&)>& fn) const;
  void for_each_mut(const std::function<void(SlotCode, ItemStackState&)>& fn);
  void clear();
};

bool is_bag_content(SlotCode s);
// The slot holding the bag a bag-content slot belongs to (kNoSlot for anything else).
SlotCode bag_parent(SlotCode s);
// In the bank, or inside a bag in the bank.
bool in_bank(SlotCode s);
// Exists in this layout: equipment < 21, pack < 8, bank < bank_slots, and bag contents inside a bag's capacity.
bool slot_valid(const Inventory& inv, SlotCode s, const Content& c, int bank_slots);
// Slots of the bag at `bag_slot` (0 if it holds no bag).
int bag_capacity(const Inventory& inv, SlotCode bag_slot, const Content& c);
// True if a bag-type stack at `s` still holds anything.
bool bag_has_items(const Inventory& inv, SlotCode s);

// ---------------------------------------------------------------- restrictions

// A class wears its own armor weight and anything lighter.
bool armor_allowed(const std::string& armor, const std::string& class_armor);
bool class_allowed(const ItemDef& it, const std::string& class_id);
bool race_allowed(const ItemDef& it, const std::string& race_id);

struct Wearer {
  const RaceDef* race = nullptr;
  const ClassDef* cls = nullptr;
  int level = 1;
  bool dual_wield = false;  // may put a weapon in the secondary slot
};

// Whether `it` may be worn in `slot` by `w` (ignoring what else is worn). On failure sets `why`.
bool can_wear(const ItemDef& it, EquipSlot slot, const Wearer& w, std::string& why);
// Class and race only (need-before-greed eligibility; level ignored).
bool usable_by(const ItemDef& it, const RaceDef& race, const ClassDef& cls);

// ---------------------------------------------------------------- stats from gear

struct GearTotals {
  int ac = 0;
  int hp = 0;
  int mana = 0;
  int endurance = 0;
  StatBlock stats{0, 0, 0, 0, 0, 0, 0};
  std::array<int, kResistCount> resists{};
  int haste = 0;  // the hastiest worn item
};
GearTotals gear_totals(const Inventory& inv, const Content& c);

// Base stats plus worn gear (buffs are added on top by the zone).
DerivedStats apply_gear(const DerivedStats& base, const GearTotals& g);

// The melee weapon numbers for one hand. `item` null = bare fist.
struct HandWeapon {
  const ItemDef* item = nullptr;
  float damage = 1.0f;  // already scaled by the class's weapon_mult (or unarmed_mult)
  float delay = 3.0f;
};
HandWeapon main_hand(const Inventory& inv, const ClassDef& cls, int level, const Content& c);
// The secondary hand when it holds a weapon, or a bare fist when both hands are empty; nullopt-like
// (damage 0) when it holds a shield or nothing usable.
HandWeapon off_hand(const Inventory& inv, const ClassDef& cls, int level, const Content& c);
// Item haste and spell haste do not stack: the larger counts (slows still apply on top).
float swing_delay(float weapon_delay, int item_haste_pct, float buff_attack_speed_pct);
// Chance per landed hit: rate scaled by delay / 3 s, so slow weapons proc more often per swing.
float proc_chance(float rate, float delay);

// ---------------------------------------------------------------- prices

std::uint64_t vendor_buy_price(int value, int cha, const EconomyRules& r);
// What a vendor pays; 0 = it will not buy (no value). Always below the buy price.
std::uint64_t vendor_sell_price(int value, int cha, const EconomyRules& r);
std::uint64_t auction_fee(std::uint64_t price, const EconomyRules& r);

// ---------------------------------------------------------------- placing and moving

// How many of `item_id` the inventory holds anywhere (lore checks).
int count_item(const Inventory& inv, const std::string& item_id);
bool lore_conflict(const Inventory& inv, const ItemDef& it);

// Adds a stack to the pack (top-up existing stacks, then empty pack slots, then bag slots). Moves what
// fits out of `stack` (count decreases); returns true when all of it fit. Bags never go inside bags.
bool give_item(Inventory& inv, const Content& c, ItemStackState& stack);
// Room for the whole stack without changing anything.
bool can_give(const Inventory& inv, const Content& c, const ItemStackState& stack);

struct MoveContext {
  Wearer wearer;
  bool bank_open = false;
  int bank_slots = static_cast<int>(kBankSlots);
};

enum class MoveOutcome : std::uint8_t { Refused, Moved };

// Moves `count` (0 = the whole stack) from one slot to another: into an empty slot, onto a matching
// stack, or swapping with what is there. Bags move with their contents. Checks wear restrictions,
// two-handed weapons, bag nesting and bank access. Nothing changes when refused (why is set).
MoveOutcome move_item(Inventory& inv, const Content& c, const MoveContext& ctx, SlotCode from, SlotCode to, int count, std::string& why);

// A sensible vendor-and-drop kit for a class at a level (common and uncommon, nothing named): the
// heaviest armor it wears, its best weapons, a bow and arrows for archers. Slot code -> item id. Used by
// the balance simulator, tests and GM tools.
std::vector<std::pair<SlotCode, std::string>> recommended_kit(const Content& c, const RaceDef& race, const ClassDef& cls, int level);

// Equipped slot whose item fits best for an automatic equip ("use" on armor), or kNoSlot.
SlotCode auto_equip_slot(const Inventory& inv, const ItemDef& it, const Wearer& w);

}  // namespace ef::game
