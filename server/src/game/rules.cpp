#include "game/rules.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ef::game {

namespace {

std::uint64_t splitmix(std::uint64_t& x) {
  std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

int round_int(double v) {
  if (!std::isfinite(v)) return 0;
  v = std::clamp(v, -2.0e9, 2.0e9);
  return static_cast<int>(std::lround(v));
}

int round_int(float v) { return round_int(static_cast<double>(v)); }

// Regen applies the out-of-combat multiplier, then the sitting one (sitting only counts out of combat).
float regen(float base, float per_level, int level, bool in_combat, bool sitting, const RegenRules& r) {
  float v = base + per_level * static_cast<float>(level);
  if (!in_combat) {
    v *= r.out_of_combat_multiplier;
    if (sitting) v *= r.sitting_multiplier;
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------- RNG

Rng::Rng(std::uint64_t seed) {
  for (auto& s : s_) s = splitmix(seed);
}

std::uint64_t Rng::next() {
  const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
  const std::uint64_t t = s_[1] << 17;
  s_[2] ^= s_[0];
  s_[3] ^= s_[1];
  s_[1] ^= s_[2];
  s_[0] ^= s_[3];
  s_[2] ^= t;
  s_[3] = rotl(s_[3], 45);
  return result;
}

double Rng::uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

int Rng::range(int lo, int hi) {
  if (hi <= lo) return lo;
  const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(hi) - lo + 1);
  return static_cast<int>(lo + static_cast<std::int64_t>(next() % span));
}

// ---------------------------------------------------------------- stats

StatBlock combine_stats(const StatBlock& race, const StatBlock& class_bonus) {
  const StatBlock& b = class_bonus;
  auto c = [](int a, int x) { return std::clamp(a + x, 1, 255); };
  return {c(race.str, b.str), c(race.sta, b.sta), c(race.agi, b.agi), c(race.dex, b.dex),
          c(race.wis, b.wis), c(race.intel, b.intel), c(race.cha, b.cha)};
}

DerivedStats derive_stats(const RaceDef& race, const ClassDef& cls, int level) {
  const auto& k = cls.combat;
  const float L = static_cast<float>(std::max(1, level));
  DerivedStats d;
  d.stats = combine_stats(race.stats, k.stats);
  // Stamina above/below 75 scales HP by up to roughly +-25%; intelligence/wisdom do the same for mana.
  const float sta_mult = 1.0f + static_cast<float>(d.stats.sta - 75) / 300.0f;
  const float mind = static_cast<float>(std::max(d.stats.intel, d.stats.wis));
  const float mana_mult = 1.0f + (mind - 75.0f) / 300.0f;
  d.max_hp = std::max(1, round_int((k.hp_base + k.hp_per_level * L) * sta_mult));
  d.max_mana = k.mana_per_level > 0.0f ? std::max(0, round_int((k.mana_base + k.mana_per_level * L) * mana_mult)) : 0;
  d.max_endurance = std::max(0, round_int(k.endurance_base + k.endurance_per_level * L + static_cast<float>(d.stats.sta - 75) / 5.0f));
  d.ac = std::max(0, round_int(k.ac_per_level * L + static_cast<float>(d.stats.agi - 75) / 3.0f));
  d.weapon_damage = std::max(1.0f, k.weapon_damage_base + k.weapon_damage_per_level * L);
  d.weapon_delay = k.weapon_delay;
  d.crit_chance = k.crit_chance + static_cast<float>(std::max(0, d.stats.dex - 75)) / 2000.0f;
  return d;
}

int skill_cap(const SkillCap& cap, int level) {
  if (level < cap.level) return 0;
  return std::min(cap.max, round_int(cap.per_level * static_cast<float>(level) + 5.0f));
}

// ---------------------------------------------------------------- consider and experience

Con con_color(int player_level, int mob_level, const ConRules& rules) {
  const int diff = mob_level - player_level;
  if (diff >= 3) return Con::Red;
  if (diff >= 1) return Con::Yellow;
  if (diff == 0) return Con::White;
  const int gap = -diff;
  const int blue_span = 1 + player_level / std::max(1, rules.blue_divisor);
  const int green_span = 1 + player_level / std::max(1, rules.green_divisor);
  if (gap <= blue_span) return Con::Blue;
  if (gap <= blue_span + green_span) return Con::Green;
  return Con::Grey;
}

const char* con_name(Con c) {
  switch (c) {
    case Con::Grey: return "grey";
    case Con::Green: return "green";
    case Con::Blue: return "blue";
    case Con::White: return "white";
    case Con::Yellow: return "yellow";
    case Con::Red: return "red";
  }
  return "white";
}

std::uint64_t xp_to_next(int level, const XpRules& r) {
  const double L = static_cast<double>(std::max(1, level));
  const double v = r.base * std::pow(L, r.level_exponent) * std::pow(r.growth, L - 1.0);
  // Content validation keeps real curves far below this; the clamp only guards the conversion.
  if (!std::isfinite(v) || v >= kMaxXp) return static_cast<std::uint64_t>(kMaxXp);
  return static_cast<std::uint64_t>(std::max(1.0, std::round(v)));
}

std::uint64_t total_xp_for_level(int level, const XpRules& r) {
  std::uint64_t total = 0;
  for (int l = 1; l < level; ++l) total = std::min(total + xp_to_next(l, r), static_cast<std::uint64_t>(kMaxXp));
  return total;
}

int level_for_xp(std::uint64_t total_xp, int max_level, const XpRules& r) {
  int level = 1;
  std::uint64_t at = 0;
  while (level < max_level) {
    const std::uint64_t need = xp_to_next(level, r);
    if (total_xp < at + need) break;
    at += need;
    ++level;
  }
  return level;
}

std::uint64_t kill_xp(int player_level, int mob_level, const XpRules& xp, const ConRules& con) {
  const Con c = con_color(player_level, mob_level, con);
  const double mult = xp.con_multipliers[static_cast<std::size_t>(c)];
  if (mult <= 0.0) return 0;
  const double L = static_cast<double>(std::max(1, mob_level));
  return static_cast<std::uint64_t>(std::max(1.0, std::round(xp.kill_base * std::pow(L, xp.level_exponent) * mult)));
}

std::vector<std::uint64_t> split_group_xp(std::uint64_t base_xp, const std::vector<int>& member_levels, const XpRules& r) {
  std::vector<std::uint64_t> out(member_levels.size(), 0);
  if (member_levels.empty() || base_xp == 0) return out;
  const double bonus = 1.0 + r.group_bonus_per_member * static_cast<double>(member_levels.size() - 1);
  double level_sum = 0.0;
  for (int l : member_levels) level_sum += static_cast<double>(std::max(1, l));
  const double pool = static_cast<double>(base_xp) * bonus;
  for (std::size_t i = 0; i < member_levels.size(); ++i) {
    out[i] = static_cast<std::uint64_t>(std::floor(pool * static_cast<double>(std::max(1, member_levels[i])) / level_sum));
  }
  return out;
}

int group_level_floor(int highest, int level_window, float level_fraction) {
  const int by_fraction = static_cast<int>(std::floor(static_cast<float>(std::max(1, highest)) * std::max(0.0f, level_fraction)));
  return std::max(1, highest - std::max(std::max(0, level_window), by_fraction));
}

std::vector<GroupXpShare> group_xp_shares(int mob_level, const std::vector<GroupXpMember>& members, const XpRules& xp, const ConRules& con,
                                          int level_window, float level_fraction) {
  std::vector<GroupXpShare> out(members.size());
  int highest = 0;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (!members[i].in_range) out[i].out_of_range = true;
    else highest = std::max(highest, std::max(1, members[i].level));
  }
  if (highest == 0) return out;
  const int floor_level = group_level_floor(highest, level_window, level_fraction);
  std::vector<int> levels;
  std::vector<std::size_t> who;
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (!members[i].in_range) continue;
    if (std::max(1, members[i].level) < floor_level) {
      out[i].too_low = true;
      continue;
    }
    levels.push_back(std::max(1, members[i].level));
    who.push_back(i);
  }
  const std::uint64_t base = kill_xp(highest, mob_level, xp, con);
  const auto shares = split_group_xp(base, levels, xp);
  for (std::size_t k = 0; k < who.size(); ++k) out[who[k]].xp = shares[k];
  return out;
}

std::uint64_t death_xp_loss(std::uint64_t total_xp, int level, const DeathRules& death, const XpRules& xp) {
  if (level < death.min_level_for_xp_loss) return 0;
  const auto loss = static_cast<std::uint64_t>(std::floor(static_cast<double>(xp_to_next(level, xp)) * static_cast<double>(death.xp_loss_fraction)));
  return std::min(loss, total_xp);
}

// ---------------------------------------------------------------- combat math

float hit_chance(int offense, int defense, int attacker_level, int defender_level, const CombatRules& r) {
  const float p = r.base_hit_chance + static_cast<float>(offense - defense) * r.hit_per_skill_point +
                  static_cast<float>(attacker_level - defender_level) * r.hit_per_level;
  return clampf(p, r.min_hit_chance, r.max_hit_chance);
}

float mitigation(int ac, int attacker_level, const CombatRules& r) {
  if (ac <= 0) return 0.0f;
  const float a = static_cast<float>(ac);
  const float m = a / (a + r.mitigation_base + r.mitigation_per_level * static_cast<float>(std::max(1, attacker_level)));
  return clampf(m, 0.0f, r.max_mitigation);
}

float avoid_chance(int skill, int attacker_level, const CombatRules& r) {
  if (skill <= 0) return 0.0f;
  const float denom = r.avoid_skill_divisor * (r.avoid_level_scale * static_cast<float>(std::max(1, attacker_level)) + r.avoid_base);
  return clampf(static_cast<float>(skill) / denom, 0.0f, r.max_avoid_chance);
}

float riposte_chance(int skill, int attacker_level, const CombatRules& r) {
  return avoid_chance(skill, attacker_level, r) * r.riposte_scale;
}

float double_attack_chance(int skill, const CombatRules& r) {
  if (skill <= 0) return 0.0f;
  return clampf(static_cast<float>(skill) / r.double_attack_divisor, 0.0f, 0.9f);
}

bool stun_lands(int attacker_level, int target_level, bool target_immune, const CombatRules& r) {
  return !target_immune && target_level <= attacker_level + r.stun_level_margin;
}

float taunt_chance(int taunt_skill, int skill_cap_value, int attacker_level, int target_level, const CombatRules& r) {
  const float skill_frac = skill_cap_value > 0 ? static_cast<float>(taunt_skill) / static_cast<float>(skill_cap_value) : 0.0f;
  const float p = r.taunt_base_chance + 0.2f * (skill_frac - 0.5f) - static_cast<float>(target_level - attacker_level) * r.taunt_per_level;
  return clampf(p, 0.05f, 0.98f);
}

float skill_up_chance(int value, int cap, const CombatRules& r) {
  if (cap <= 0 || value >= cap) return 0.0f;
  return clampf(r.skill_up_base + r.skill_up_scale * static_cast<float>(cap - value) / static_cast<float>(cap), 0.0f, 1.0f);
}

HitRange player_hit_range(const DerivedStats& d, int level) {
  // Strength adds to both ends; the spread is 1..2x the weapon's damage plus a small level bonus.
  const float str_bonus = static_cast<float>(std::max(0, d.stats.str - 75)) / 10.0f;
  const float level_bonus = static_cast<float>(std::max(1, level)) / 4.0f;
  const int lo = std::max(1, round_int(d.weapon_damage * 0.5f + str_bonus * 0.5f));
  const int hi = std::max(lo, round_int(d.weapon_damage * 1.5f + level_bonus + str_bonus));
  return {lo, hi};
}

int ability_damage(const EffectDef& e, int level, Rng& rng) {
  const float base = e.base + e.per_level * static_cast<float>(std::max(1, level));
  const int lo = std::max(1, round_int(base * 0.8f));
  const int hi = std::max(lo, round_int(base * 1.2f));
  return rng.range(lo, hi);
}

MobStats mob_stats(const MobDef& def, const MobScaling& s) {
  const float L = static_cast<float>(std::max(1, def.level));
  MobStats m;
  m.max_hp = std::max(1, round_int((s.hp_base + s.hp_per_level * L + s.hp_quad * L * L) * def.hp_mult));
  const int max_hit = std::max(1, round_int((s.max_hit_base + s.max_hit_per_level * L + s.max_hit_quad * L * L) * def.damage_mult));
  m.hit = {std::max(1, max_hit / 2), max_hit};
  m.ac = std::max(0, round_int(s.ac_per_level * L));
  m.offense = round_int(s.skill_per_level * L);
  m.defense = round_int(s.skill_per_level * L);
  m.avoid_skill = round_int(s.avoid_skill_per_level * L);
  return m;
}

int mitigate(int raw, int ac, int attacker_level, float damage_taken_mult, const CombatRules& r) {
  const float after = static_cast<float>(raw) * (1.0f - mitigation(ac, attacker_level, r)) * std::max(0.0f, damage_taken_mult);
  return std::max(1, round_int(after));
}

SwingOutcome resolve_swing(const AttackerView& a, const DefenderView& d, const CombatRules& r, Rng& rng) {
  SwingOutcome out;
  if (!rng.chance(hit_chance(a.offense, d.defense, a.level, d.level, r))) return out;  // Miss
  if (d.riposte > 0 && rng.chance(riposte_chance(d.riposte, a.level, r))) {
    out.result = SwingResult::Riposte;
    return out;
  }
  if (d.parry > 0 && rng.chance(avoid_chance(d.parry, a.level, r))) {
    out.result = SwingResult::Parry;
    return out;
  }
  if (d.dodge > 0 && rng.chance(avoid_chance(d.dodge, a.level, r))) {
    out.result = SwingResult::Dodge;
    return out;
  }
  int raw = rng.range(a.range.min, a.range.max);
  raw = std::max(1, round_int(static_cast<float>(raw) * std::max(0.0f, a.damage_mult)));
  out.result = SwingResult::Hit;
  if (rng.chance(a.crit_chance)) {
    out.result = SwingResult::Crit;
    raw = std::max(raw + 1, round_int(static_cast<float>(raw) * r.crit_multiplier));
  }
  out.damage = mitigate(raw, d.ac, a.level, d.damage_taken_mult, r);
  return out;
}

// ---------------------------------------------------------------- regeneration

float hp_regen(int level, bool in_combat, bool sitting, const RegenRules& r) {
  return regen(r.hp_base, r.hp_per_level, level, in_combat, sitting, r);
}

float endurance_regen(int level, bool in_combat, bool sitting, const RegenRules& r) {
  return regen(r.endurance_base, r.endurance_per_level, level, in_combat, sitting, r);
}

float mana_regen(int level, bool in_combat, bool sitting, const RegenRules& r) {
  return regen(r.mana_base, r.mana_per_level, level, in_combat, sitting, r);
}

}  // namespace ef::game
