// Data-driven game content loaded from /content JSON. Validated at startup (fail fast).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <memory>

#include <nlohmann/json.hpp>

#include "game/dungeon_gen.hpp"
#include "game/quests.hpp"
#include "game/craft_types.hpp"
#include "game/social_types.hpp"
#include "game/world_geom.hpp"

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
  float threat_mult = 1.0f;  // scales every point of threat the class generates (tanks hold aggro)
  // Phase 5: an equipped weapon hits for its damage x weapon_mult; bare fists hit for
  // (weapon_damage_base + weapon_damage_per_level x level) x unarmed_mult (Monks fight unarmed).
  float weapon_mult = 1.0f;
  float unarmed_mult = 0.4f;
  StatBlock stats;  // bonuses added to the race's base stats
};

// Armor weights, lightest first. A class wears its own weight and anything lighter.
inline constexpr std::array<const char*, 4> kArmorTypes{"cloth", "leather", "chain", "plate"};
// Weapon categories a class may be proficient with (items name one in Phase 5).
inline constexpr std::array<const char*, 10> kWeaponTypes{"1h_slash", "2h_slash", "1h_pierce", "2h_pierce", "1h_blunt",
                                                          "2h_blunt", "hand_to_hand", "archery", "throwing", "shield"};

struct ClassDef {
  std::string id;
  std::string name;
  std::string archetype;
  std::string description;
  ClassCombat combat;
  std::vector<std::pair<std::string, SkillCap>> skills;  // skills.json defaults merged with overrides
  std::vector<std::string> starter_spells;  // scribed and memorized for new casters
  std::string armor = "plate";              // heaviest armor weight (kArmorTypes)
  std::vector<std::string> weapons;          // weapon proficiencies (kWeaponTypes)
  // Phase 5: given at character creation (equip = put on if it fits).
  struct StartingItem {
    std::string item;
    int count = 1;
    bool equip = false;
  };
  std::vector<StartingItem> starting_items;

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
  // Dungeons: place the group in a room instead ("entrance", "boss", "all", or a room index). Resolved
  // to x/z/radius when the content loads ("all" becomes one group per room besides entrance and boss).
  std::string room{};
  // A named rare: when a member respawns, `rare_chance` of the time it is `rare` instead (one at a time).
  std::string rare{};
  float rare_chance = 0.0f;
};

// A region that moves a player into another zone, arriving at one of that zone's entries.
struct ZoneLineDef {
  std::string id;
  float x = 0.0f;
  float z = 0.0f;
  float hx = 4.0f;  // half extents of the trigger box (axis-aligned unless yaw)
  float hz = 4.0f;
  float yaw = 0.0f;
  std::string to;     // destination zone
  std::string entry;  // entry id in the destination zone
  std::string room;   // dungeons: centre the line in this room ("entrance")
  bool contains(float px, float pz) const;
};

// A named arrival point (zone lines, travel and portals land here).
struct ZoneEntryDef {
  std::string id;
  SpawnPoint at;
  std::string room;  // dungeons: the room centre
};

// Where a scene (Godot) placed an NPC; it overrides npcs.json's position.
struct NpcPlacement {
  std::string npc;
  float x = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
};

// Zone kinds: hub (safe starting city), outdoor, dungeon, test.
struct ZoneDef {
  std::string id;
  std::string name;
  std::string description;
  std::string kind = "outdoor";
  int level_min = 1;
  int level_max = 60;
  bool persistent = false;  // kept loaded while empty (hubs and the test zone)
  bool raid = false;        // Phase 15: a raid dungeon (only a raid may enter; its bosses lock out their loot)
  float min_x = 0.0f;
  float min_z = 0.0f;
  float max_x = 0.0f;
  float max_z = 0.0f;
  float ground_y = 0.0f;  // = terrain.base
  Terrain terrain;
  SpawnPoint safe_spawn;
  float interest_radius = 80.0f;
  float say_radius = 60.0f;
  float nav_cell = 2.0f;
  std::vector<MobSpawn> spawns;
  std::vector<Obstacle> obstacles;  // round blockers: movement collision and line of sight
  std::vector<WallBox> walls;       // box blockers
  std::vector<ZoneLineDef> zone_lines;
  std::vector<ZoneEntryDef> entries;
  std::vector<NpcPlacement> npc_placements;  // from the scene export
  std::string scene;                         // client scene (hand-authored zones), e.g. res://zones/x.tscn
  bool has_dungeon = false;
  DungeonParams dungeon;
  std::shared_ptr<const DungeonLayout> layout;  // generated from `dungeon` at load
  std::uint32_t layout_hash = 0;                // the client checks its own copy against this
  float ground_at(float x, float z) const { return terrain.height(x, z); }
  const ZoneEntryDef* find_entry(std::string_view entry_id) const;
  bool is_dungeon() const { return has_dungeon; }
};

