// Pure tradeskill rules (Phase 13): combine success and skill-up chances, what a failure loses, and the
// pack checks a combine needs. No networking or entity state; the zone (zone_crafts.cpp) calls these.
#pragma once

#include <string>
#include <string_view>

#include "game/items.hpp"
#include "game/rules.hpp"

namespace ef::game {

// The chance a combine succeeds at `skill` for a recipe of `trivial`: success_max at or past the
// trivial, less success_per_point for each point short of it, never below success_min.
float combine_chance(int skill, int trivial, const CraftRules& r);

// The chance a combine (whatever its outcome) raises the skill by one: 0 at or past the trivial and at
// `cap`; otherwise skill_up_base, plus skill_up_per_point for each point short of the trivial beyond the
// first, at most skill_up_max.
float craft_skill_up_chance(int skill, int trivial, int cap, const CraftRules& r);

// How many of `count` units a failed combine loses (each with `loss_chance`).
int units_lost(int count, float loss_chance, Rng& rng);

// The pack is the eight pack slots and the bags in them (never the bank or worn gear).
bool in_pack(SlotCode s);
int count_in_pack(const Inventory& inv, std::string_view item_id);
// Takes up to `count` from the pack, bag contents first; returns how many were taken.
int take_from_pack(Inventory& inv, std::string_view item_id, int count);

enum class CombineCheck : std::uint8_t { Ok, MissingIngredient, MissingTool, NoRoom, Lore };
struct CombinePlan {
  CombineCheck check = CombineCheck::Ok;
  std::string item;  // what is missing
  int have = 0;      // how many of it the pack holds
  int need = 0;
};

// Whether the pack holds every ingredient and tool of `r`, the character does not already own a lore
// result (anywhere: pack, bank or worn), and the pack has room for the result once the ingredients are
// gone. Changes nothing.
CombinePlan check_combine(const Inventory& inv, const Content& c, const RecipeDef& r);

}  // namespace ef::game
