#include "game/loot_rules.hpp"

#include <algorithm>

namespace ef::game {

std::size_t pick_weighted(const LootPool& pool, Rng& rng) {
  double total = 0.0;
  for (const auto& e : pool.entries) total += std::max(0.0, static_cast<double>(e.weight));
  if (pool.entries.empty() || total <= 0.0) return 0;
  double r = rng.uniform() * total;
  for (std::size_t i = 0; i < pool.entries.size(); ++i) {
    r -= std::max(0.0, static_cast<double>(pool.entries[i].weight));
    if (r < 0.0) return i;
  }
  return pool.entries.size() - 1;
}

std::vector<ItemStackState> roll_loot(const LootTable& t, Rng& rng) {
  std::vector<ItemStackState> out;
  const auto cap = static_cast<std::size_t>(std::max(0, t.max_drops));
  for (const auto& d : t.drops) {
    if (out.size() >= cap) return out;
    if (rng.chance(d.chance)) out.push_back({d.item, rng.range(d.min, d.max), 0});
  }
  for (const auto& pool : t.pools) {
    for (int r = 0; r < pool.rolls; ++r) {
      if (out.size() >= cap) return out;
      if (!rng.chance(pool.chance) || pool.entries.empty()) continue;
      const auto& e = pool.entries[pick_weighted(pool, rng)];
      if (!e.item.empty()) out.push_back({e.item, rng.range(e.min, e.max), 0});
    }
  }
  return out;
}

int resolve_rolls(std::vector<RollEntry>& entries, Rng& rng) {
  int best = -1;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto& e = entries[i];
    if (e.choice == RollChoice::Pass) {
      e.roll = 0;
      continue;
    }
    e.roll = rng.range(1, 100);
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    const auto& b = entries[static_cast<std::size_t>(best)];
    const bool better = (e.choice == RollChoice::Need && b.choice != RollChoice::Need) || (e.choice == b.choice && e.roll > b.roll);
    if (better) best = static_cast<int>(i);
  }
  return best;
}

std::vector<std::int64_t> round_robin(std::size_t items, const std::vector<std::int64_t>& party, std::size_t start) {
  std::vector<std::int64_t> out(items, 0);
  if (party.empty()) return out;
  for (std::size_t i = 0; i < items; ++i) out[i] = party[(start + i) % party.size()];
  return out;
}

std::vector<std::uint64_t> split_coin(std::uint64_t coin, std::size_t members) {
  if (members == 0) return {};
  std::vector<std::uint64_t> out(members, coin / members);
  const std::uint64_t rem = coin % members;
  for (std::uint64_t i = 0; i < rem; ++i) ++out[static_cast<std::size_t>(i)];
  return out;
}

bool worth_rolling(const ItemDef& it) { return it.equippable() || (it.rarity != "common" && !it.rarity.empty()); }

}  // namespace ef::game