// A faction (content/factions.json): what killing its members does to your standings, and where a
// character starts with it (default plus a race adjustment).
struct FactionDef {
  std::string id;
  std::string name;
  std::string description;
  int base = 0;                                         // default standing
  std::vector<std::pair<std::string, int>> race_mods;  // race -> added to the default
  std::vector<std::pair<std::string, int>> hits;       // killing a member: faction -> change
  int standing_for(std::string_view race, int earned) const;
};

// A named place travel can reach: teleporters, ferries and portal spells.
struct TravelPointDef {
  std::string id;
  std::string name;
  std::string zone;
  SpawnPoint at;
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
  Heal, Dot, Hot, Debuff, Slow, Snare, Root, Mez, Fear, Dispel, Resurrect, Teleport, RestoreManaPct,  // Phase 3
  // Phase 4. summon_pet: a pet (ref = pets.json id) of level effect_amount(). charm: a lasting control that
  // makes the target mob the caster's pet. illusion: a lasting cosmetic look (ref = race or model id).
  // summon_item: `value` copies of an item (ref) that vanish after `duration` s (0 = never). lifetap: damage
  // that heals the caster by `value`% of it (default 100). drain: a DoT whose ticks heal the caster by `value`%.
  // feign_death/hide/sneak: stealth states. threat_drop: lowers the user's threat by `value`% on the target
  // (self-targeted with a radius: on every mob around). track: lists nearby creatures. pick_lock: opens a
  // lockbox. hp_to_mana: converts effect_amount() health into mana.
  SummonPet, Charm, Illusion, SummonItem, Lifetap, Drain, FeignDeath, Hide, Sneak, ThreatDrop, Track, PickLock, HpToMana
};

// Stats a buff or debuff can modify. *_pct values are percentages (+30 = 30% more).
enum class BuffStat : std::uint8_t {
  DamageTakenPct, MeleeDamagePct, CritChancePct, Ac, MaxHp, MaxMana, HpRegen, ManaRegen,
  Str, Sta, Agi, Dex, Wis, Int, Cha, AttackSpeedPct, MoveSpeedPct,
  ResistMagic, ResistFire, ResistCold, ResistPoison, ResistDisease, ResistAll,
  // Phase 4: damage dealt back to each melee attacker that lands a hit; threat generated (+100 = double);
  // invisibility (> 0 hides from proximity aggro until the bearer acts against an enemy).
  DamageShield, ThreatPct, Invisible
};

// enemy: one hostile mob. self: the caster. friendly: the targeted player or pet, or the caster.
// group: the caster, its pet, and nearby group members. area: every hostile mob
// within the radius of the target (range > 0) or of the caster (range 0). corpse: a player corpse.
// pet: the caster's own pet.
enum class TargetType : std::uint8_t { Enemy, Self, Friendly, Group, Area, Corpse, Pet };
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
  std::string ref;           // summon_pet: pet id; summon_item: item id; illusion: race or model id
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
  // Bard songs: no cast time and no mana. Once started a song pulses its effects every
  // rules.songs.pulse_seconds until the bard stops, starts another, or is silenced.
  bool song = false;
  std::string position;  // "behind": the user must stand behind the target (backstab)
  // Phase 5: the ranged slot must hold a weapon of this type (archery shots need a bow), and
  // rules.combat.ranged_weapon_share of its damage adds to the ability's damage.
  std::string requires_weapon;
  bool item_effect = false;  // content/item_effects.json: cast by items, never learned

  bool passive() const { return type == "passive"; }
  // Helps its target (self, friendly, group, corpse) rather than harming it.
  bool beneficial() const { return target != TargetType::Enemy && target != TargetType::Area; }
  // Longest lasting effect (0 if it only has instant effects).
  float lasting_seconds() const;
  bool has(EffectType t) const;
};

bool is_lasting(EffectType t);

