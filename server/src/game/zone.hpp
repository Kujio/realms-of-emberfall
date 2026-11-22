// A single zone instance: owns its players, mobs, and corpses; simulates movement, mob AI, and
// combat; and computes interest sets. Networking and persistence are abstracted behind ZoneSink so
// the zone is testable without sockets or a database. Game rules live in rules.hpp (pure).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "game/class_rules.hpp"
#include "game/content.hpp"
#include "game/factions.hpp"
#include "game/items.hpp"
#include "game/loot_rules.hpp"
#include "game/mob_ai.hpp"
#include "game/movement.hpp"
#include "game/nav_grid.hpp"
#include "game/rules.hpp"
#include "game/spatial_hash.hpp"
#include "game/spells.hpp"
#include "game/threat.hpp"
#include "game/trade.hpp"
#include "net/protocol.gen.hpp"

namespace ef::game {

enum class EntityKind : std::uint8_t { Player = 0, Mob = 1, Corpse = 2, Npc = 3 };

// Bits in EntityState.flags / EntitySpawn.flags / PlayerStats.flags.
inline constexpr std::uint8_t kFlagDead = 1;
inline constexpr std::uint8_t kFlagInCombat = 2;
inline constexpr std::uint8_t kFlagSitting = 4;
inline constexpr std::uint8_t kFlagStunned = 8;
inline constexpr std::uint8_t kFlagFleeing = 16;
inline constexpr std::uint8_t kFlagEvading = 32;
inline constexpr std::uint8_t kFlagMezzed = 64;
inline constexpr std::uint8_t kFlagRooted = 128;
// Bits in EntityState.flags2 / EntitySpawn.flags2 / PlayerStats.flags2 (Phase 4).
inline constexpr std::uint8_t kFlag2Feigned = 1;    // lying still, playing dead
inline constexpr std::uint8_t kFlag2Hidden = 2;     // hidden or invisible
inline constexpr std::uint8_t kFlag2Sneaking = 4;
inline constexpr std::uint8_t kFlag2Singing = 8;
inline constexpr std::uint8_t kFlag2Pet = 16;       // a mob that belongs to a player
inline constexpr std::uint8_t kFlag2Charmed = 32;   // ...because it is charmed

// CombatEvent.kind values. The client turns these into combat-log lines and floating text.
enum class CombatKind : std::uint8_t {
  Hit = 0, Miss, Dodge, Parry, Riposte, Crit, AbilityHit, AbilityMiss, Taunt, TauntFail,
  Stun, StunFail, Heal, Buff, BuffFade, Death, Evade,
  // Phase 3: spells. amount is damage/healing, or a duration in ms for lasting effects.
  SpellHit, Resist, DotTick, HotTick, Mez, MezBreak, Root, RootBreak, Snare, Slow, Fear, Debuff,
  Dispel, Resurrect, Teleport, ManaRestore, Immune,
  // Phase 4. amount: damage for DamageShield/Lifetap/DrainTick, healing for LifetapHeal, a duration in
  // ms for Charm/Illusion, percent of threat dropped for ThreatDrop.
  DamageShield, Lifetap, DrainTick, Charm, CharmBreak, Feign, FeignFail, Hide, HideFail, Summon,
  ThreatDrop, Illusion, SongPulse, PetDeath, Backstab
};

// Chat channel ids carried in ChatMessage.channel.
inline constexpr std::uint8_t kChatSay = 0;
inline constexpr std::uint8_t kChatSystem = 1;
inline constexpr std::uint8_t kChatCombat = 2;

// LootTake.slot value meaning "everything".
inline constexpr std::uint8_t kLootAll = 255;
inline constexpr std::size_t kMaxBuffs = 15;
inline constexpr std::size_t kMaxGems = 8;
// Body radii for movement collision (players match the client's prediction).
inline constexpr float kPlayerRadius = 0.4f;
inline constexpr float kMobBodyRadius = 0.5f;

struct SkillState {
  std::string id;
  int value = 0;
};

// A beneficial effect or a recast timer saved with the character (seconds left).
struct SavedBuff {
  std::string ability_id;
  int caster_level = 1;
  std::string caster_name;
  float seconds_left = 0.0f;
  float next_tick_seconds = 0.0f;  // periodic effects: time until the next tick (0 = none)
};
struct SavedTimer {
  std::string ability_id;
  float seconds_left = 0.0f;
};

// A resurrection waiting for the dead player's answer.
struct RezOffer {
  std::uint32_t id = 0;  // unique per offer: a reply must name the offer it answers
  EntityId corpse = 0;   // 0 = none
  float pct = 0.0f;
  EntityId caster = 0;
  std::string caster_name;
  std::uint32_t expires_tick = 0;
};

// A Bard's current song: pulses its effects until stopped or replaced.
struct SongState {
  const AbilityDef* ability = nullptr;
  int gem = -1;
  EntityId target = 0;
  std::uint32_t next_pulse = 0;
  bool active() const { return ability != nullptr; }
};

struct PendingInput {
  std::uint32_t seq = 0;
  MoveIntent intent;
  Vec3 predicted;
};

struct PlayerEntity {
  EntityId id = 0;
  std::uint32_t session = 0;
  std::int64_t char_id = 0;
  std::string name;
  std::string race;
  std::string class_id;
  std::uint8_t gender = 0;  // 0 = female, 1 = male: picks the model
  const RaceDef* race_def = nullptr;
  const ClassDef* class_def = nullptr;
  int level = 1;
  Vec3 pos;
  float yaw = 0.0f;

  // Movement input state
  static constexpr std::size_t kQueueCap = 8;
  std::array<PendingInput, kQueueCap> queue{};
  std::size_t queue_head = 0;
  std::size_t queue_size = 0;
  InputBudget budget{4};
  std::uint32_t last_queued_seq = 0;
  std::uint32_t last_applied_seq = 0;
  bool has_seq = false;
  Vec3 last_pred;
  std::uint32_t last_pred_seq = 0;
  bool has_pred = false;
  std::uint32_t check_cooldown = 0;  // ticks to skip teleport/desync checks after a correction

