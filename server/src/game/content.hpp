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
  std::vector<std::string> starter_spells;  // scribed and memorized for new casters

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

// A vertical cylinder that blocks line of sight (spells). Movement collision arrives with the
// zone pipeline in Phase 6.
struct Obstacle {
  float x = 0.0f;
  float z = 0.0f;
  float radius = 1.0f;
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
  std::vector<Obstacle> obstacles;
};

struct SkillDef {
  std::string id;
  std::string name;
  std::string kind;
  std::string description;
};

// Effect types shared by abilities and spells. Instant effects resolve when the ability lands;
// lasting ones (buff, debuff, slow, snare, root, mez, fear, dot, hot) become one ActiveBuff on
// the target that holds every lasting line of that spell.
enum class EffectType : std::uint8_t {
  Damage, Stun, Threat, Taunt, Buff, HealPct, RestoreEndurancePct, AreaThreat,  // Phase 2
  Heal, Dot, Hot, Debuff, Slow, Snare, Root, Mez, Fear, Dispel, Resurrect, Teleport, RestoreManaPct
};

// Stats a buff or debuff can modify. *_pct values are percentages (+30 = 30% more).
enum class BuffStat : std::uint8_t {
  DamageTakenPct, MeleeDamagePct, CritChancePct, Ac, MaxHp, MaxMana, HpRegen, ManaRegen,
  Str, Sta, Agi, Dex, Wis, Int, Cha, AttackSpeedPct, MoveSpeedPct,
  ResistMagic, ResistFire, ResistCold, ResistPoison, ResistDisease, ResistAll
};

// enemy: one hostile mob. self: the caster. friendly: the targeted player, or the caster.
// group: the caster and nearby group members (groups arrive in Phase 7). area: every hostile
// mob within the radius of the target (range > 0) or of the caster (range 0). corpse: a player corpse.
enum class TargetType : std::uint8_t { Enemy, Self, Friendly, Group, Area, Corpse };
enum class Resource : std::uint8_t { None, Endurance, Mana };
// Resist types. None cannot be resisted (heals, buffs, physical abilities).
enum class ResistType : std::uint8_t { None, Magic, Fire, Cold, Poison, Disease };
inline constexpr std::size_t kResistCount = 6;

struct EffectDef {
  EffectType type = EffectType::Damage;
  float base = 0.0f;       // amount at level 0 (damage, heal, threat, per-tick amount)
  float per_level = 0.0f;  // added per caster level
  float max = 0.0f;        // cap on base + per_level * level (0 = uncapped)
  float value = 0.0f;      // buff magnitude, percentage, dispel count, or XP-restore percent
  float duration = 0.0f;   // seconds (stun and every lasting effect)
  float radius = 0.0f;     // area effects
  float tick = 0.0f;       // dot/hot interval in seconds (0 = rules.spells.tick_seconds)
  int max_target_level = 0;  // stun/mez/fear: no effect on targets above this level (0 = any)
  std::string destination;   // teleport: "safe_spot", "bind", or a city id
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
  float cooldown = 0.0f;  // recast timer
  bool gcd = true;
  float range = 0.0f;
  TargetType target = TargetType::Enemy;
  ResistType resist = ResistType::None;
  int resist_adjust = 0;  // added to the target's resist (negative = harder to resist)
  std::string skill;  // skill that improves on use (actives) or that the passive represents
  std::string description;
  std::vector<EffectDef> effects;
  // Spells (spells.json): learned from scrolls and cast from memorized gems.
  bool spell = false;
  std::string stack_group;  // same group = same buff line; defaults to the id
  int rank = 1;             // a higher rank overwrites a lower one in the same group
  float radius = 0.0f;      // area spells
  int max_targets = 0;      // area spells (0 = rules.spells.max_area_targets)
  std::string requires_body;  // e.g. "undead": only works on mobs of that body type

  bool passive() const { return type == "passive"; }
  // Helps its target (self, friendly, group, corpse) rather than harming it.
  bool beneficial() const { return target != TargetType::Enemy && target != TargetType::Area; }
  // Longest lasting effect (0 if it only has instant effects).
  float lasting_seconds() const;
};

bool is_lasting(EffectType t);

struct ItemDef {
  std::string id;
  std::string name;
  std::string kind;  // loot, consumable, scroll
  int stack = 1;
  int value = 0;  // copper
  std::string rarity;
  std::string description;
  std::string spell;  // scrolls: the spell they teach
  std::string visual;  // item_visuals id shown when the item is held (optional)
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
  // Resolved from the model after parsing: the mob's body radius at scale 1 (models.json collision_radius).
  float collision_radius = 0.5f;
  std::string body = "humanoid";  // humanoid, animal, insect, undead, construct, elemental, ...
  // Per-type resist values (index = ResistType); -1 = rules.mob_scaling.resist_base + resist_per_level * level.
  std::array<int, kResistCount> resists{-1, -1, -1, -1, -1, -1};
};

// Visual models (content/models.json). The server only needs the gameplay-relevant numbers and the ids
// for reference checks; scene paths, animation maps, attachment bones and parts are client data
// (checked by tools/validate_content.py).
struct ModelDef {
  std::string id;
  float scale = 1.0f;             // multiplies the entity's own scale
  float nameplate_height = 2.2f;  // metres above the feet at scale 1
  float collision_radius = 0.5f;  // body radius at scale 1: melee reach and click picking
  std::string animations;         // animation map id ("" = none: placeholder motion)
};

struct AnimationMapDef {
  std::string id;
};

// A mesh that attaches to a skeleton slot when an item is held (weapon, shield).
struct ItemVisualDef {
  std::string id;
  std::string slot;  // main_hand, off_hand, back, head
};