// Equipment slots (index = the slot's number inside the equipment container; see items.hpp).
enum class EquipSlot : std::uint8_t {
  Head, Face, Neck, Shoulders, Back, Chest, Arms, Wrist1, Wrist2, Hands, Finger1, Finger2, Waist, Legs, Feet,
  Primary, Secondary, Ranged, Ammo, Ear1, Ear2
};
inline constexpr std::size_t kEquipSlotCount = 21;
inline constexpr std::array<const char*, kEquipSlotCount> kEquipSlotNames{
    "head", "face", "neck", "shoulders", "back", "chest", "arms", "wrist1", "wrist2", "hands", "finger1",
    "finger2", "waist", "legs", "feet", "primary", "secondary", "ranged", "ammo", "ear1", "ear2"};
// Names content uses in an item's "slots" list ("wrist", "finger" and "ear" fit either of a pair).
inline constexpr std::array<const char*, 18> kItemSlotNames{"head", "face", "neck", "shoulders", "back", "chest",
                                                            "arms", "wrist", "hands", "finger", "waist", "legs",
                                                            "feet", "primary", "secondary", "ranged", "ammo", "ear"};
inline constexpr std::array<const char*, 4> kRarities{"common", "uncommon", "rare", "epic"};
// Item kinds. weapon/armor/shield/jewelry/ammo are worn; consumables are used up; bags hold items.
// Phase 16: books teach a tradeskill's recipes when read.
inline constexpr std::array<const char*, 12> kItemKinds{"weapon", "armor", "shield", "jewelry", "ammo", "consumable",
                                                        "scroll", "bag", "loot", "quest", "tradeskill", "book"};

// A spell effect an item carries (content/item_effects.json ids).
struct ItemProc {
  std::string effect;  // fires on a landed melee hit with this weapon
  float rate = 0.0f;   // chance per hit for a 3.0 s weapon; slower weapons proc more often per hit
};
struct ItemClick {
  std::string effect;  // used from the inventory (consumables are used up)
  float cast_time = 0.0f;
  float recast = 0.0f;  // seconds before any item with the same effect can be used again
};

struct ItemDef {
  std::string id;
  std::string name;
  std::string kind;  // kItemKinds
  int stack = 1;
  int value = 0;  // copper: what a vendor charges before markup
  std::string rarity = "common";
  std::string description;
  std::string spell;   // scrolls: the spell they teach
  std::string tradeskill;  // Phase 16: books: the tradeskill whose recipes they teach
  std::string visual;  // item_visuals id shown when the item is held (optional)
  bool summoned = false;  // conjured by a spell: never loot, dissolves on its owner's death
  // Phase 5: equipment.
  std::uint32_t slot_mask = 0;     // bit per EquipSlot it can be worn in
  std::vector<std::string> slots;  // as written (kItemSlotNames)
  std::string armor;   // cloth, leather, chain, plate (armor only)
  std::string weapon;  // kWeaponTypes (weapons, shields, bows)
  int damage = 0;
  float delay = 0.0f;  // seconds (weapons)
  float range = 0.0f;  // bows and thrown weapons
  int ac = 0;
  int hp = 0;
  int mana = 0;
  int endurance = 0;
  StatBlock stats{0, 0, 0, 0, 0, 0, 0};    // bonuses
  std::array<int, kResistCount> resists{};  // index = ResistType (None unused)
  int haste = 0;  // percent; only the hastiest worn item counts
  ItemProc proc;
  ItemClick click;
  bool no_trade = false;
  bool lore = false;  // at most one per character (worn, packed or banked)
  bool magic = false;
  std::vector<std::string> classes;  // empty = every class
  std::vector<std::string> races;    // empty = every race
  int req_level = 0;
  int bag_slots = 0;   // bags: how many items they hold
  std::string source;  // where it comes from, for named placeholders ("dungeon:<name>")

  bool equippable() const { return slot_mask != 0; }
  bool fits(EquipSlot s) const { return ((slot_mask >> static_cast<unsigned>(s)) & 1u) != 0; }
  bool is_bag() const { return kind == "bag"; }
  bool two_handed() const { return weapon.size() > 2 && weapon[0] == '2' && weapon[1] == 'h'; }
  // Phase 17: worn as a stack, used up one at a time (ammunition, and thrown weapons in the ranged slot).
  bool stacks_when_worn() const { return kind == "ammo" || (kind == "weapon" && weapon == "throwing"); }
  bool stackable() const { return stack > 1; }
};

struct LootDrop {
  std::string item;
  float chance = 0.0f;
  int min = 1;
  int max = 1;
};