  // Character state (persisted)
  std::uint64_t xp = 0;  // total experience
  DerivedStats base_stats;  // race, class, and level
  DerivedStats stats;       // base_stats plus buffs (refresh_stats)
  int hp = 1;
  int mana = 0;
  int endurance = 0;
  std::uint64_t coin = 0;  // copper
  std::uint64_t bank_coin = 0;
  Inventory inv;  // worn gear, pack, bags, and bank (Phase 5)
  std::vector<SkillState> skills;
  std::string bind_zone;
  Vec3 bind_pos;
  float bind_yaw = 0.0f;
  std::vector<std::string> spellbook;             // spell ids, in the order scribed
  std::array<std::string, kMaxGems> gems{};       // memorized spell ids ("" = empty)

  // Combat state (not persisted)
  float hp_acc = 0.0f;  // fractional regeneration
  float mana_acc = 0.0f;
  float end_acc = 0.0f;
  EntityId target = 0;
  bool auto_attack = false;
  bool sitting = false;
  bool dead = false;
  std::uint32_t swing_ready_tick = 0;
  std::uint32_t gcd_until = 0;
  std::uint32_t stunned_until = 0;
  std::uint32_t last_combat_tick = 0;
  bool ever_in_combat = false;
  std::uint32_t range_warn_tick = 0;
  std::vector<std::pair<std::string, std::uint32_t>> cooldowns;  // ability id -> ready tick
  std::vector<ActiveBuff> buffs;
  bool buffs_dirty = false;
  std::array<std::uint32_t, kMaxGems> gem_ready{};  // memorizing: not castable before this tick
  CastState cast;
  RezOffer rez;
  EntityId looting = 0;
  // Phase 4
  EntityId pet = 0;       // the player's pet (summoned or charmed), 0 = none
  SongState song;
  bool hidden = false;    // hide: proximity aggro ignores the player; moving ends it unless sneaking
  bool sneaking = false;  // slower, quieter movement
  bool feigned = false;   // feign death: lying still until the player acts or moves
  std::uint64_t pet_hash = 0;
  std::uint32_t pet_sent_tick = 0;
  // Phase 5
  EntityId banking = 0;        // the banker whose window is open (bank slots usable while in range)
  EntityId vendor = 0;         // the vendor whose window is open
  std::uint32_t trade = 0;     // the trade session this player is in (0 = none)
  LootRule loot_rule = LootRule::NeedGreed;
  bool inv_dirty = false;      // send the inventory at the end of the tick
  bool save_dirty = false;     // the game server saves the character soon (items moved)
  std::uint64_t sheet_hash = 0;
  std::string held_sig;        // main|off|ranged item ids last broadcast
  int unplaced_items = 0;      // stacks that found no slot when loading (the character must not enter)
  // Phase 6
  std::uint32_t arrived_tick = 0;  // zone lines ignore a player who has only just arrived
  bool transferring = false;       // leaving for another zone at the end of this tick
  EntityId traveling_npc = 0;      // a ferry this player is waiting on (0 = none)
  FactionStandings factions;       // earned standing (persisted)
  bool factions_dirty = false;     // send FactionStandings at the end of the tick
  QuestBook quests;                // quest progress (persisted as JSON)
  bool journal_dirty = false;      // send QuestJournal at the end of the tick

  // Replication
  std::uint32_t changed_tick = 0;   // last tick position/hp/flags changed (snapshot filter)
  std::uint64_t stats_hash = 0;
  std::uint32_t stats_sent_tick = 0;
  std::vector<EntityId> known;      // sorted ids this player has spawned on its client
  std::unordered_map<EntityId, std::uint32_t> sent_tick;  // last tick each known entity's state went out

