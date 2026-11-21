// The spell engine's pure parts: effect magnitudes, resist rolls, concentration, stacking,
// the cast state machine, line of sight, and buff bookkeeping. No entities, networking, or I/O;
// the zone (zone_spells.cpp) applies these to players and mobs. Every tunable is in
// content/rules.json "spells".
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/content.hpp"
#include "game/math.hpp"
#include "game/rules.hpp"
#include "game/spatial_hash.hpp"

namespace ef::game {

// ActiveBuff::control bits.
inline constexpr std::uint8_t kControlRoot = 1;
inline constexpr std::uint8_t kControlMez = 2;
inline constexpr std::uint8_t kControlFear = 4;

inline constexpr std::uint32_t kNever = 0xFFFFFFFFu;

// Each lasting line of a spell keeps its own expiry (a 3 s root and a 9 s DoT in one spell end
// at 3 s and 9 s), and each periodic line its own amount and timer.
struct StatMod {
  BuffStat stat = BuffStat::Ac;
  float value = 0.0f;
  std::uint32_t expires_tick = kNever;
};
struct ControlLine {
  std::uint8_t bit = 0;  // kControlRoot, kControlMez, or kControlFear
  std::uint32_t expires_tick = kNever;
};
struct PeriodicLine {
  int amount = 0;  // per tick: > 0 heals (hot), < 0 damages (dot)
  std::uint32_t interval = 1;
  std::uint32_t next_tick = 0;
  std::uint32_t expires_tick = kNever;
};

// One lasting effect on an entity: every lasting line of one spell or ability cast by one caster.
// Occupies one buff slot (beneficial) or one debuff slot (detrimental) until its last line ends.
struct ActiveBuff {
  std::string ability_id;
  std::string name;
  std::string stack_group;
  int rank = 1;
  bool beneficial = true;
  EntityId caster = 0;
  std::int64_t caster_char = 0;  // 0 for non-player casters
  std::string caster_name;
  int caster_level = 1;
  ResistType school = ResistType::None;
  std::uint32_t applied_tick = 0;
  std::uint32_t expires_tick = 0;  // when the last line ends
  std::vector<StatMod> mods;
  std::vector<ControlLine> controls;
  std::vector<PeriodicLine> periodics;

  std::uint8_t control() const;  // the control bits of the lines still present
  bool ticks() const { return !periodics.empty(); }
  bool harms_over_time() const;  // has a damaging periodic line
};

// ---------------------------------------------------------------- magnitudes

// base + per_level * level, capped at max (when max > 0), rounded, never negative.
int effect_amount(const EffectDef& e, int caster_level);
// amount * (1 +- spread), at least 1 when amount > 0.
int roll_spread(int amount, float spread, Rng& rng);

// Builds the lasting part of a spell (nullopt-like: control == 0, no mods, no periodic) for
// one target. `ticks_per_second` converts durations (20 at the default tick rate).
ActiveBuff build_buff(const AbilityDef& a, int caster_level, EntityId caster, std::int64_t caster_char,
                      const std::string& caster_name, std::uint32_t now, float ticks_per_second, const SpellRules& r);
bool has_lasting_part(const ActiveBuff& b);
// Removes lines that have ended by `now` (after their final tick was taken); returns true if any did.
bool prune_lines(ActiveBuff& b, std::uint32_t now);
// Removes the control lines with `bit` (a broken mez or root); returns true if any were removed.
bool strip_control(ActiveBuff& b, std::uint8_t bit);

// ---------------------------------------------------------------- resists

// The target's resist value for one type: the mob's own value (or the level default) plus buffs.
int base_mob_resist(const MobDef& m, ResistType type, const MobScaling& s);
float stat_total(const std::vector<ActiveBuff>& buffs, BuffStat stat);
int resist_bonus(const std::vector<ActiveBuff>& buffs, ResistType type);

// Chance (0..1) that a spell of this resist type is resisted. ResistType::None is never resisted.
float resist_chance(ResistType type, int resist_value, int caster_level, int target_level, const SpellRules& r);

struct ResistRoll {
  bool resisted = false;
  float fraction = 1.0f;  // damage kept: 1 = full, less on a partial resist
};
// One roll. With `partial`, a spell that lands may still be partly resisted (damage spells).
ResistRoll roll_resist(float chance, bool partial, const SpellRules& r, Rng& rng);

// ---------------------------------------------------------------- casting

// Chance (0..1) to keep casting after taking `damage` while casting.
float concentration_chance(int channeling, int damage, int max_hp, const SpellRules& r);
// Number of usable spell gems at a level.
int gem_count(int level, const SpellRules& r);

enum class CastPhase : std::uint8_t { Idle, Casting };
// CastEnd.result values.
enum class CastResult : std::uint8_t { Completed = 0, Interrupted = 1, Cancelled = 2, Failed = 3 };

struct CastState {
  CastPhase phase = CastPhase::Idle;
  const AbilityDef* ability = nullptr;
  EntityId target = 0;
  int gem = -1;  // -1 for abilities cast without a gem
  std::uint32_t started = 0;
  std::uint32_t finishes = 0;
  Vec3 origin;  // where the caster stood when the cast began
  bool casting() const { return phase == CastPhase::Casting; }
};

// Idle -> Casting.
CastState begin_cast(const AbilityDef& a, EntityId target, int gem, std::uint32_t now, std::uint32_t cast_ticks, const Vec3& pos);
// True once the cast time has elapsed (the zone then resolves the spell and calls end_cast).
bool cast_complete(const CastState& c, std::uint32_t now);
// Moving more than `max_distance` from where the cast began interrupts it.
bool moved_while_casting(const CastState& c, const Vec3& pos, float max_distance);
// Casting -> Idle.
void end_cast(CastState& c);

// Segment a-b against every obstacle (horizontal plane). Endpoints inside an obstacle still see out.
bool line_of_sight(const Vec3& a, const Vec3& b, std::span<const Obstacle> obstacles);

// ---------------------------------------------------------------- stacking

enum class StackAction : std::uint8_t { Add, Replace, Blocked, NoSlot };
struct StackDecision {
  StackAction action = StackAction::Add;
  std::size_t index = 0;  // Replace / Blocked: the existing effect involved
};
// Stacking rules:
//  - Same stack group (for detrimental periodic effects: same group AND same caster, so several
//    casters' DoTs stack): an equal or higher rank replaces and refreshes it, a lower rank is blocked.
//  - Otherwise it needs a free slot of its polarity (buff_slots beneficial, debuff_slots detrimental).
StackDecision decide_stacking(const std::vector<ActiveBuff>& existing, const ActiveBuff& incoming, int buff_slots, int debuff_slots);

// Indices of up to `count` effects to dispel, newest first: beneficial ones when
// `remove_beneficial`, detrimental ones otherwise.
std::vector<std::size_t> dispel_targets(const std::vector<ActiveBuff>& buffs, bool remove_beneficial, int count);

std::uint8_t control_bits(const std::vector<ActiveBuff>& buffs);
// Movement and attack-speed multipliers from snares/haste and slows (clamped).
float move_speed_multiplier(const std::vector<ActiveBuff>& buffs);
float attack_delay_multiplier(const std::vector<ActiveBuff>& buffs);

// ---------------------------------------------------------------- misc

// Experience a resurrection gives back.
std::uint64_t rez_xp(std::uint64_t xp_lost, float pct);
// Threat for buffing someone a mob is fighting.
double buff_threat(int spell_level, const SpellRules& r);
// Sitting mana regen multiplier from the meditate skill.
float meditate_multiplier(int meditate, int cap, const SpellRules& r);

}  // namespace ef::game
