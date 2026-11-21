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

#include "game/content.hpp"
#include "game/mob_ai.hpp"
#include "game/movement.hpp"
#include "game/rules.hpp"
#include "game/spatial_hash.hpp"
#include "game/threat.hpp"
#include "net/protocol.gen.hpp"

namespace ef::game {

enum class EntityKind : std::uint8_t { Player = 0, Mob = 1, Corpse = 2 };

// Bits in EntityState.flags / EntitySpawn.flags / PlayerStats.flags.
inline constexpr std::uint8_t kFlagDead = 1;
inline constexpr std::uint8_t kFlagInCombat = 2;
inline constexpr std::uint8_t kFlagSitting = 4;
inline constexpr std::uint8_t kFlagStunned = 8;
inline constexpr std::uint8_t kFlagFleeing = 16;
inline constexpr std::uint8_t kFlagEvading = 32;

// CombatEvent.kind values. The client turns these into combat-log lines and floating text.
enum class CombatKind : std::uint8_t {
  Hit = 0, Miss, Dodge, Parry, Riposte, Crit, AbilityHit, AbilityMiss, Taunt, TauntFail,
  Stun, StunFail, Heal, Buff, BuffFade, Death, Evade
};

// Chat channel ids carried in ChatMessage.channel.
inline constexpr std::uint8_t kChatSay = 0;
inline constexpr std::uint8_t kChatSystem = 1;
inline constexpr std::uint8_t kChatCombat = 2;

// Pack slots until bags and equipment arrive in Phase 5.
inline constexpr std::size_t kInventorySlots = 16;
// LootTake.slot value meaning "everything".
inline constexpr std::uint8_t kLootAll = 255;
inline constexpr std::size_t kMaxBuffs = 15;

struct ItemStackState {
  std::string item_id;
  int count = 0;
  bool empty() const { return count <= 0; }
};

struct SkillState {
  std::string id;
  int value = 0;
};

struct ActiveBuff {
  std::string ability_id;
  std::string name;
  BuffStat stat = BuffStat::DamageTakenPct;
  float value = 0.0f;
  std::uint32_t expires_tick = 0;
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
  DerivedStats stats;
  int hp = 1;
  int mana = 0;
  int endurance = 0;
  std::uint64_t coin = 0;  // copper
  std::array<ItemStackState, kInventorySlots> inventory{};
  std::vector<SkillState> skills;
  std::string bind_zone;
  Vec3 bind_pos;
  float bind_yaw = 0.0f;

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
  EntityId looting = 0;

  // Replication
  std::uint32_t changed_tick = 0;   // last tick position/hp/flags changed (snapshot filter)
  std::uint64_t stats_hash = 0;
  std::uint32_t stats_sent_tick = 0;
  std::vector<EntityId> known;      // sorted ids this player has spawned on its client
  std::unordered_map<EntityId, std::uint32_t> sent_tick;  // last tick each known entity's state went out

  // Effective skill: 0 if locked at the current level, else min(trained, cap).
  int skill(std::string_view skill_id) const;
  float buff_total(BuffStat stat) const;
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
  bool alive() const { return state != AiState::Dead && state != AiState::Respawn; }
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
  float scale = 1.0f;
  int level = 1;
  Vec3 pos;
  float yaw = 0.0f;
  std::uint64_t coin = 0;
  std::vector<ItemStackState> items;
  std::uint64_t xp_lost = 0;
  std::uint32_t expires_tick = 0;
  EntityId opened_by = 0;
  bool empty() const;
};

struct ZonePlayerInit {
  std::uint32_t session = 0;
  std::int64_t char_id = 0;
  std::string name;
  std::string race;
  std::string class_id;
  int level = 1;
  Vec3 pos;
  float yaw = 0.0f;
  std::uint64_t xp = 0;
  int hp = -1;  // -1 = full
  int mana = -1;
  int endurance = -1;
  std::uint64_t coin = 0;
  std::vector<std::pair<int, ItemStackState>> items;  // slot -> stack
  std::vector<SkillState> skills;
  std::string bind_zone;
  Vec3 bind_pos;
  float bind_yaw = 0.0f;
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
  std::vector<ItemStackState> items;
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
};

enum class InputResult { Queued, Stale, QueueFull, UnknownEntity };

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

 private:
  struct SpawnGroup {
    const MobDef* def = nullptr;
    MobSpawn spawn;
    int alive = 0;
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
  // `counter` marks a riposte's return swing, which cannot itself be riposted.
  void player_swing(PlayerEntity& p, MobEntity& m, ZoneSink& sink, bool counter);
  void mob_swing(MobEntity& m, PlayerEntity& p, ZoneSink& sink, bool counter);
  void damage_mob(MobEntity& m, PlayerEntity& attacker, int damage, double bonus_threat, ZoneSink& sink);
  void damage_player(PlayerEntity& p, int damage, const MobEntity& source, ZoneSink& sink);
  void kill_mob(MobEntity& m, PlayerEntity* killer, ZoneSink& sink);
  void kill_player(PlayerEntity& p, const MobEntity& killer, ZoneSink& sink);
  void award_xp(PlayerEntity& p, std::uint64_t amount, ZoneSink& sink);
  // `save` = false inside a death, whose corpse write saves the character atomically.
  void recompute_level(PlayerEntity& p, ZoneSink& sink, bool announce, bool save = true);
  void regen(PlayerEntity& p);
  void expire_buffs(PlayerEntity& p, ZoneSink& sink);
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
                    std::string_view ability, ZoneSink& sink);
  void combat_text(PlayerEntity& p, const std::string& text, ZoneSink& sink);
  void engage(MobEntity& m, PlayerEntity& p, double threat, ZoneSink& sink);
  void call_assist(MobEntity& m, EntityId target);
  void drop_from_threat(EntityId player);
  std::uint32_t cooldown_ready(const PlayerEntity& p, std::string_view ability) const;
  void set_cooldown(PlayerEntity& p, std::string_view ability, std::uint32_t ready);
  float reach(const MobEntity& m) const;

  // --- mobs (zone_mobs.cpp)
  void spawn_group_member(std::size_t group);
  void mob_think(MobEntity& m, ZoneSink& sink);
  void mob_move_toward(MobEntity& m, const Vec3& to, float speed, float stop_distance);
  void set_mob_state(MobEntity& m, AiState s);
  PlayerEntity* mob_target(MobEntity& m);
  void process_respawns();

  // --- loot (zone_loot.cpp)
  CorpseEntity& make_mob_corpse(const MobEntity& m, std::int64_t loot_char);
  void send_loot_window(PlayerEntity& p, const CorpseEntity& c, ZoneSink& sink);
  void close_loot(PlayerEntity& p, ZoneSink& sink, const std::string& message);
  void remove_corpse(EntityId id, ZoneSink& sink);
  void expire_corpses(ZoneSink& sink);
  bool give_item(PlayerEntity& p, ItemStackState& stack);

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
  std::unordered_map<EntityId, PlayerEntity> players_;
  std::map<EntityId, MobEntity> mobs_;  // ordered: deterministic AI iteration under test
  std::map<EntityId, CorpseEntity> corpses_;
  std::vector<SpawnGroup> groups_;
  SpatialHash grid_;

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
