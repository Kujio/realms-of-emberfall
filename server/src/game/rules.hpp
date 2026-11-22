// Pure, unit-tested game rules: derived stats, combat math, XP/con, death penalty, regen, and
// skill-ups. No networking, I/O, or entity state; every tunable comes from content/rules.json.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "game/content.hpp"

namespace ef::game {

// Small, fast, seedable RNG (xoshiro256**) so combat is deterministic under test.
class Rng {
 public:
  explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ULL);
  std::uint64_t next();
  double uniform();                // [0, 1)
  int range(int lo, int hi);       // inclusive; returns lo if hi <= lo
  bool chance(double p) { return uniform() < p; }
  bool chance(float p) { return uniform() < static_cast<double>(p); }

 private:
  std::uint64_t s_[4]{};
};

// ---------------------------------------------------------------- stats

struct DerivedStats {
  StatBlock stats;
  int max_hp = 1;
  int max_mana = 0;
  int max_endurance = 0;
  int ac = 0;
  float weapon_damage = 1.0f;  // main hand (gear-derived from Phase 5; bare fists otherwise)
  float weapon_delay = 3.0f;   // seconds
  float crit_chance = 0.0f;
  // Phase 5: from worn gear.
  float offhand_damage = 0.0f;  // 0 = nothing to swing in the off hand
  float offhand_delay = 3.0f;
  std::array<int, kResistCount> resists{};  // index = ResistType
  int haste = 0;                            // hastiest worn item, percent
};

StatBlock combine_stats(const StatBlock& race, const StatBlock& class_bonus);
DerivedStats derive_stats(const RaceDef& race, const ClassDef& cls, int level);
// 0 when the skill is not yet unlocked at this level.
int skill_cap(const SkillCap& cap, int level);

// ---------------------------------------------------------------- consider and experience

Con con_color(int player_level, int mob_level, const ConRules& rules);
const char* con_name(Con c);

// Upper bound for any XP amount (well inside uint64 and exact in double). Content validation
// rejects curves whose cumulative total reaches it.
inline constexpr double kMaxXp = 1.0e15;

// XP needed to advance from `level` to `level + 1`.
std::uint64_t xp_to_next(int level, const XpRules& rules);
// Total XP at the start of `level` (level 1 = 0).
std::uint64_t total_xp_for_level(int level, const XpRules& rules);
// Level for a total XP amount, capped at max_level.
int level_for_xp(std::uint64_t total_xp, int max_level, const XpRules& rules);
// How much a mob of `hp_mult` toughness is worth relative to a standard one (Phase 8):
// clamp(1 + toughness_weight x (hp_mult - 1), toughness_min, toughness_max).
double toughness_xp_mult(double hp_mult, const XpRules& xp);
// XP for killing a mob, before group splitting, times `toughness` (toughness_xp_mult). Grey cons give 0.
std::uint64_t kill_xp(int player_level, int mob_level, const XpRules& xp, const ConRules& con, double toughness = 1.0);
// Splits a kill's XP among group members by level, with a per-member group bonus.
std::vector<std::uint64_t> split_group_xp(std::uint64_t base_xp, const std::vector<int>& member_levels, const XpRules& rules);

// Phase 7: a group kill. `in_range` = alive, in the zone and within social.group.xp_range of the kill.
struct GroupXpMember {
  int level = 1;
  bool in_range = true;
};
struct GroupXpShare {
  std::uint64_t xp = 0;
  bool too_low = false;       // too far below the group's highest member to share
  bool out_of_range = false;  // dead, elsewhere, or too far away
};
// The lowest level that shares a kill with a group whose highest member is `highest`:
// highest - max(level_window, floor(highest * level_fraction)), at least 1.
int group_level_floor(int highest, int level_window, float level_fraction);
// Shares one kill's experience. The mob cons against the highest member in range (so a high-level
// member makes a trivial mob give nothing), members below group_level_floor get nothing, and the rest
// split the kill (with the group bonus for each sharer) by level, as split_group_xp does.
std::vector<GroupXpShare> group_xp_shares(int mob_level, const std::vector<GroupXpMember>& members, const XpRules& xp, const ConRules& con,
                                          int level_window, float level_fraction, double toughness = 1.0);
// XP lost on death: a fraction of the current level's span, not below 0 total. No loss under the
// configured minimum level.
std::uint64_t death_xp_loss(std::uint64_t total_xp, int level, const DeathRules& death, const XpRules& xp);

// ---------------------------------------------------------------- combat math

float hit_chance(int offense, int defense, int attacker_level, int defender_level, const CombatRules& r);
// Fraction of melee damage removed by armor.
float mitigation(int ac, int attacker_level, const CombatRules& r);
// Dodge or parry chance from a skill value (0 skill = 0 chance).
float avoid_chance(int skill, int attacker_level, const CombatRules& r);
float riposte_chance(int skill, int attacker_level, const CombatRules& r);
float double_attack_chance(int skill, const CombatRules& r);
bool stun_lands(int attacker_level, int target_level, bool target_immune, const CombatRules& r);
float taunt_chance(int taunt_skill, int skill_cap_value, int attacker_level, int target_level, const CombatRules& r);
float skill_up_chance(int value, int cap, const CombatRules& r);

struct HitRange {
  int min = 1;
  int max = 1;
};
HitRange player_hit_range(const DerivedStats& d, int level);
// Rolled damage for an ability effect with a +-20% spread.
int ability_damage(const EffectDef& e, int level, Rng& rng);

struct MobStats {
  int max_hp = 1;
  HitRange hit;
  int ac = 0;
  int offense = 0;
  int defense = 0;
  int avoid_skill = 0;
};
MobStats mob_stats(const MobDef& def, const MobScaling& s);

enum class SwingResult : std::uint8_t { Miss, Hit, Crit, Dodge, Parry, Riposte };

struct AttackerView {
  int level = 1;
  int offense = 0;
  HitRange range;
  float crit_chance = 0.0f;
  float damage_mult = 1.0f;  // offensive discipline etc.
};

struct DefenderView {
  int level = 1;
  int defense = 0;
  int ac = 0;
  int dodge = 0;    // skill values; 0 = cannot
  int parry = 0;
  int riposte = 0;
  float damage_taken_mult = 1.0f;  // defensive discipline etc.
};

struct SwingOutcome {
  SwingResult result = SwingResult::Miss;
  int damage = 0;
};

// One melee swing: hit roll, then riposte/parry/dodge, then damage, crit, and mitigation.
SwingOutcome resolve_swing(const AttackerView& a, const DefenderView& d, const CombatRules& r, Rng& rng);
// Applies armor and multipliers to raw damage; never below 1 for a landed hit.
int mitigate(int raw, int ac, int attacker_level, float damage_taken_mult, const CombatRules& r);

// ---------------------------------------------------------------- regeneration (per second)

float hp_regen(int level, bool in_combat, bool sitting, const RegenRules& r);
float endurance_regen(int level, bool in_combat, bool sitting, const RegenRules& r);
float mana_regen(int level, bool in_combat, bool sitting, const RegenRules& r);

}  // namespace ef::game
