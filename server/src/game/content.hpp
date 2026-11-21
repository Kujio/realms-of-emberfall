// Data-driven game content loaded from /content JSON. Validated at startup (fail fast).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace ef::game {

struct SpawnPoint {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
};

// The seven primary attributes.
struct StatBlock {
  int str = 0;
  int sta = 0;
  int agi = 0;
  int dex = 0;
  int wis = 0;
  int intel = 0;
  int cha = 0;
};

// Cap for a skill: min(max, per_level * level + 5) once level >= unlock level.
struct SkillCap {
  int level = 1;
  float per_level = 5.0f;
  int max = 250;
};

struct RaceDef {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> allowed_classes;
  std::vector<std::string> cities;
  StatBlock stats{75, 75, 75, 75, 75, 75, 75};
};

// Per-class numbers for derived stats and the placeholder weapon used until items exist.
struct ClassCombat {
  float hp_base = 20.0f;
  float hp_per_level = 15.0f;
  float mana_base = 0.0f;
  float mana_per_level = 0.0f;
  float endurance_base = 15.0f;
  float endurance_per_level = 4.0f;
  float ac_per_level = 2.0f;
  float weapon_damage_base = 4.0f;
  float weapon_damage_per_level = 0.4f;
  float weapon_delay = 3.0f;
  float crit_chance = 0.0f;
  StatBlock stats;  // bonuses added to the race's base stats
};

struct ClassDef {
  std::string id;
  std::string name;
  std::string archetype;
  std::string description;
  ClassCombat combat;
  std::vector<std::pair<std::string, SkillCap>> skills;  // skills.json defaults merged with overrides

  const SkillCap* skill(std::string_view skill_id) const;
};

struct CityDef {
  std::string id;
  std::string name;
  std::string description;
  std::string zone;
  SpawnPoint spawn;
};

struct MobSpawn {
  std::string mob;
  float x = 0.0f;
  float z = 0.0f;
  int count = 1;
  float radius = 0.0f;
};

struct ZoneDef {
  std::string id;
  std::string name;
  std::string description;
  float min_x = 0.0f;
  float min_z = 0.0f;
  float max_x = 0.0f;
  float max_z = 0.0f;
  float ground_y = 0.0f;
  SpawnPoint safe_spawn;
  float interest_radius = 80.0f;
  float say_radius = 60.0f;
  std::vector<MobSpawn> spawns;
};

struct SkillDef {
  std::string id;
  std::string name;
  std::string kind;
  std::string description;
};

enum class EffectType : std::uint8_t { Damage, Stun, Threat, Taunt, Buff, HealPct, RestoreEndurancePct, AreaThreat };
enum class BuffStat : std::uint8_t { DamageTakenPct, MeleeDamagePct, CritChancePct };
enum class TargetType : std::uint8_t { Enemy, Self };
enum class Resource : std::uint8_t { None, Endurance, Mana };

struct EffectDef {
  EffectType type = EffectType::Damage;
  float base = 0.0f;       // damage/threat amount at level 0
  float per_level = 0.0f;  // added per caster level
  float value = 0.0f;      // buff magnitude or percentage
  float duration = 0.0f;   // seconds (stun, buff)
  float radius = 0.0f;     // area effects
  BuffStat stat = BuffStat::DamageTakenPct;
};

struct AbilityDef {
  std::string id;
  std::string name;
  std::string class_id;
  int level = 1;
  std::string type;  // display category: nuke, heal, cc, utility, discipline, passive, ...
  Resource resource = Resource::None;
  int cost = 0;
  float cast_time = 0.0f;
  float cooldown = 0.0f;
  bool gcd = true;
  float range = 0.0f;
  TargetType target = TargetType::Enemy;
  std::string resist = "none";
  std::string skill;  // skill that improves on use (actives) or that the passive represents
  std::string description;
  std::vector<EffectDef> effects;

  bool passive() const { return type == "passive"; }
};

struct ItemDef {
  std::string id;
  std::string name;
  std::string kind;
  int stack = 1;
  int value = 0;  // copper
  std::string rarity;
  std::string description;
};

struct LootDrop {
  std::string item;
  float chance = 0.0f;
  int min = 1;
  int max = 1;
};

struct LootTable {
  std::string id;
  int coin_min = 0;
  int coin_max = 0;
  int max_drops = 1;
  std::vector<LootDrop> drops;
};

struct MobDef {
  std::string id;
  std::string name;
  int level = 1;
  std::string model;
  float scale = 1.0f;
  float hp_mult = 1.0f;
  float damage_mult = 1.0f;
  float attack_delay = 2.5f;
  float run_speed = 6.0f;
  float aggro_radius = 0.0f;
  bool assist = false;
  float assist_radius = 0.0f;
  std::string faction;
  float flee_hp_pct = 0.0f;
  float wander_radius = 0.0f;
  float leash_radius = 80.0f;
  float respawn_seconds = 60.0f;
  bool stun_immune = false;
  std::string loot;
};

struct MovementRules {
  float run_speed = 7.0f;            // metres/second
  std::uint32_t input_budget_max = 4;  // max banked movement inputs (jitter absorption)
  float desync_tolerance = 2.0f;     // metres between client prediction and server before a hard correction
  float teleport_tolerance = 1.5f;   // multiplier on max legal per-input displacement
};

