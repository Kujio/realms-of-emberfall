// A single zone instance: owns its players, simulates movement, and computes interest sets.
// Networking is abstracted behind ZoneSink so the zone is testable without sockets.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/content.hpp"
#include "game/movement.hpp"
#include "game/spatial_hash.hpp"
#include "net/protocol.gen.hpp"

namespace ef::game {

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
  std::uint8_t level = 1;
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

  // Interest: sorted ids this player currently has spawned on its client.
  std::vector<EntityId> known;
};

struct ZonePlayerInit {
  std::uint32_t session = 0;
  std::int64_t char_id = 0;
  std::string name;
  std::string race;
  std::string class_id;
  std::uint8_t level = 1;
  Vec3 pos;
  float yaw = 0.0f;
};

class ZoneSink {
 public:
  virtual ~ZoneSink() = default;
  ZoneSink() = default;
  ZoneSink(const ZoneSink&) = delete;
  ZoneSink& operator=(const ZoneSink&) = delete;

  virtual void zone_spawn(const PlayerEntity& to, const PlayerEntity& who) = 0;
  virtual void zone_despawn(const PlayerEntity& to, EntityId who) = 0;
  virtual void zone_snapshot(const PlayerEntity& to, const proto::Snapshot& snap) = 0;
  virtual void zone_correction(const PlayerEntity& to, const char* reason) = 0;
  virtual void zone_violation(const PlayerEntity& who, const char* reason) = 0;
};

enum class InputResult { Queued, Stale, QueueFull, UnknownEntity };

class Zone {
 public:
  static constexpr std::uint32_t kMaxInputsPerTick = 2;
  static constexpr std::uint32_t kCorrectionCooldownTicks = 10;
  static constexpr float kInterestHysteresis = 1.15f;

  Zone(const ZoneDef& def, const MovementRules& rules, float dt);

  const ZoneDef& def() const { return def_; }
  Bounds bounds() const { return {def_.min_x, def_.min_z, def_.max_x, def_.max_z}; }
  std::size_t player_count() const { return players_.size(); }

  // Clamps the spawn position into bounds. Returns the new entity id.
  EntityId add_player(const ZonePlayerInit& init);
  void remove_player(EntityId id, ZoneSink& sink);

  InputResult queue_input(EntityId id, std::uint32_t seq, const MoveIntent& intent, const Vec3& predicted);

  void tick(std::uint32_t server_tick, ZoneSink& sink);

  PlayerEntity* find(EntityId id);
  const PlayerEntity* find(EntityId id) const;
  void for_each_player(const std::function<void(const PlayerEntity&)>& fn) const;
  // Appends ids of players within radius (inclusive) of pos.
  void players_near(const Vec3& pos, float radius, std::vector<EntityId>& out) const;

 private:
  void apply_inputs(PlayerEntity& p, ZoneSink& sink);
  void update_interest(PlayerEntity& p, ZoneSink& sink);

  ZoneDef def_;
  MovementRules rules_;
  float dt_;
  EntityId next_id_ = 1;
  std::unordered_map<EntityId, PlayerEntity> players_;
  SpatialHash grid_;

  // Scratch buffers reused every tick to keep the loop allocation-light.
  std::vector<EntityId> scratch_candidates_;
  std::vector<EntityId> scratch_known_;
  std::vector<EntityId> scratch_diff_;
  proto::Snapshot scratch_snapshot_;
};

}  // namespace ef::game