  // Effective skill: 0 if locked at the current level, else min(trained, cap).
  int skill(std::string_view skill_id) const;
  float buff_total(BuffStat stat) const;
  bool knows_spell(std::string_view spell_id) const;
  // Hidden, invisible, or feigning: mobs do not notice the player on their own.
  bool stealthed() const;
};

struct MobEntity {
  EntityId id = 0;
  const MobDef* def = nullptr;
  std::shared_ptr<const MobDef> owned_def;  // hand-placed mobs own a copy of their definition
  std::size_t spawn_index = SIZE_MAX;  // SIZE_MAX = placed by hand (never respawns)
  Vec3 home;
  Vec3 pos;
  float yaw = 0.0f;
  MobStats stats;
  int hp = 1;
  float hp_acc = 0.0f;
  AiState state = AiState::Idle;
  std::uint32_t state_tick = 0;
  ThreatTable threat;
  std::vector<std::pair<std::int64_t, int>> damage_by;  // char id -> damage dealt (loot/XP rights)
  std::uint32_t swing_ready_tick = 0;
  std::uint32_t stunned_until = 0;
  std::uint32_t last_melee_tick = 0;
  std::uint32_t next_wander_tick = 0;
  std::uint32_t changed_tick = 0;
  Vec3 dest;
  bool fled = false;
  std::vector<ActiveBuff> buffs;  // spell effects on the mob (debuffs, DoTs, crowd control)
  bool buffs_dirty = false;
  bool forgot = false;  // its foes vanished (feign death, evade): walks home without resetting
  // Pets (Phase 4): a summoned pet (owned_def from a pets.json template) or a charmed mob.
  EntityId owner = 0;  // the player it obeys, 0 = wild
  std::int64_t owner_char = 0;
  bool charmed = false;
  const PetDef* pet_def = nullptr;  // summoned pets: their template
  PetMode pet_mode = PetMode::Follow;
  EntityId pet_target = 0;  // what the pet attacks (ordered, or defending)
  Vec3 guard_pos;
  std::uint32_t passive_until = 0;  // "back off": ignore attackers until then
  std::uint32_t next_charm_check = 0;
  // Phase 6: the path a mob follows around walls (empty = walking straight).
  std::vector<Vec3> path;
  std::size_t path_next = 0;
  Vec3 path_goal;
  std::uint32_t path_tick = 0;
  // Phase 6: special abilities and boss mechanics (reset when the fight ends).
  std::vector<std::uint32_t> special_ready;  // per special: tick it may fire again
  std::vector<int> adds_fired;               // per adds special: thresholds already passed
  std::uint32_t fight_start = 0;             // 0 = not fighting
  float enrage_pct = 0.0f;                   // extra melee damage from enrages
  bool enraged = false;
  bool timer_enraged = false;
  std::vector<EntityId> adds;                // helpers it called (they leave if it resets)
  EntityId add_of = 0;                       // the boss that called this one
  bool rare = false;                         // a named rare in its spawn group's place
  bool alive() const { return state != AiState::Dead && state != AiState::Respawn; }
  bool is_pet() const { return owner != 0; }
};

struct NpcEntity {
  EntityId id = 0;
  const NpcDef* def = nullptr;
  Vec3 pos;
  float yaw = 0.0f;
};

// One stack on a corpse. Player corpses remember where each item was (looting it back re-equips);
// mob corpses may reserve an item for one party member (round robin or a won roll).
struct CorpseItem {
  ItemStackState stack;
  SlotCode origin = kNoSlot;
  std::int64_t owner = 0;    // char id that may take it (0 = anyone allowed to loot the corpse)
  std::uint32_t roll = 0;    // a need/greed roll in progress
  bool won = false;          // owner won it in a roll: the reservation never lapses
  bool empty() const { return stack.empty(); }
};

struct CorpseEntity {
  EntityId id = 0;
  std::int64_t db_id = 0;          // player corpses only (persisted)
  bool player_corpse = false;
  std::int64_t owner_char_id = 0;  // player corpses: the dead character
  std::int64_t loot_char_id = 0;   // who may loot (owner, or the mob's killer); 0 = anyone
  std::string name;
  std::string owner_name;
  std::string model;
  std::string class_id;     // live player corpses: the owner's class, so clients draw the right appearance
  std::uint8_t gender = 0;  // live player corpses: the owner's gender
  float scale = 1.0f;
  int level = 1;
  Vec3 pos;
  float yaw = 0.0f;
  std::uint64_t coin = 0;
  std::vector<CorpseItem> items;
  std::uint64_t xp_lost = 0;
  std::uint32_t expires_tick = 0;
  EntityId opened_by = 0;
  bool rezzed = false;  // a resurrection was accepted (not persisted: xp_lost is 0 by then)
  // Group loot (mob corpses): who shares it, and when round-robin items become free for all of them.
  std::vector<std::int64_t> party;
  std::uint32_t free_tick = 0;
  bool empty() const;
  bool shared_with(std::int64_t char_id) const;
};

struct ZonePlayerInit {
  std::uint32_t session = 0;
  std::int64_t char_id = 0;
  std::string name;
  std::string race;
  std::string class_id;
  int gender = 0;  // 0 = female, 1 = male
  int level = 1;
  Vec3 pos;
  float yaw = 0.0f;
  std::uint64_t xp = 0;
  int hp = -1;  // -1 = full
  int mana = -1;
  int endurance = -1;
  std::uint64_t coin = 0;
  std::uint64_t bank_coin = 0;
  std::vector<std::pair<int, ItemStackState>> items;  // slot code -> stack
  std::vector<std::pair<int, float>> item_seconds;    // slot -> seconds a summoned item has left
  std::vector<SkillState> skills;
  std::string bind_zone;
  Vec3 bind_pos;
  float bind_yaw = 0.0f;
  std::vector<std::string> spellbook;  // empty for a new caster: the class starter spells are granted
  std::vector<std::string> gems;       // up to kMaxGems ("" = empty)
  std::vector<SavedBuff> buffs;
  std::vector<SavedTimer> timers;
  std::vector<std::pair<std::string, int>> factions;  // earned standing
  std::string quests = "{}";
};

// A persisted player corpse being placed back into the world (server start).
struct CorpseInit {
  std::int64_t db_id = 0;
  std::int64_t owner_char_id = 0;
  std::string owner_name;
  int level = 1;
  Vec3 pos;
  float yaw = 0.0f;
  std::uint64_t coin = 0;
  std::vector<CorpseItem> items;  // origin = the slot code each came from
  std::uint64_t xp_lost = 0;
  float seconds_left = 3600.0f;
};

class ZoneSink {
 public:
  virtual ~ZoneSink() = default;
  ZoneSink() = default;
  ZoneSink(const ZoneSink&) = delete;
  ZoneSink& operator=(const ZoneSink&) = delete;

  // An encoded protocol message for one player (reliable when channel == 0).
  virtual void zone_send(const PlayerEntity& to, std::uint8_t channel, std::span<const std::uint8_t> packet) = 0;
  // A MoveCorrection has been decided for `to` (the sink sends it).
  virtual void zone_correction(const PlayerEntity& to, const char* reason) = 0;
  virtual void zone_violation(const PlayerEntity& who, const char* reason) = 0;
  // Persistence hooks. The zone's in-memory state is authoritative; these record it.
  virtual void zone_player_died(const PlayerEntity& /*p*/, const CorpseEntity& /*corpse*/) {}
  virtual void zone_corpse_looted(const CorpseEntity& /*corpse*/, const PlayerEntity& /*looter*/) {}
  virtual void zone_corpse_removed(const CorpseEntity& /*corpse*/) {}
  virtual void zone_character_changed(const PlayerEntity& /*p*/) {}
  // A resurrection was accepted: the corpse (no XP left to restore) and the owner together.
  virtual void zone_corpse_resurrected(const CorpseEntity& /*corpse*/, const PlayerEntity& /*owner*/) {}
  // A trade completed: both characters must be saved together, in one transaction.
  virtual void zone_trade_completed(const PlayerEntity& /*a*/, const PlayerEntity& /*b*/) {}
  // Items moved between two players' reach (a loot roll won straight into a pack, etc.): save both now.
  virtual void zone_items_awarded(const PlayerEntity& /*p*/) {}
};

enum class InputResult { Queued, Stale, QueueFull, UnknownEntity };

// A place in the world: a zone and a spot in it.
struct Destination {
  std::string zone;
  Vec3 pos;
  float yaw = 0.0f;
};

// A player leaving this zone (zone line, travel, a portal, respawning at a bind elsewhere). The game
// server moves them after the zone's tick.
struct ZoneTransfer {
  EntityId player = 0;
  std::int64_t char_id = 0;
  Destination to;
  std::string reason;
};

class Zone {
 public:
  static constexpr std::uint32_t kMaxInputsPerTick = 2;
  static constexpr std::uint32_t kCorrectionCooldownTicks = 10;
  static constexpr float kInterestHysteresis = 1.15f;
  static constexpr std::uint32_t kHeartbeatTicks = 20;