// A weighted pool: rolled `rolls` times; each roll first passes `chance`, then picks one entry by weight
// (an entry with no item is "nothing").
struct LootPoolEntry {
  std::string item;
  float weight = 1.0f;
  int min = 1;
  int max = 1;
};
struct LootPool {
  int rolls = 1;
  float chance = 1.0f;
  std::vector<LootPoolEntry> entries;
};

struct LootTable {
  std::string id;
  int coin_min = 0;
  int coin_max = 0;
  int max_drops = 1;            // cap on items from drops and pools together
  std::vector<LootDrop> drops;  // independent chances
  std::vector<LootPool> pools;  // weighted picks (Phase 5)
};

// A mob's special ability (Phase 6). Every field but `type` is optional; which ones matter depends on it:
//   proc         a landed melee hit casts `effect` on the target, `chance` of the time
//   cast         every `every` seconds in a fight, cast `effect` on the target within `range`
//   aoe          every `every` seconds, `effect` on every player within `range` of the target (center
//                "target", punishing a crowd) or of the mob (center "self"): spread out or step back
//   enrage       below `below` health, melee damage +`value`%
//   enrage_timer after `seconds` of fighting, melee damage +`value`% (a fight that runs long is lost)
//   rampage      a landed hit also strikes another foe in reach, `chance` of the time
//   summon       every `every` seconds, a target farther than `range` is pulled to the mob
//   adds         at each health fraction in `at`, `count` of mob `mob` join the fight beside it
// `message` is said aloud when it fires.
// Phase 15: Support heals or buffs an ally in the fight (a friendly mob spell).
enum class MobSpecialType : std::uint8_t { Proc, Cast, Aoe, Enrage, EnrageTimer, Rampage, Summon, Adds, Support };
struct MobSpecial {
  MobSpecialType type = MobSpecialType::Proc;
  std::string effect;  // content/mob_spells.json id
  float chance = 0.0f;
  float every = 0.0f;
  float range = 0.0f;
  float below = 0.0f;
  float value = 0.0f;
  float seconds = 0.0f;
  std::vector<float> at;
  std::string mob;
  int count = 1;
  bool on_self = false;  // aoe centred on the mob instead of its target
  std::string message;
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
  bool charm_immune = false;  // named and boss mobs
  // Phase 12: a city guard. Guards attack monsters fighting in their sight (a train led into town),
  // call the other guards, and claim the kill when they did most of the damage.
  bool guard = false;
  std::string loot;
  // Resolved from the model after parsing: the mob's body radius at scale 1 (models.json collision_radius).
  float collision_radius = 0.5f;
  std::string body = "humanoid";  // humanoid, animal, insect, undead, construct, elemental, ...
  // Per-type resist values (index = ResistType); -1 = rules.mob_scaling.resist_base + resist_per_level * level.
  std::array<int, kResistCount> resists{-1, -1, -1, -1, -1, -1};
  // Phase 6
  std::vector<MobSpecial> specials;
  bool named = false;  // a named rare (unique name, better loot)
  bool boss = false;   // a dungeon boss
};

// Pet templates (content/pets.json). A summoned pet is a mob of this template at the level its spell
// gives, owned by the caster; its numbers scale like any mob's (rules.mob_scaling).
struct PetDef {
  std::string id;
  std::string name;      // shown as "<owner>'s <name>"
  std::string variant;   // elemental, undead, spirit, animated, beast
  std::string model;
  std::string body = "elemental";
  float scale = 1.0f;
  float hp_mult = 1.0f;
  float damage_mult = 1.0f;
  float attack_delay = 2.5f;
  float run_speed = 7.5f;
  float threat_mult = 1.0f;              // earth servants hold a mob's attention
  float damage_shield_per_level = 0.0f;  // fire servants burn whoever strikes them
  std::array<int, kResistCount> resists{-1, -1, -1, -1, -1, -1};
  std::string description;
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
  // Per gender and armour type, the model an outfit switches to ("" = keeps the gender's model).
  std::array<std::array<std::string, kArmorTypes.size()>, kGenders.size()> outfit_models;
};

struct ClassAppearance {
  std::string class_id;
  std::vector<std::pair<std::string, std::string>> held;  // slot -> item id
};

// Non-combat characters placed in a zone. Trainers sell spell scrolls to their classes.
// A vendor's stock line: count -1 = never runs out; otherwise one comes back every `restock` seconds.
struct VendorStock {
  std::string item;
  int count = -1;
  float restock = 0.0f;
};