inline constexpr std::array<const char*, 4> kAttachSlots{"main_hand", "off_hand", "back", "head"};
inline constexpr std::array<const char*, 2> kGenders{"female", "male"};  // index = wire gender

// content/appearance.json: which model each race and gender uses, and what each class holds by default.
struct RaceAppearance {
  std::string race;
  std::array<std::string, kGenders.size()> models;  // per gender
};

struct ClassAppearance {
  std::string class_id;
  std::vector<std::pair<std::string, std::string>> held;  // slot -> item id
};

// Non-combat characters placed in a zone. Trainers sell spell scrolls to their classes.
struct NpcDef {
  std::string id;
  std::string name;
  std::string role;  // "trainer"
  std::string model = "humanoid";
  int level = 50;
  std::string zone;
  float x = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
  std::vector<std::string> classes;  // whom a trainer teaches
  std::vector<std::string> sells;    // item ids (scrolls)
  std::string greeting;
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
  float resist_base = 5.0f;
  float resist_per_level = 0.5f;
};

// The spell engine's tunables (rules.json "spells").
struct SpellRules {
  float tick_seconds = 3.0f;          // default dot/hot interval
  int buff_slots = 15;                // beneficial effects on one target
  int debuff_slots = 10;              // detrimental effects on one target
  int spellbook_size = 120;
  std::vector<int> gem_unlock_levels{1, 1, 4, 9, 16, 25, 35, 45};  // one entry per gem (max 8)
  float memorize_seconds = 2.0f;      // a newly memorized gem is ready after this long
  float move_interrupt_distance = 0.5f;
  float range_slack = 1.1f;           // target may drift this far past range before the spell lands
  float damage_spread = 0.1f;         // +-10% on spell damage and heals
  // Resists: chance = clamp(eff / (eff + scale_base + scale_per_level * caster level)
  //                         + (target level - caster level) * per_level_diff, min, max)
  float resist_scale_base = 100.0f;
  float resist_scale_per_level = 5.0f;
  float resist_per_level_diff = 0.03f;
  float min_resist = 0.02f;
  float max_resist = 0.95f;
  float partial_resist_min = 0.25f;   // a partial resist keeps at least this fraction of damage
  // Concentration when hit while casting: keep = clamp(base + channeling bonus - damage penalty).
  float concentration_base = 0.5f;
  float concentration_skill_bonus = 0.45f;
  float concentration_skill_scale = 100.0f;
  float concentration_damage_scale = 0.6f;
  float min_concentration = 0.05f;
  float max_concentration = 0.97f;
  float root_break_chance = 0.05f;    // per direct-damage hit on a rooted mob
  float heal_aggro_radius = 60.0f;    // mobs this close to a healer notice heals on their targets
  float buff_threat_per_level = 2.0f; // threat per spell level for buffing someone in combat
  float rez_offer_seconds = 60.0f;
  float rez_hp_pct = 0.2f;            // a resurrected player returns with this much health
  int max_area_targets = 8;
  float meditate_bonus = 0.5f;        // sitting mana regen x (1 + bonus * meditate / cap)
};

struct Rules {
  int max_level = 60;
  int max_characters_per_account = 4;
  int starting_coin = 0;  // copper given to a new character
  MovementRules movement;
  XpRules xp;
  ConRules con;
  CombatRules combat;
  DeathRules death;
  MobScaling mob_scaling;
  SpellRules spells;
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
  std::vector<AbilityDef> spells;
  std::vector<NpcDef> npcs;
  std::vector<ModelDef> models;
  std::vector<AnimationMapDef> animation_maps;
  std::vector<ItemVisualDef> item_visuals;
  std::vector<RaceAppearance> race_appearance;
  std::vector<ClassAppearance> class_appearance;

  const RaceDef* find_race(std::string_view id) const;
  const ClassDef* find_class(std::string_view id) const;
  const CityDef* find_city(std::string_view id) const;
  const ZoneDef* find_zone(std::string_view id) const;
  const SkillDef* find_skill(std::string_view id) const;
  const AbilityDef* find_ability(std::string_view id) const;
  const ItemDef* find_item(std::string_view id) const;
  const LootTable* find_loot(std::string_view id) const;
  const MobDef* find_mob(std::string_view id) const;
  const AbilityDef* find_spell(std::string_view id) const;
  const NpcDef* find_npc(std::string_view id) const;
  const ModelDef* find_model(std::string_view id) const;
  const ItemVisualDef* find_item_visual(std::string_view id) const;
  // Abilities of one class, ordered by level then file order.
  std::vector<const AbilityDef*> abilities_for(std::string_view class_id) const;
  std::vector<const AbilityDef*> spells_for(std::string_view class_id) const;
  // The scroll item that teaches a spell (nullptr if none).
  const ItemDef* scroll_for(std::string_view spell_id) const;
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
  nlohmann::json spells;
  nlohmann::json npcs;
  nlohmann::json models;
  nlohmann::json appearance;
};

const char* resist_name(ResistType r);
// Level bands used by content validation: every class with abilities has some in each band.
inline constexpr std::array<std::pair<int, int>, 4> kLevelBands{{{1, 15}, {15, 30}, {30, 45}, {45, 60}}};

// Parses the documents. Throws ContentError listing every problem found.
Content parse_content(const ContentDocs& docs);

// Cross-reference and range checks. Returns all problems (empty = valid).
std::vector<std::string> validate_content(const Content& c);

// Loads every <dir>/*.json content file, parses, and validates. Throws ContentError.
Content load_content(const std::filesystem::path& dir);

}  // namespace ef::game
