#include "game/spells.hpp"

#include <algorithm>
#include <cmath>

namespace ef::game {

namespace {

float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

std::uint32_t to_ticks(float seconds, float ticks_per_second) {
  if (!(seconds > 0.0f)) return 0;
  return static_cast<std::uint32_t>(std::min(4.0e9, std::ceil(static_cast<double>(seconds) * static_cast<double>(ticks_per_second) - 1e-4)));
}

BuffStat resist_stat(ResistType t) {
  switch (t) {
    case ResistType::Magic: return BuffStat::ResistMagic;
    case ResistType::Fire: return BuffStat::ResistFire;
    case ResistType::Cold: return BuffStat::ResistCold;
    case ResistType::Poison: return BuffStat::ResistPoison;
    case ResistType::Disease: return BuffStat::ResistDisease;
    case ResistType::None: break;
  }
  return BuffStat::ResistAll;
}

}  // namespace

// ---------------------------------------------------------------- magnitudes

int effect_amount(const EffectDef& e, int caster_level) {
  float v = e.base + e.per_level * static_cast<float>(std::max(1, caster_level));
  if (e.max > 0.0f) v = std::min(v, e.max);
  return std::max(0, static_cast<int>(std::lround(v)));
}

int roll_spread(int amount, float spread, Rng& rng) {
  if (amount <= 0) return 0;
  const double f = 1.0 + static_cast<double>(spread) * (rng.uniform() * 2.0 - 1.0);
  return std::max(1, static_cast<int>(std::lround(static_cast<double>(amount) * f)));
}

ActiveBuff build_buff(const AbilityDef& a, int caster_level, EntityId caster, std::int64_t caster_char,
                      const std::string& caster_name, std::uint32_t now, float ticks_per_second, const SpellRules& r) {
  ActiveBuff b;
  b.ability_id = a.id;
  b.name = a.name;
  b.stack_group = a.stack_group.empty() ? a.id : a.stack_group;
  b.rank = a.rank;
  b.beneficial = a.beneficial();
  b.caster = caster;
  b.caster_char = caster_char;
  b.caster_name = caster_name;
  b.caster_level = caster_level;
  b.school = a.resist;
  b.applied_tick = now;
  b.expires_tick = now + to_ticks(a.lasting_seconds(), ticks_per_second);
  for (const auto& e : a.effects) {
    const std::uint32_t ends = now + to_ticks(e.duration, ticks_per_second);
    switch (e.type) {
      case EffectType::Buff:
      case EffectType::Debuff:
        b.mods.push_back({e.stat, e.value != 0.0f ? e.value : static_cast<float>(effect_amount(e, caster_level)), ends});
        break;
      case EffectType::Slow:
      case EffectType::Snare:
        b.mods.push_back({e.stat, -std::fabs(e.value), ends});
        break;
      case EffectType::Root: b.controls.push_back({kControlRoot, ends}); break;
      case EffectType::Mez: b.controls.push_back({kControlMez, ends}); break;
      case EffectType::Fear: b.controls.push_back({kControlFear, ends}); break;
      case EffectType::Charm: b.controls.push_back({kControlCharm, ends}); break;
      case EffectType::Illusion:
        b.illusion = e.ref;
        b.illusion_expires = ends;
        break;
      case EffectType::Dot:
      case EffectType::Hot:
      case EffectType::Drain: {
        const int amount = effect_amount(e, caster_level);
        const std::uint32_t interval = std::max<std::uint32_t>(1, to_ticks(e.tick > 0.0f ? e.tick : r.tick_seconds, ticks_per_second));
        const float drain = e.type == EffectType::Drain ? (e.value > 0.0f ? e.value / 100.0f : 1.0f) : 0.0f;
        b.periodics.push_back({e.type == EffectType::Hot ? amount : -amount, interval, now + interval, ends, drain});
        break;
      }
      default: break;
    }
  }
  return b;
}

std::uint8_t ActiveBuff::control() const {
  std::uint8_t c = 0;
  for (const auto& l : controls) c |= l.bit;
  return c;
}

bool ActiveBuff::harms_over_time() const {
  return std::any_of(periodics.begin(), periodics.end(), [](const PeriodicLine& l) { return l.amount < 0; });
}

bool has_lasting_part(const ActiveBuff& b) {
  return !b.mods.empty() || !b.controls.empty() || !b.periodics.empty() || !b.illusion.empty();
}

bool prune_lines(ActiveBuff& b, std::uint32_t now) {
  const std::size_t before = b.mods.size() + b.controls.size() + b.periodics.size();
  std::erase_if(b.mods, [&](const StatMod& l) { return l.expires_tick <= now; });
  std::erase_if(b.controls, [&](const ControlLine& l) { return l.expires_tick <= now; });
  std::erase_if(b.periodics, [&](const PeriodicLine& l) { return l.expires_tick <= now; });
  bool illusion_ended = false;
  if (!b.illusion.empty() && b.illusion_expires <= now) {
    b.illusion.clear();
    illusion_ended = true;
  }
  return illusion_ended || b.mods.size() + b.controls.size() + b.periodics.size() != before;
}

bool strip_control(ActiveBuff& b, std::uint8_t bit) {
  const std::size_t before = b.controls.size();
  std::erase_if(b.controls, [&](const ControlLine& l) { return (l.bit & bit) != 0; });
  return b.controls.size() != before;
}

// ---------------------------------------------------------------- resists

int base_mob_resist(const MobDef& m, ResistType type, const MobScaling& s) {
  if (type == ResistType::None) return 0;
  const int own = m.resists[static_cast<std::size_t>(type)];
  if (own >= 0) return own;
  return std::max(0, static_cast<int>(std::lround(s.resist_base + s.resist_per_level * static_cast<float>(m.level))));
}

float stat_total(const std::vector<ActiveBuff>& buffs, BuffStat stat) {
  float total = 0.0f;
  for (const auto& b : buffs)
    for (const auto& m : b.mods)
      if (m.stat == stat) total += m.value;
  return total;
}

int resist_bonus(const std::vector<ActiveBuff>& buffs, ResistType type) {
  if (type == ResistType::None) return 0;
  return static_cast<int>(std::lround(stat_total(buffs, resist_stat(type)) + stat_total(buffs, BuffStat::ResistAll)));
}

float resist_chance(ResistType type, int resist_value, int caster_level, int target_level, const SpellRules& r) {
  if (type == ResistType::None) return 0.0f;
  const float eff = static_cast<float>(std::max(0, resist_value));
  const float scale = r.resist_scale_base + r.resist_scale_per_level * static_cast<float>(std::max(1, caster_level));
  const float level_term = static_cast<float>(target_level - caster_level) * r.resist_per_level_diff;
  return clampf(eff / (eff + scale) + level_term, r.min_resist, r.max_resist);
}

ResistRoll roll_resist(float chance, bool partial, const SpellRules& r, Rng& rng) {
  ResistRoll out;
  if (chance <= 0.0f) return out;
  if (rng.chance(chance)) {
    out.resisted = true;
    out.fraction = 0.0f;
    return out;
  }
  // A landed damage spell is partly resisted with the same chance, keeping between
  // partial_resist_min and all of its damage.
  if (partial && rng.chance(chance)) {
    out.fraction = r.partial_resist_min + (1.0f - r.partial_resist_min) * static_cast<float>(rng.uniform());
  }
  return out;
}

// ---------------------------------------------------------------- casting

float concentration_chance(int channeling, int damage, int max_hp, const SpellRules& r) {
  const float skill = static_cast<float>(std::max(0, channeling));
  const float bonus = r.concentration_skill_bonus * skill / (skill + r.concentration_skill_scale);
  const float hurt = static_cast<float>(std::max(0, damage)) / static_cast<float>(std::max(1, max_hp));
  return clampf(r.concentration_base + bonus - r.concentration_damage_scale * hurt, r.min_concentration, r.max_concentration);
}

int gem_count(int level, const SpellRules& r) {
  int n = 0;
  for (int l : r.gem_unlock_levels)
    if (level >= l) ++n;
  return n;
}

CastState begin_cast(const AbilityDef& a, EntityId target, int gem, std::uint32_t now, std::uint32_t cast_ticks, const Vec3& pos) {
  CastState c;
  c.phase = CastPhase::Casting;
  c.ability = &a;
  c.target = target;
  c.gem = gem;
  c.started = now;
  c.finishes = now + cast_ticks;
  c.origin = pos;
  return c;
}

bool cast_complete(const CastState& c, std::uint32_t now) { return c.casting() && now >= c.finishes; }

bool moved_while_casting(const CastState& c, const Vec3& pos, float max_distance) {
  return c.casting() && horizontal_distance_sq(c.origin, pos) > max_distance * max_distance;
}

void end_cast(CastState& c) { c = CastState{}; }

bool line_of_sight(const Vec3& a, const Vec3& b, std::span<const Obstacle> obstacles) {
  const float dx = b.x - a.x;
  const float dz = b.z - a.z;
  const float len_sq = dx * dx + dz * dz;
  for (const auto& o : obstacles) {
    const float r2 = o.radius * o.radius;
    const auto inside = [&](const Vec3& p) { return (p.x - o.x) * (p.x - o.x) + (p.z - o.z) * (p.z - o.z) <= r2; };
    if (inside(a) || inside(b)) continue;  // standing against a pillar does not blind you
    float t = len_sq > 0.0f ? ((o.x - a.x) * dx + (o.z - a.z) * dz) / len_sq : 0.0f;
    t = clampf(t, 0.0f, 1.0f);
    const float cx = a.x + dx * t - o.x;
    const float cz = a.z + dz * t - o.z;
    if (cx * cx + cz * cz < r2) return false;
  }
  return true;
}

// ---------------------------------------------------------------- stacking

StackDecision decide_stacking(const std::vector<ActiveBuff>& existing, const ActiveBuff& incoming, int buff_slots, int debuff_slots) {
  const bool per_caster = !incoming.beneficial && incoming.ticks();
  for (std::size_t i = 0; i < existing.size(); ++i) {
    const ActiveBuff& b = existing[i];
    if (b.stack_group != incoming.stack_group || b.beneficial != incoming.beneficial) continue;
    if (per_caster && b.caster != incoming.caster) continue;
    if (incoming.rank >= b.rank) return {StackAction::Replace, i};
    return {StackAction::Blocked, i};
  }
  const auto same_polarity = static_cast<int>(
      std::count_if(existing.begin(), existing.end(), [&](const ActiveBuff& b) { return b.beneficial == incoming.beneficial; }));
  if (same_polarity >= (incoming.beneficial ? buff_slots : debuff_slots)) return {StackAction::NoSlot, 0};
  return {StackAction::Add, existing.size()};
}

std::vector<std::size_t> dispel_targets(const std::vector<ActiveBuff>& buffs, bool remove_beneficial, int count) {
  std::vector<std::size_t> out;
  for (std::size_t i = buffs.size(); i-- > 0 && static_cast<int>(out.size()) < count;)
    if (buffs[i].beneficial == remove_beneficial) out.push_back(i);
  return out;
}

std::uint8_t control_bits(const std::vector<ActiveBuff>& buffs) {
  std::uint8_t c = 0;
  for (const auto& b : buffs) c |= b.control();
  return c;
}

float move_speed_multiplier(const std::vector<ActiveBuff>& buffs) {
  return clampf(1.0f + stat_total(buffs, BuffStat::MoveSpeedPct) / 100.0f, 0.1f, 2.0f);
}

float attack_delay_multiplier(const std::vector<ActiveBuff>& buffs) {
  // +30% attack speed = 1/1.3 of the delay; -30% (a slow) = 1/0.7.
  return 1.0f / clampf(1.0f + stat_total(buffs, BuffStat::AttackSpeedPct) / 100.0f, 0.1f, 3.0f);
}

// ---------------------------------------------------------------- misc

std::uint64_t rez_xp(std::uint64_t xp_lost, float pct) {
  const double p = static_cast<double>(clampf(pct, 0.0f, 100.0f)) / 100.0;
  return static_cast<std::uint64_t>(std::floor(static_cast<double>(xp_lost) * p));
}

double buff_threat(int spell_level, const SpellRules& r) {
  return static_cast<double>(r.buff_threat_per_level) * static_cast<double>(std::max(1, spell_level));
}

float meditate_multiplier(int meditate, int cap, const SpellRules& r) {
  if (meditate <= 0 || cap <= 0) return 1.0f;
  return 1.0f + r.meditate_bonus * static_cast<float>(std::min(meditate, cap)) / static_cast<float>(cap);
}

}  // namespace ef::game