// One destination an NPC offers: a travel point or city id, or "bind" for a soulbinder.
struct TravelOptionDef {
  std::string dest;
  int fee = 0;          // copper
  float delay = 0.0f;   // seconds before departure (ferries)
};

struct NpcDef {
  std::string id;
  std::string name;
  std::string role;  // trainer, lockbox, vendor, banker, auctioneer, teleporter, ferry, binder, questgiver, station
  std::string model = "humanoid";
  int level = 50;
  std::string zone;
  float x = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
  std::vector<std::string> classes;  // whom a trainer teaches
  std::vector<std::string> sells;    // item ids (scrolls)
  std::string greeting;
  int lock_level = 0;  // lockboxes: the pick lock difficulty
  std::vector<VendorStock> stock;  // vendors
  std::vector<TravelOptionDef> travel;  // teleporters, ferries, binders
  std::string faction;  // whose standing decides whether it serves you ("" = serves everyone)
  std::string station;  // stations (Phase 13): the station kind a tradeskill works at (content/crafts.json)
  int band_min = 0;  // vendors: the level band they serve (display)
  int band_max = 0;
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
  // Phase 8: tougher mobs are worth more. Kill XP x clamp(1 + toughness_weight x (hp_mult - 1), min, max).
  double toughness_weight = 0.75;
  double toughness_min = 1.0;
  double toughness_max = 6.0;
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
  float ranged_weapon_share = 0.5f;  // archery: share of the bow's damage added to each shot (Phase 5)
  // Phase 4
  float behind_arc_degrees = 120.0f;   // positional attacks: the arc behind the target that counts
  float offhand_damage_mult = 0.6f;    // dual wield: the off-hand swing's share of a main-hand hit
  float dual_wield_divisor = 250.0f;   // chance = min(max_dual_wield_chance, skill / divisor)
  float max_dual_wield_chance = 0.85f;
  float enhanced_damage_max = 0.25f;   // enhanced damage at skill 250: +25% melee damage
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

// Pets and charm (rules.json "pets").
struct PetRules {
  float follow_distance = 3.0f;      // a following pet stays about this close to its owner
  float leash_distance = 120.0f;     // a pet this far from its owner drops its target and comes back
  float assist_radius = 40.0f;       // pets defend their owner against mobs this close
  float back_off_seconds = 6.0f;     // after "back off" a pet ignores attackers this long
  float charm_check_seconds = 6.0f;  // a charm rolls to break this often
  // Break chance per check: clamp(base + per_level * (target level - caster level) - cha_scale * (cha - 75), min, max)
  float charm_break_base = 0.08f;
  float charm_break_per_level = 0.025f;
  float charm_break_cha_scale = 0.0008f;
  float min_charm_break = 0.02f;
  float max_charm_break = 0.6f;
};

// Bard songs (rules.json "songs").
struct SongRules {
  float pulse_seconds = 6.0f;  // a song applies its effects when started and then this often
};

// Sneak, hide, feign death, pick lock (rules.json "stealth").
struct StealthRules {
  float sneak_speed_mult = 0.5f;  // sneaking movement speed
  float sneak_aggro_mult = 0.5f;  // proximity aggro radius against a sneaking player
  // success = clamp(base + per_skill * skill - per_level * max(0, level difference), min, max)
  float hide_base = 0.35f;
  float hide_per_skill = 0.003f;
  float feign_base = 0.4f;
  float feign_per_skill = 0.0028f;
  float feign_per_level = 0.02f;
  float pick_base = 0.3f;
  float pick_per_skill = 0.004f;
  float pick_per_level = 0.02f;
  float min_chance = 0.05f;
  float max_chance = 0.97f;
};

// Vendors, bank, trade, auctions and group loot (rules.json "economy").
struct EconomyRules {
  // Vendor prices: buy = value x clamp(buy_base - buy_per_cha x (cha - 75), min_buy, max_buy);
  // sell = value x clamp(sell_base + sell_per_cha x (cha - 75), min_sell, max_sell). max_sell < min_buy.
  float buy_base = 1.3f;
  float buy_per_cha = 0.002f;
  float min_buy = 1.05f;
  float max_buy = 1.6f;
  float sell_base = 0.25f;
  float sell_per_cha = 0.001f;
  float min_sell = 0.1f;
  float max_sell = 0.4f;
  int buyback_slots = 20;   // a vendor resells this many of the items players sold it
  float npc_range = 12.0f;  // vendors, bankers and auctioneers
  int bank_slots = 16;
  float trade_range = 15.0f;
  float trade_invite_seconds = 30.0f;
  float auction_fee_pct = 5.0f;  // paid when listing, not refunded
  int auction_min_fee = 10;      // copper
  float auction_hours = 48.0f;
  int auction_max_listings = 20;  // per character, sold and expired ones included until collected
  int auction_page_size = 20;
  float loot_roll_seconds = 30.0f;
  float round_robin_free_seconds = 120.0f;  // round-robin items anyone in the party may take after this
  int loot_party_max = 6;
};

// Content-shape requirements checked at load (0 = off; content/rules.json turns them on).
struct ContentRules {
  int min_class_abilities = 0;  // every class with content has at least this many abilities + spells
  int max_level_gap = 0;        // ...and gains a new one at least every this many levels from level 1
  int min_items = 0;            // at least this many items that are not spell scrolls (Phase 5)
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
  PetRules pets;
  SongRules songs;
  StealthRules stealth;
  EconomyRules economy;
  ContentRules content;
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
  std::vector<PetDef> pets;
  std::vector<ModelDef> models;
  std::vector<AnimationMapDef> animation_maps;
  std::vector<ItemVisualDef> item_visuals;
  std::vector<RaceAppearance> race_appearance;
  std::vector<ClassAppearance> class_appearance;
  std::vector<AbilityDef> item_effects;  // content/item_effects.json
  std::vector<TravelPointDef> travel_points;  // content/travel.json
  std::vector<AbilityDef> mob_spells;         // content/mob_spells.json: what mob specials cast
  std::vector<FactionDef> factions;           // content/factions.json
  std::vector<QuestDef> quests;               // content/quests.json
  SocialRules social;                         // content/social.json (Phase 7)
  CraftContent crafts;                        // content/crafts.json (Phase 13)

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
  const PetDef* find_pet(std::string_view id) const;
  const ModelDef* find_model(std::string_view id) const;
  const ItemVisualDef* find_item_visual(std::string_view id) const;
  const AbilityDef* find_item_effect(std::string_view id) const;
  const TravelPointDef* find_travel_point(std::string_view id) const;
  const AbilityDef* find_mob_spell(std::string_view id) const;
  const FactionDef* find_faction(std::string_view id) const;
  const QuestDef* find_quest(std::string_view id) const;
  // Rebuilds the id index find_item uses (parse_content calls it; tests that edit `items` may too).
  void index_items();
  // Abilities of one class, ordered by level then file order.
  std::vector<const AbilityDef*> abilities_for(std::string_view class_id) const;
  std::vector<const AbilityDef*> spells_for(std::string_view class_id) const;
  // The scroll item that teaches a spell (nullptr if none).
  const ItemDef* scroll_for(std::string_view spell_id) const;