// Consider colors, ordered from trivial to dangerous.
enum class Con : std::uint8_t { Grey = 0, Green, Blue, White, Yellow, Red };
inline constexpr std::size_t kConCount = 6;

struct XpRules {
  double base = 10.0;
  double level_exponent = 2.0;
  double growth = 1.0718;
  double kill_base = 1.5;
  std::array<double, kConCount> con_multipliers{0.0, 0.5, 0.85, 1.0, 1.15, 1.3};
  double group_bonus_per_member = 0.1;
};

struct ConRules {
  int blue_divisor = 7;
  int green_divisor = 6;
};

struct RegenRules {
  float hp_base = 0.5f;
  float hp_per_level = 0.06f;
  float endurance_base = 1.0f;
  float endurance_per_level = 0.05f;
  float mana_base = 0.5f;
  float mana_per_level = 0.05f;
  float out_of_combat_multiplier = 3.0f;
  float sitting_multiplier = 2.0f;
};

struct CombatRules {
  float gcd_seconds = 1.5f;
  float melee_range = 4.0f;
  float loot_range = 10.0f;
  float event_radius = 60.0f;
  float in_combat_seconds = 6.0f;
  float base_hit_chance = 0.72f;
  float hit_per_skill_point = 0.0015f;
  float hit_per_level = 0.015f;
  float min_hit_chance = 0.05f;
  float max_hit_chance = 0.95f;
  float crit_multiplier = 2.0f;
  float mitigation_base = 50.0f;
  float mitigation_per_level = 25.0f;
  float max_mitigation = 0.75f;
  float avoid_skill_divisor = 10.0f;
  float avoid_level_scale = 5.0f;
  float avoid_base = 50.0f;
  float max_avoid_chance = 0.25f;
  float riposte_scale = 0.6f;
  float double_attack_divisor = 500.0f;
  int stun_level_margin = 2;
  float taunt_base_chance = 0.7f;
  float taunt_per_level = 0.08f;
  float skill_up_base = 0.025f;
  float skill_up_scale = 0.2f;
  RegenRules regen;
};

struct DeathRules {
  float xp_loss_fraction = 0.1f;
  int min_level_for_xp_loss = 6;
  float player_corpse_seconds = 3600.0f;
  float mob_corpse_seconds = 300.0f;
  float empty_mob_corpse_seconds = 30.0f;
};

struct MobScaling {
  float hp_base = 15.0f;
  float hp_per_level = 8.0f;
  float hp_quad = 0.08f;
  float max_hit_base = 3.0f;
  float max_hit_per_level = 1.4f;
  float max_hit_quad = 0.006f;
  float ac_per_level = 3.0f;
  float skill_per_level = 5.0f;
  float avoid_skill_per_level = 3.0f;
  float chase_seconds = 30.0f;
  float flee_seconds = 6.0f;
  int aggro_scan_ticks = 4;
};

struct Rules {
  int max_level = 60;
  int max_characters_per_account = 4;
  MovementRules movement;
  XpRules xp;
  ConRules con;
  CombatRules combat;
  DeathRules death;
  MobScaling mob_scaling;
};

struct Content {
  Rules rules;
  std::vector<RaceDef> races;
  std::vector<ClassDef> classes;
  std::vector<CityDef> cities;
  std::vector<ZoneDef> zones;
  std::vector<SkillDef> skills;
  std::vector<AbilityDef> abilities;
  std::vector<ItemDef> items;
  std::vector<LootTable> loot_tables;
  std::vector<MobDef> mobs;

  const RaceDef* find_race(std::string_view id) const;
  const ClassDef* find_class(std::string_view id) const;
  const CityDef* find_city(std::string_view id) const;
  const ZoneDef* find_zone(std::string_view id) const;
  const SkillDef* find_skill(std::string_view id) const;
  const AbilityDef* find_ability(std::string_view id) const;
  const ItemDef* find_item(std::string_view id) const;
  const LootTable* find_loot(std::string_view id) const;
  const MobDef* find_mob(std::string_view id) const;
  // Abilities of one class, ordered by level then file order.
  std::vector<const AbilityDef*> abilities_for(std::string_view class_id) const;
};

class ContentError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The JSON documents, one per content file. A null document counts as an empty list (handy in
// tests); load_content() requires every file to exist.
struct ContentDocs {
  nlohmann::json rules;
  nlohmann::json races;
  nlohmann::json classes;
  nlohmann::json cities;
  nlohmann::json zones;
  nlohmann::json skills;
  nlohmann::json abilities;
  nlohmann::json items;
  nlohmann::json loot;
  nlohmann::json mobs;
};

// Parses the documents. Throws ContentError listing every problem found.
Content parse_content(const ContentDocs& docs);

// Cross-reference and range checks. Returns all problems (empty = valid).
std::vector<std::string> validate_content(const Content& c);

// Loads every <dir>/*.json content file, parses, and validates. Throws ContentError.
Content load_content(const std::filesystem::path& dir);

}  // namespace ef::game
