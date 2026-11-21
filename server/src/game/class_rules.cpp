#include "game/class_rules.hpp"

#include <algorithm>
#include <cmath>

namespace ef::game {

namespace {

float clamp01(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

}  // namespace

// ---------------------------------------------------------------- positioning

Vec3 forward_of(float yaw) { return {-std::sin(yaw), 0.0f, -std::cos(yaw)}; }

bool is_behind(const Vec3& target, float target_yaw, const Vec3& attacker, float arc_degrees) {
  const float dx = attacker.x - target.x;
  const float dz = attacker.z - target.z;
  const float d = std::sqrt(dx * dx + dz * dz);
  if (d < 1e-3f) return true;
  const Vec3 f = forward_of(target_yaw);
  // The angle between "away from the target's face" and "toward the attacker" must fit in half the arc.
  const float cos_to_back = -(f.x * dx + f.z * dz) / d;
  const float half = std::clamp(arc_degrees, 0.0f, 360.0f) * 0.5f * kPi / 180.0f;
  return cos_to_back >= std::cos(half) - 1e-5f;
}

// ---------------------------------------------------------------- melee skills

float dual_wield_chance(int skill, const CombatRules& r) {
  if (skill <= 0 || r.dual_wield_divisor <= 0.0f) return 0.0f;
  return clamp01(static_cast<float>(skill) / r.dual_wield_divisor, 0.0f, r.max_dual_wield_chance);
}

float enhanced_damage_mult(int skill, const CombatRules& r) {
  if (skill <= 0) return 1.0f;
  return 1.0f + r.enhanced_damage_max * std::min(1.0f, static_cast<float>(skill) / 250.0f);
}

// ---------------------------------------------------------------- stealth

float hide_chance(int skill, const StealthRules& r) {
  return clamp01(r.hide_base + r.hide_per_skill * static_cast<float>(std::max(0, skill)), r.min_chance, r.max_chance);
}

float feign_chance(int skill, int level_gap, const StealthRules& r) {
  const float v = r.feign_base + r.feign_per_skill * static_cast<float>(std::max(0, skill)) - r.feign_per_level * static_cast<float>(std::max(0, level_gap));
  return clamp01(v, r.min_chance, r.max_chance);
}

float pick_lock_chance(int skill, int level_gap, const StealthRules& r) {
  const float v = r.pick_base + r.pick_per_skill * static_cast<float>(std::max(0, skill)) - r.pick_per_level * static_cast<float>(std::max(0, level_gap));
  return clamp01(v, r.min_chance, r.max_chance);
}

// ---------------------------------------------------------------- charm and pets

float charm_break_chance(int caster_level, int target_level, int charisma, const PetRules& r) {
  const float v = r.charm_break_base + r.charm_break_per_level * static_cast<float>(target_level - caster_level) -
                  r.charm_break_cha_scale * static_cast<float>(charisma - 75);
  return clamp01(v, r.min_charm_break, r.max_charm_break);
}

MobDef make_pet_def(const PetDef& pet, int level, const std::string& owner_name) {
  MobDef m;
  m.id = pet.id;
  std::string name = owner_name.empty() ? pet.name : owner_name + "'s " + pet.name;
  if (name.size() > 48) name.resize(48);
  m.name = std::move(name);
  m.level = std::max(1, level);
  m.model = pet.model;
  m.scale = pet.scale;
  m.hp_mult = pet.hp_mult;
  m.damage_mult = pet.damage_mult;
  m.attack_delay = pet.attack_delay;
  m.run_speed = pet.run_speed;
  m.aggro_radius = 0.0f;  // pets never notice anyone on their own
  m.assist = false;
  m.faction = "pet";
  m.leash_radius = 1.0e6f;  // pets follow their owner, not a spawn point
  m.respawn_seconds = 1.0f;
  m.stun_immune = false;
  m.charm_immune = true;
  m.body = pet.body;
  m.resists = pet.resists;
  return m;
}

int pet_damage_shield(const PetDef& pet, int level) {
  return std::max(0, static_cast<int>(std::lround(pet.damage_shield_per_level * static_cast<float>(std::max(1, level)))));
}

// ---------------------------------------------------------------- threat

double threat_scale(float class_mult, float threat_pct) {
  return std::max(0.0, static_cast<double>(class_mult) * (1.0 + static_cast<double>(threat_pct) / 100.0));
}

double threat_after_drop(double threat, float pct) {
  return std::max(0.0, threat * (1.0 - static_cast<double>(std::clamp(pct, 0.0f, 100.0f)) / 100.0));
}

// ---------------------------------------------------------------- tracking

const char* compass_direction(float dx, float dz) {
  static const char* kNames[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
  if (std::fabs(dx) < 1e-4f && std::fabs(dz) < 1e-4f) return "here";
  // Bearing clockwise from north (-Z) toward east (+X).
  float bearing = std::atan2(dx, -dz);
  if (bearing < 0.0f) bearing += 2.0f * kPi;
  const int sector = static_cast<int>(std::floor(bearing / (kPi / 4.0f) + 0.5f)) % 8;
  return kNames[sector];
}

// ---------------------------------------------------------------- proficiencies

int armor_rank(std::string_view armor) {
  for (std::size_t i = 0; i < kArmorTypes.size(); ++i)
    if (armor == kArmorTypes[i]) return static_cast<int>(i);
  return -1;
}

bool can_wear_armor(const ClassDef& c, std::string_view armor) {
  const int want = armor_rank(armor);
  return want >= 0 && want <= armor_rank(c.armor);
}

bool can_use_weapon(const ClassDef& c, std::string_view weapon_type) {
  return std::find(c.weapons.begin(), c.weapons.end(), weapon_type) != c.weapons.end();
}

}  // namespace ef::game