  Zone(const ZoneDef& def, const Content& content, float dt, std::uint64_t seed = 1);

  const ZoneDef& def() const { return def_; }
  Bounds bounds() const { return {def_.min_x, def_.min_z, def_.max_x, def_.max_z}; }
  std::size_t player_count() const { return players_.size(); }
  std::size_t mob_count() const { return mobs_.size(); }
  std::size_t corpse_count() const { return corpses_.size(); }
  std::size_t npc_count() const { return npcs_.size(); }
  std::uint32_t now() const { return tick_; }
  std::uint32_t ticks(float seconds) const;

  // Clamps the spawn position into bounds. Returns the new entity id.
  EntityId add_player(const ZonePlayerInit& init);
  // Closes loot, drops threat, and despawns for watchers. Dead players are revived at their
  // bind point first (their corpse stays), so what gets saved afterwards is a living character.
  void remove_player(EntityId id, ZoneSink& sink);
  // Sends ability list, skills, inventory, and stats (on entering the world).
  void send_initial_state(EntityId id, ZoneSink& sink);

  InputResult queue_input(EntityId id, std::uint32_t seq, const MoveIntent& intent, const Vec3& predicted);

  // Player intents (validated here; failures answer the client, they are not violations).
  void set_target(EntityId player, EntityId target, ZoneSink& sink);
  void set_auto_attack(EntityId player, bool enabled, ZoneSink& sink);
  void use_ability(EntityId player, std::string_view ability_id, EntityId target, ZoneSink& sink);
  void set_sitting(EntityId player, bool sitting, ZoneSink& sink);
  void respawn(EntityId player, ZoneSink& sink);
  void loot_open(EntityId player, EntityId corpse, ZoneSink& sink);
  void loot_take(EntityId player, EntityId corpse, std::uint8_t slot, ZoneSink& sink);
  void loot_close(EntityId player, EntityId corpse, ZoneSink& sink);
  // Spells (zone_spells.cpp).
  void cast_spell(EntityId player, int gem, EntityId target, ZoneSink& sink);
  void stop_cast(EntityId player, ZoneSink& sink);
  void memorize_spell(EntityId player, int gem, std::string_view spell_id, ZoneSink& sink);
  void scribe_spell(EntityId player, int slot, ZoneSink& sink);
  void trainer_open(EntityId player, EntityId npc, ZoneSink& sink);
  void trainer_buy(EntityId player, EntityId npc, std::string_view item_id, ZoneSink& sink);
  // Answers the pending offer with this id; replies to an older (replaced) offer are ignored.
  void rez_response(EntityId player, std::uint32_t offer_id, bool accept, ZoneSink& sink);
  // Pets (zone_pets.cpp).
  void pet_command(EntityId player, PetOrder order, EntityId target, ZoneSink& sink);
  // Items, vendors, bank, trade, loot rolls (zone_items.cpp, zone_trade.cpp).
  void move_item(EntityId player, SlotCode from, SlotCode to, int count, ZoneSink& sink);
  void destroy_item(EntityId player, SlotCode slot, std::string_view item_id, ZoneSink& sink);
  void use_item(EntityId player, SlotCode slot, EntityId target, ZoneSink& sink);
  void vendor_open(EntityId player, EntityId npc, ZoneSink& sink);
  void vendor_buy(EntityId player, EntityId npc, std::string_view item_id, int count, bool buyback, ZoneSink& sink);
  void vendor_sell(EntityId player, EntityId npc, SlotCode slot, int count, ZoneSink& sink);
  void bank_open(EntityId player, EntityId npc, ZoneSink& sink);
  void bank_coin(EntityId player, bool deposit, std::uint64_t amount, ZoneSink& sink);
  void trade_request(EntityId player, EntityId target, ZoneSink& sink);
  void trade_respond(EntityId player, EntityId from, bool accept, ZoneSink& sink);
  void trade_set_item(EntityId player, int index, SlotCode slot, ZoneSink& sink);
  void trade_set_coin(EntityId player, std::uint64_t coin, ZoneSink& sink);
  // trade_id and revision (from TradeState) must match: accepting an offer that changed since is ignored.
  void trade_accept(EntityId player, bool accept, ZoneSink& sink, std::uint32_t trade_id = 0, std::uint32_t revision = 0);
  void trade_cancel(EntityId player, const std::string& message, ZoneSink& sink);
  void loot_roll_choice(EntityId player, std::uint32_t roll_id, RollChoice choice, ZoneSink& sink);
  // Travel NPCs (zone_travel.cpp): teleporters, ferries and binders.
  void travel_open(EntityId player, EntityId npc, ZoneSink& sink);
  void travel_choose(EntityId player, EntityId npc, std::string_view dest, ZoneSink& sink);
  // Players leaving this zone since the last call (the game server moves them).
  std::vector<ZoneTransfer> take_transfers();
  // Where a destination ("bind", "safe_spot", a city or travel point id) is for this player.
  bool resolve_destination(const PlayerEntity& p, const std::string& destination, Destination& out) const;
  // Re-summons a pet that followed its owner here from another zone.
  void restore_pet(EntityId player, const std::string& pet_id, int level, float hp_fraction, ZoneSink& sink);
  const CollisionMap& collision() const { return collision_; }
  const NavGrid& nav() const { return nav_; }
  float ground_at(float x, float z) const { return def_.ground_at(x, z); }
  // The zone line a point is inside, if any.
  const ZoneLineDef* zone_line_at(float x, float z) const;
  bool has_player_corpses() const;
  // Factions (zone_factions.cpp).
  int standing(const PlayerEntity& p, std::string_view faction) const;
  Regard regard(const PlayerEntity& p, std::string_view faction) const;
  // Adds earned standing (a quest reward, a GM tool) and tells the player.
  void adjust_faction(EntityId player, std::string_view faction, int delta, ZoneSink& sink);
  // Quests (zone_quests.cpp).
  void npc_talk(EntityId player, EntityId npc, ZoneSink& sink);
  void npc_choose(EntityId player, EntityId npc, std::string_view key, ZoneSink& sink);
  void quest_abandon(EntityId player, std::string_view quest_id, ZoneSink& sink);
  // How many of an item the player carries in the pack and its bags.
  int carried(const PlayerEntity& p, std::string_view item_id) const;
  void set_loot_rule(EntityId player, LootRule rule, ZoneSink& sink);
  // Auctions (the board itself lives in the game server). Checks the player is alive, not trading,
  // and near an auctioneer; on failure answers the player and returns false.
  bool at_auctioneer(EntityId player, EntityId npc, ZoneSink& sink);
  // Removes a pack item for a listing after charging the fee. Returns false (and answers) if refused.
  bool take_for_auction(EntityId player, SlotCode slot, std::string_view item_id, std::uint64_t fee, ItemStackState& out, ZoneSink& sink);
  // Gives an auction item to a player's pack (and optionally charges `price`). False if no room/coin.
  bool give_from_auction(EntityId player, const ItemStackState& stack, std::uint64_t price, ZoneSink& sink);
  void add_coin(EntityId player, std::uint64_t amount, ZoneSink& sink);
  // Messages the player through the zone's system channel.
  void tell(EntityId player, const std::string& text, ZoneSink& sink);
  // Gives a stack to a player's pack (tests and GM tools). Returns false if it did not all fit.
  bool give(EntityId player, const std::string& item_id, int count, ZoneSink& sink);
  bool in_trade(EntityId player) const;
  // Seconds a summoned item has left (0 = permanent), for saving.
  float item_seconds_left(const ItemStackState& s) const;

