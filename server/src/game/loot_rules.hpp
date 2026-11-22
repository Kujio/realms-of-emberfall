// Pure loot rules: rolling a loot table, group loot (round-robin and need-before-greed), and coin splits.
#pragma once

#include <cstdint>
#include <vector>

#include "game/content.hpp"
#include "game/items.hpp"
#include "game/rules.hpp"

namespace ef::game {

// Independent drops first, then each weighted pool, never more than max_drops item stacks in total.
std::vector<ItemStackState> roll_loot(const LootTable& t, Rng& rng);

// One weighted pick from a pool (index into entries; the entry may be "nothing").
std::size_t pick_weighted(const LootPool& pool, Rng& rng);

enum class LootRule : std::uint8_t { NeedGreed = 0, RoundRobin = 1 };
// Wire values of LootRollChoice.choice.
enum class RollChoice : std::uint8_t { Pass = 0, Greed = 1, Need = 2 };

struct RollEntry {
  std::int64_t char_id = 0;
  RollChoice choice = RollChoice::Pass;
  int roll = 0;  // 1..100, filled by resolve_rolls for need and greed
};

// Rolls 1..100 for everyone who did not pass. Any need beats every greed; the highest roll wins, ties
// go to the earlier entry. Returns the winner's index, or -1 when everyone passed.
int resolve_rolls(std::vector<RollEntry>& entries, Rng& rng);

// Item `i` of `items` goes to party[(start + i) % party.size()].
std::vector<std::int64_t> round_robin(std::size_t items, const std::vector<std::int64_t>& party, std::size_t start);

// Splits coin evenly; the first members get the remainder one copper each.
std::vector<std::uint64_t> split_coin(std::uint64_t coin, std::size_t members);

// Items worth a need/greed roll: anything wearable, or uncommon and better.
bool worth_rolling(const ItemDef& it);

}  // namespace ef::game
