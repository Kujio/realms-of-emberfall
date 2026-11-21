// Pure, unit-tested rules for the Phase 4 class mechanics: positional attacks, dual wield and
// enhanced damage, stealth and feign death, charm breaks, pet stats, tracking directions, threat
// scaling, and armor/weapon proficiencies. No entities, networking, or I/O; every tunable comes
// from content/rules.json.
#pragma once

#include <string>
#include <string_view>

#include "game/content.hpp"
#include "game/math.hpp"
#include "game/rules.hpp"

namespace ef::game {

// ---------------------------------------------------------------- positioning

// Unit forward vector for a yaw (yaw 0 faces -Z; see zone_mobs.cpp yaw_toward).
Vec3 forward_of(float yaw);
// True when `attacker` stands inside the `arc_degrees` arc directly behind a target at `target`
// facing `target_yaw`. An attacker on top of the target counts as behind.
bool is_behind(const Vec3& target, float target_yaw, const Vec3& attacker, float arc_degrees);

// ---------------------------------------------------------------- melee skills

// Chance (0..1) that a dual-wielding attacker also swings its off hand this round.
float dual_wield_chance(int skill, const CombatRules& r);
// Melee damage multiplier from the enhanced damage skill (1 = no bonus).
float enhanced_damage_mult(int skill, const CombatRules& r);

// ---------------------------------------------------------------- stealth

// Chances (0..1). `level_gap` is how far the hardest relevant foe (or lock) is above the user.
float hide_chance(int skill, const StealthRules& r);
float feign_chance(int skill, int level_gap, const StealthRules& r);
float pick_lock_chance(int skill, int level_gap, const StealthRules& r);

// ---------------------------------------------------------------- charm and pets

// Chance (0..1) that a charm breaks at one check.
float charm_break_chance(int caster_level, int target_level, int charisma, const PetRules& r);
// The mob a pet template becomes at `level` (named for its owner).
MobDef make_pet_def(const PetDef& pet, int level, const std::string& owner_name);
// Damage a pet's own shield deals back to each attacker that hits it.
int pet_damage_shield(const PetDef& pet, int level);

enum class PetMode : std::uint8_t { Follow = 0, Guard = 1 };
// PetCommand.command values.
enum class PetOrder : std::uint8_t { Attack = 0, Follow = 1, Guard = 2, BackOff = 3, Dismiss = 4 };

// ---------------------------------------------------------------- threat

// Multiplier on threat a player generates: the class's threat_mult and any threat_pct buffs.
double threat_scale(float class_mult, float threat_pct);
// Threat left after dropping `pct` percent (clamped to 0..100).
double threat_after_drop(double threat, float pct);

// ---------------------------------------------------------------- tracking

// "north", "north-east", ... for the direction from (0,0) to (dx, dz). North is -Z, east is +X.
const char* compass_direction(float dx, float dz);

// ---------------------------------------------------------------- proficiencies

// Index into kArmorTypes, or -1 for an unknown weight.
int armor_rank(std::string_view armor);
// A class wears its own armor weight and anything lighter.
bool can_wear_armor(const ClassDef& c, std::string_view armor);
bool can_use_weapon(const ClassDef& c, std::string_view weapon_type);

}  // namespace ef::game