  // What a player's beneficial effects and recast timers look like for saving.
  std::vector<SavedBuff> saved_buffs(const PlayerEntity& p) const;
  std::vector<SavedTimer> saved_timers(const PlayerEntity& p) const;

  // Places a persisted player corpse (server start).
  EntityId add_corpse(const CorpseInit& init);
  // Places a mob outside any spawn group (tests and future GM commands). It never respawns.
  // The zone keeps its own copy of `def`.
  EntityId spawn_mob(const MobDef& def, const Vec3& pos);

  void tick(std::uint32_t server_tick, ZoneSink& sink);

  PlayerEntity* find(EntityId id);
  const PlayerEntity* find(EntityId id) const;
  PlayerEntity* find_by_char(std::int64_t char_id);
  MobEntity* find_mob(EntityId id);
  CorpseEntity* find_corpse(EntityId id);
  void for_each_player(const std::function<void(const PlayerEntity&)>& fn) const;
  void for_each_mob(const std::function<void(const MobEntity&)>& fn) const;
  void for_each_corpse(const std::function<void(const CorpseEntity&)>& fn) const;
  // Appends ids of entities (any kind) within radius (inclusive) of pos.
  void players_near(const Vec3& pos, float radius, std::vector<EntityId>& out) const;
  // Test hook: gives a player XP (level-ups included).
  void grant_xp(EntityId player, std::uint64_t amount, ZoneSink& sink);
  // GM/test hook (and the path mob spells will use): puts a spell's lasting part on a player or
  // mob as if a level `caster_level` caster with no entity had cast it. Skips resists. Returns
  // what the stacking rules decided.
  StackAction force_effect(EntityId target, const AbilityDef& spell, int caster_level, ZoneSink& sink);
  NpcEntity* find_npc(EntityId id);
  void for_each_npc(const std::function<void(const NpcEntity&)>& fn) const;

 private:
  struct SpawnGroup {
    const MobDef* def = nullptr;
    MobSpawn spawn;
    int alive = 0;
    const MobDef* rare = nullptr;  // the named rare that sometimes takes a member's place
    bool rare_alive = false;
  };

  // --- movement and interest (zone.cpp)
  void apply_inputs(PlayerEntity& p, ZoneSink& sink);
  void update_interest(PlayerEntity& p, ZoneSink& sink);
  bool entity_position(EntityId id, Vec3& out) const;
  bool spawn_message(EntityId id, proto::EntitySpawn& s) const;
  void send_snapshots(ZoneSink& sink);
  void send_player_stats(PlayerEntity& p, ZoneSink& sink, bool force);
  void respawn_spot_watchers(EntityId who, ZoneSink& sink);