 private:
  std::unordered_map<std::string, std::size_t> item_index_;  // id -> position in `items` (verified on use)
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
  nlohmann::json pets;
  nlohmann::json models;
  nlohmann::json appearance;
  nlohmann::json item_effects;
  nlohmann::json zone_scenes;  // {"scenes": [...]}: content/zones/scenes/*.json (scene exports)
  nlohmann::json travel;       // content/travel.json
  nlohmann::json mob_spells;   // content/mob_spells.json
  nlohmann::json factions;     // content/factions.json
  nlohmann::json quests;       // content/quests.json
  nlohmann::json social;       // content/social.json
  nlohmann::json crafts;       // content/crafts.json (Phase 13)
};

const char* resist_name(ResistType r);
// Level bands used by content validation: every class with abilities has some in each band.
inline constexpr std::array<std::pair<int, int>, 4> kLevelBands{{{1, 15}, {15, 30}, {30, 45}, {45, 60}}};

// Reads every content file in `dir` (zones from dir/zones/*.json, scene exports from dir/zones/scenes).
ContentDocs read_content_docs(const std::filesystem::path& dir);

// Parses the documents. Throws ContentError listing every problem found.
Content parse_content(const ContentDocs& docs);

// Cross-reference and range checks. Returns all problems (empty = valid).
std::vector<std::string> validate_content(const Content& c);

// Loads every <dir>/*.json content file, parses, and validates. Throws ContentError.
Content load_content(const std::filesystem::path& dir);

}  // namespace ef::game
