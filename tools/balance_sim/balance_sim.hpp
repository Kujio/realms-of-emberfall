// Balance simulator v1: each class solo-kills even-level mobs in a real (headless) zone, played by
// a simple autopilot whose kit comes only from content data (best rank of each line the class knows
// at that level). Reports time-to-kill, survivability, and mana efficiency per class and level.
// Used by the emberfall-balance-sim tool and by a smoke test.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/content.hpp"

namespace ef::sim {

struct FightStats {
  std::string class_id;
  int level = 1;
  int fights = 0;
  int wins = 0;
  double ttk_seconds = 0.0;      // mean time to kill over wins
  double hp_left_pct = 0.0;      // mean health left over wins (0..100)
  double mana_used_pct = 0.0;    // mean mana spent per fight, % of the pool (0 for classes without mana)
  double damage_per_mana = 0.0;  // mob health per point of mana spent (0 if none spent)
  double win_rate() const { return fights > 0 ? static_cast<double>(wins) / fights : 0.0; }
};

struct SimOptions {
  int fights = 12;
  std::uint64_t seed = 1;
  float fight_seconds = 240.0f;  // a fight that lasts longer counts as a loss
  float mob_hp_mult = 1.0f;
  float mob_damage_mult = 1.0f;
};

// One class at one level against `fights` fresh even-level mobs.
FightStats simulate(const game::Content& content, const std::string& class_id, int level, const SimOptions& opt);

// What the autopilot memorizes and uses (for the report and tests).
std::vector<std::string> kit_summary(const game::Content& content, const std::string& class_id, int level);

struct Outlier {
  std::string class_id;
  int level = 0;
  std::string why;
};
// Flags classes whose time-to-kill is more than `ttk_factor` times the median at a level (or less
// than 1/ttk_factor of it), or whose win rate is below `min_win_rate`.
std::vector<Outlier> find_outliers(const std::vector<FightStats>& rows, double ttk_factor, double min_win_rate);

// The full report as text: one row per class, one column per level.
std::string format_table(const std::vector<FightStats>& rows, const std::vector<int>& levels);

}  // namespace ef::sim