  // --- combat (zone_combat.cpp)
  void player_combat(PlayerEntity& p, ZoneSink& sink);
  // `counter` marks a riposte's return swing, which cannot itself be riposted. `weapon_mult` scales
  // the hit (the off hand's share for a dual-wield swing).
  void player_swing(PlayerEntity& p, MobEntity& m, ZoneSink& sink, bool counter, float weapon_mult = 1.0f, const ItemDef* weapon = nullptr);
  void mob_swing(MobEntity& m, PlayerEntity& p, ZoneSink& sink, bool counter);
  // `direct` is false for damage over time (it still breaks mez, but never roots). Damage dealt by
  // `via_pet` credits its owner (`attacker`) for loot and XP, while the threat goes to the pet.
  void damage_mob(MobEntity& m, PlayerEntity& attacker, int damage, double bonus_threat, ZoneSink& sink, bool direct = true,
                  MobEntity* via_pet = nullptr);
  void damage_player(PlayerEntity& p, int damage, EntityId source, const std::string& source_name, ZoneSink& sink);
  void kill_mob(MobEntity& m, PlayerEntity* killer, ZoneSink& sink);
  void kill_player(PlayerEntity& p, EntityId killer, const std::string& killer_name, ZoneSink& sink);
  void award_xp(PlayerEntity& p, std::uint64_t amount, ZoneSink& sink);
  // `save` = false inside a death, whose corpse write saves the character atomically.
  void recompute_level(PlayerEntity& p, ZoneSink& sink, bool announce, bool save = true);
  void regen(PlayerEntity& p);
  void try_skill_up(PlayerEntity& p, std::string_view skill, ZoneSink& sink);
  void unlock_skills(PlayerEntity& p);
  void send_skills(PlayerEntity& p, ZoneSink& sink);
  void send_inventory(PlayerEntity& p, ZoneSink& sink);
  void send_target_info(PlayerEntity& p, ZoneSink& sink);
  void ability_fail(PlayerEntity& p, const AbilityDef& a, const std::string& message, ZoneSink& sink);
  bool in_combat(const PlayerEntity& p) const;
  void mark_combat(PlayerEntity& p);
  void stand(PlayerEntity& p);
  void combat_event(const Vec3& where, EntityId source, EntityId target, CombatKind kind, std::uint32_t amount,
                    std::string_view ability, ZoneSink& sink, ResistType school = ResistType::None);
  void combat_text(PlayerEntity& p, const std::string& text, ZoneSink& sink);
  void engage(MobEntity& m, PlayerEntity& p, double threat, ZoneSink& sink);
  // Threat a player generates is scaled by its class and threat_pct buffs.
  double scaled_threat(const PlayerEntity& p, double threat) const;
  // Puts `id` (a player or pet) on the mob's hate list.
  void engage_id(MobEntity& m, EntityId id, double threat);
  // The melee damage-shield answer to an attacker that landed a hit on `defender` (player or pet).
  void reflect_damage_shield(MobEntity& attacker, EntityId defender, int shield, ZoneSink& sink);
  void call_assist(MobEntity& m, EntityId target);
  void drop_from_threat(EntityId player);
  std::uint32_t cooldown_ready(const PlayerEntity& p, std::string_view ability) const;
  void set_cooldown(PlayerEntity& p, std::string_view ability, std::uint32_t ready);
  float reach(const MobEntity& m) const;
  int mob_ac(const MobEntity& m) const;
  void refresh_stats(PlayerEntity& p);
  void relocate(PlayerEntity& p, const Vec3& pos, float yaw, ZoneSink& sink, const char* reason);
  // Relocates within this zone, or queues a transfer when the destination is elsewhere.
  void move_player_to(PlayerEntity& p, const Destination& to, ZoneSink& sink, const char* reason);
  void check_zone_lines(PlayerEntity& p);
  void tick_travel(ZoneSink& sink);
  MobEntity* spawn_pet_for(PlayerEntity& p, const PetDef& def, int level);
  // A body position pushed out of walls and onto the ground.
  Vec3 settle(Vec3 pos, float radius) const;

  // --- spells (zone_spells.cpp)
  // Checks shared by abilities and spells; on failure answers the client and returns false.
  bool can_act(PlayerEntity& p, const AbilityDef& a, ZoneSink& sink);
  // Resolves and validates the target; returns the entity the ability will land on (0 = none needed).
  bool resolve_target(PlayerEntity& p, const AbilityDef& a, EntityId requested, EntityId& out, std::string& why, bool landing);
  void pay_and_lock(PlayerEntity& p, const AbilityDef& a, ZoneSink& sink);
  void resolve_ability(PlayerEntity& p, const AbilityDef& a, EntityId target, ZoneSink& sink);
  void apply_detrimental(PlayerEntity& p, const AbilityDef& a, MobEntity& m, ZoneSink& sink);
  void apply_beneficial(PlayerEntity& caster, const AbilityDef& a, PlayerEntity& t, ZoneSink& sink);
  void heal(PlayerEntity& healer, PlayerEntity& t, int amount, const AbilityDef& a, CombatKind kind, ZoneSink& sink);
  void healer_threat(PlayerEntity& healer, const PlayerEntity& t, double amount, bool split, ZoneSink& sink);
  StackAction place_buff(std::vector<ActiveBuff>& host, ActiveBuff&& b);
  void resurrect(PlayerEntity& caster, const AbilityDef& a, EntityId corpse, ZoneSink& sink);
  void teleport(PlayerEntity& p, const std::string& destination, ZoneSink& sink);

  void cast_tick(PlayerEntity& p, ZoneSink& sink);
  void finish_cast(PlayerEntity& p, ZoneSink& sink);
  void interrupt_cast(PlayerEntity& p, CastResult result, const std::string& message, ZoneSink& sink);
  void tick_player_effects(PlayerEntity& p, ZoneSink& sink);
  void tick_mob_effects(MobEntity& m, ZoneSink& sink);
  void on_mob_damaged(MobEntity& m, bool direct, ZoneSink& sink);
  void send_buff_list(const PlayerEntity& to, EntityId of, ZoneSink& sink);
  void flush_buff_lists(ZoneSink& sink);
  void send_spellbook(PlayerEntity& p, ZoneSink& sink);
  void send_gems(PlayerEntity& p, ZoneSink& sink);
  void restore_saved(PlayerEntity& p, const ZonePlayerInit& init);
  const AbilityDef* any_ability(std::string_view id) const;  // spell or ability
  int mob_resist(const MobEntity& m, ResistType type) const;

