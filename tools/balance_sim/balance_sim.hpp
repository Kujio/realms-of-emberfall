// Balance simulator v1: each class solo-kills even-level mobs in a real (headless) zone, played by
// a simple autopilot whose kit comes only from content data (best rank of each line the class knows
// at that level). Reports time-to-kill, survivability, and mana efficiency per class and level.
// Used by the emberfall-balance-sim tool and by a smoke test.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
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
  bool gear = true;  // wear the recommended kit for the level (game/items.hpp); false = bare-handed, as in Phase 4
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

// ------------------------------------------------------------------ progression (Phase 8)
// Time to level from 1 to the cap, solo and in a standard six-person group. At each sampled level the
// party pulls `fights` sparring foes in a row: buffing, fighting, resting on its own regeneration
// (sitting) until health, mana and endurance are back, then spending `pull_seconds` finding the next
// one. Deaths cost `death_seconds` and their experience loss. Experience comes from the zone's own
// award (group split and bonus included), so seconds per level = xp_to_next / measured XP per second.

inline constexpr double kNoProgress = 1.0e9;  // seconds per level when a sample earned no experience
// Level bands for the report: [first, second) levels, so 1-15 is the time from level 1 to 15.
inline constexpr std::array<std::pair<int, int>, 4> kBands{{{1, 15}, {15, 30}, {30, 45}, {45, 60}}};

struct ProgressionOptions {
  int fights = 4;  // pulls per sampled level
  int step = 1;    // sample every `step` levels (and the last); the rest are interpolated
  std::uint64_t seed = 1;
  bool gear = true;
  float pull_seconds = 25.0f;       // finding and pulling the next mob
  float death_seconds = 240.0f;     // respawn, run back, rebuff
  float rest_cap_seconds = 600.0f;  // longest rest between pulls
  float fight_seconds = 300.0f;     // a fight that lasts longer is abandoned
  float assist_seconds = 3.0f;      // a group lets its tank hold the mob this long before joining in
  // The group's prey: dungeon-strength mobs a little above the group (yellow con).
  int group_level_offset = 2;
  float group_hp_mult = 3.0f;
  float group_damage_mult = 1.5f;
};

struct LevelSample {
  int level = 1;
  bool interpolated = false;
  int fights = 0, wins = 0, deaths = 0;  // deaths of the measured character
  double fight_seconds = 0.0;            // mean per pull
  double rest_seconds = 0.0;             // mean per pull
  double xp_per_hour = 0.0;              // the measured character's
  double seconds_per_level = 0.0;
};

struct Progression {
  std::string class_id;
  bool group = false;
  std::vector<LevelSample> levels;  // 1 .. max_level-1: the time from each level to the next
  std::array<double, kBands.size()> band_hours{};
  double total_hours = 0.0;
  double hours_1_50 = 0.0, hours_50_60 = 0.0;
  int fights = 0, wins = 0, deaths = 0;  // over the sampled levels
  int stalled_level = 0;  // the first sampled level that earned no experience (0 = none): the cap is out of reach
};

// The standard group for measuring `class_id`: that class plus a Warrior, Cleric, Shaman, Wizard and
// Rogue (a Ranger or Enchanter fills in for the measured class).
std::vector<std::string> standard_group(const game::Content& content, const std::string& class_id);
// One level's sample for `party` (the first member is the one measured).
LevelSample simulate_level(const game::Content& content, const std::vector<std::string>& party, int level, const ProgressionOptions& opt);
Progression simulate_progression(const game::Content& content, const std::string& class_id, bool group, const ProgressionOptions& opt);
// Classes whose total time to the cap is more than `factor` times the median of their mode (or less
// than 1/factor of it), or that died in more than `max_death_rate` of their pulls.
std::vector<Outlier> progression_outliers(const std::vector<Progression>& rows, double factor, double max_death_rate);
// Hours per band and in total, solo and grouped, plus the per-level detail when `detail`.
std::string format_progression(const std::vector<Progression>& rows, bool detail);

}  // namespace ef::sim