  // --- pets (zone_pets.cpp)
  void summon_pet(PlayerEntity& p, const AbilityDef& a, const EffectDef& e, ZoneSink& sink);
  bool make_charmed(PlayerEntity& p, MobEntity& m, ZoneSink& sink);
  // Ends a charm: the mob is wild again and, if `hostile` and its charmer is here, attacks them.
  void break_charm(MobEntity& m, bool hostile, ZoneSink& sink);
  // Dismisses a summoned pet (it vanishes) or breaks a charm.
  void release_pet(MobEntity& m, bool hostile, const char* message, ZoneSink& sink);
  void pet_think(MobEntity& m, ZoneSink& sink);
  void pet_swing(MobEntity& pet, MobEntity& target, ZoneSink& sink);
  void mob_swing_pet(MobEntity& m, MobEntity& pet, ZoneSink& sink);
  void damage_pet(MobEntity& pet, int damage, EntityId source, ZoneSink& sink);
  void kill_pet(MobEntity& pet, ZoneSink& sink);
  // Takes a summoned pet out of the world (no corpse) and out of every hate list.
  void remove_pet_entity(MobEntity& pet, ZoneSink& sink);
  // A wild, living mob a player may attack (pets are never valid enemies).
  MobEntity* hostile_mob(EntityId id);
  void send_pet_status(PlayerEntity& p, ZoneSink& sink, bool force);
  void apply_to_pet(PlayerEntity& caster, const AbilityDef& a, MobEntity& pet, ZoneSink& sink);

  // --- class mechanics (zone_classes.cpp): songs, stealth, summons, tracking, locks
  void start_song(PlayerEntity& p, const AbilityDef& a, int gem, EntityId target, ZoneSink& sink);
  void stop_song(PlayerEntity& p, const std::string& message, ZoneSink& sink);
  void song_tick(PlayerEntity& p, ZoneSink& sink);
  // Ends hide and sneak (and, when `offensive`, invisibility) because the player acted.
  void break_stealth(PlayerEntity& p, bool offensive, ZoneSink& sink);
  void stand_from_feign(PlayerEntity& p, ZoneSink& sink);
  void feign_death(PlayerEntity& p, const AbilityDef& a, const EffectDef& e, ZoneSink& sink);
  void try_hide(PlayerEntity& p, const AbilityDef& a, ZoneSink& sink);
  void toggle_sneak(PlayerEntity& p, ZoneSink& sink);
  void threat_drop(PlayerEntity& p, const AbilityDef& a, const EffectDef& e, MobEntity* target, ZoneSink& sink);
  void track(PlayerEntity& p, const EffectDef& e, ZoneSink& sink);
  void pick_lock(PlayerEntity& p, const AbilityDef& a, EntityId npc, ZoneSink& sink);
  void summon_item(PlayerEntity& p, const EffectDef& e, ZoneSink& sink);
  void expire_items(PlayerEntity& p, ZoneSink& sink);
  // The race or model a player currently looks like ("" = their own).
  const std::string* illusion_of(const PlayerEntity& p) const;
  std::uint8_t player_flags2(const PlayerEntity& p) const;
  std::uint8_t mob_flags2(const MobEntity& m) const;

  // --- quests (zone_quests.cpp)
  bool take_carried(PlayerEntity& p, std::string_view item_id, int count);
  std::vector<const QuestDef*> active_quest_defs(const PlayerEntity& p) const;
  void quest_say(PlayerEntity& p, const NpcDef& n, const std::string& text, ZoneSink& sink);
  void advance_at_npc(PlayerEntity& p, const NpcDef& n, ZoneSink& sink);
  void send_dialog(PlayerEntity& p, const NpcEntity& n, const std::string& text, ZoneSink& sink);
  void complete_quest(PlayerEntity& p, const NpcEntity& n, const QuestDef& q, ZoneSink& sink);
  void quest_on_kill(PlayerEntity& p, const MobDef& m, ZoneSink& sink);
  void send_journal(PlayerEntity& p, ZoneSink& sink);

  // --- factions (zone_factions.cpp)
  void faction_hits(PlayerEntity& p, const FactionDef& f, ZoneSink& sink);
  // A service NPC of a faction that thinks too little of the player says so and refuses.
  bool npc_refuses(PlayerEntity& p, const NpcDef& n, ZoneSink& sink);
  void send_factions(PlayerEntity& p, ZoneSink& sink);

  // --- mob specials (zone_specials.cpp)
  void mob_specials(MobEntity& m, EntityId target, ZoneSink& sink);
  void mob_on_hit(MobEntity& m, PlayerEntity& p, ZoneSink& sink);
  void mob_cast(MobEntity& m, const AbilityDef& a, PlayerEntity& p, ZoneSink& sink);
  void mob_say(const MobEntity& m, const std::string& text, ZoneSink& sink);
  void reset_specials(MobEntity& m, ZoneSink& sink);
  // A boss's adds leave with it (it died or reset). Safe inside the mob loop: nothing is erased.
  void dismiss_adds(MobEntity& m, ZoneSink& sink);

  // --- mobs (zone_mobs.cpp)
  void spawn_group_member(std::size_t group);
  void mob_think(MobEntity& m, ZoneSink& sink);
  void mob_move_toward(MobEntity& m, const Vec3& to, float speed, float stop_distance);
  void set_mob_state(MobEntity& m, AiState s);
  // The top of the mob's hate list that it can still fight (a living player or pet), or 0.
  EntityId mob_target(MobEntity& m);
  bool foe_position(EntityId id, Vec3& out) const;
  void process_respawns();

  // --- items (zone_items.cpp)
  struct VendorStockState {
    const ItemDef* item = nullptr;
    int count = -1;  // -1 = unlimited
    int max = -1;
    std::uint32_t restock_ticks = 0;
    std::uint32_t next_restock = 0;
  };
  struct VendorState {
    std::vector<VendorStockState> stock;
    std::vector<ItemStackState> buyback;  // newest last
  };
  struct LootRollState {
    std::uint32_t id = 0;
    EntityId corpse = 0;
    std::size_t index = 0;  // item on the corpse
    std::vector<RollEntry> entries;
    std::vector<bool> answered;
    std::uint32_t deadline = 0;
  };
  struct TradeSession {
    std::uint32_t id = 0;
    std::array<EntityId, 2> who{};
    std::array<TradeSide, 2> side{};
    bool open = false;  // false = invited, waiting for an answer
    std::uint32_t invite_expires = 0;
    std::uint32_t revision = 1;  // bumped on every offer change; an accept names the revision it saw
  };
  Wearer wearer_of(const PlayerEntity& p) const;
  MoveContext move_context(const PlayerEntity& p) const;
  void mark_inventory(PlayerEntity& p);
  // A worn item changed: stats, the character sheet, and what others see held.
  void gear_changed(PlayerEntity& p, ZoneSink& sink);
  void send_character_sheet(PlayerEntity& p, ZoneSink& sink, bool force);
  void broadcast_equipment(PlayerEntity& p, ZoneSink& sink);
  std::string held_item(const PlayerEntity& p, EquipSlot slot) const;
  const NpcEntity* npc_in_reach(const PlayerEntity& p, EntityId npc, const char* role) const;
  VendorState& vendor_state(const NpcEntity& n);
  void restock_vendors();
  void send_vendor_window(PlayerEntity& p, const NpcEntity& n, ZoneSink& sink);
  bool items_locked(PlayerEntity& p, ZoneSink& sink);  // trading or dead: answers and returns true
  void check_windows(PlayerEntity& p, ZoneSink& sink);  // closes vendor/bank/trade out of range
  void use_item_effect(PlayerEntity& p, const ItemDef& item, SlotCode slot, EntityId target, ZoneSink& sink);
  // Fires a weapon's proc on a mob it just hit.
  void try_proc(PlayerEntity& p, const ItemDef* weapon, MobEntity& m, ZoneSink& sink);
  const ItemDef* worn(const PlayerEntity& p, EquipSlot slot) const;

  // --- trade (zone_trade.cpp)
  TradeSession* trade_of(const PlayerEntity& p);
  void send_trade_state(TradeSession& t, ZoneSink& sink);
  void close_trade(TradeSession& t, bool completed, const std::string& message, ZoneSink& sink);
  void complete_trade(TradeSession& t, ZoneSink& sink);
  void tick_trades(ZoneSink& sink);

  // --- loot (zone_loot.cpp)
  CorpseEntity& make_mob_corpse(const MobEntity& m, std::int64_t loot_char);
  // The players sharing a kill's loot: everyone on the damage list still in the zone (until groups in
  // Phase 7), the top damager first, at most rules.economy.loot_party_max.
  std::vector<std::int64_t> loot_party(const MobEntity& m, std::int64_t top) const;
  void start_rolls(CorpseEntity& c, ZoneSink& sink);
  void finish_roll(std::uint32_t roll_id, ZoneSink& sink);
  void tick_rolls(ZoneSink& sink);
  bool may_take(const PlayerEntity& p, const CorpseEntity& c, const CorpseItem& it) const;
  // Owner looting their own corpse: put an item back where it was worn (or the pack).
  bool restore_item(PlayerEntity& p, CorpseItem& it);
  void send_loot_window(PlayerEntity& p, const CorpseEntity& c, ZoneSink& sink);
  void close_loot(PlayerEntity& p, ZoneSink& sink, const std::string& message);
  void remove_corpse(EntityId id, ZoneSink& sink);
  void expire_corpses(ZoneSink& sink);
  bool give_item(PlayerEntity& p, ItemStackState& stack);
  std::string item_name(const std::string& id) const;

  // Sends to every player within the combat event radius of `where`. Reuses scratch_near_.
  template <class M>
  void broadcast(const Vec3& where, const M& msg, ZoneSink& sink) {
    scratch_near_.clear();
    grid_.query(where.x, where.z, rules_.combat.event_radius, scratch_near_);
    proto::encode_packet_into(msg, send_buf_);
    for (EntityId id : scratch_near_)
      if (const PlayerEntity* p = find(id)) sink.zone_send(*p, M::kChannel, send_buf_);
  }
  template <class M>
  void send(ZoneSink& sink, const PlayerEntity& to, const M& msg) {
    proto::encode_packet_into(msg, send_buf_);
    sink.zone_send(to, M::kChannel, send_buf_);
  }

  EntityId new_id();

  ZoneDef def_;
  const Content& content_;
  const Rules& rules_;
  float dt_;
  Rng rng_;
  std::uint32_t tick_ = 0;
  EntityId next_id_ = 1;
  std::uint32_t next_rez_offer_ = 1;
  std::unordered_map<EntityId, PlayerEntity> players_;
  std::map<EntityId, MobEntity> mobs_;  // ordered: deterministic AI iteration under test
  std::map<EntityId, CorpseEntity> corpses_;
  std::map<EntityId, NpcEntity> npcs_;
  std::map<EntityId, VendorState> vendors_;
  std::map<std::uint32_t, TradeSession> trades_;
  std::uint32_t next_trade_ = 1;
  std::map<std::uint32_t, LootRollState> rolls_;
  std::uint32_t next_roll_ = 1;
  std::vector<SpawnGroup> groups_;
  SpatialHash grid_;
  CollisionMap collision_;
  NavGrid nav_;
  std::vector<ZoneTransfer> transfers_;
  struct Departure {
    EntityId player = 0;
    EntityId npc = 0;
    std::string dest;
    int fee = 0;
    std::uint32_t at_tick = 0;
  };
  std::vector<Departure> departures_;  // ferries waiting to sail
  int path_budget_ = 0;                // path searches left this tick

  // Scratch buffers reused every tick to keep the loop allocation-light.
  std::vector<EntityId> scratch_candidates_;
  std::vector<EntityId> scratch_known_;
  std::vector<EntityId> scratch_diff_;
  std::vector<EntityId> scratch_near_;
  std::vector<EntityId> scratch_dead_;
  std::vector<std::pair<std::uint32_t, EntityId>> scratch_due_;
  proto::Snapshot scratch_snapshot_;
  std::vector<std::uint8_t> send_buf_;
};

// "1 platinum, 2 gold, 3 silver, 4 copper" (10 of each makes one of the next).
std::string format_coin(std::uint64_t copper);

}  // namespace ef::game
