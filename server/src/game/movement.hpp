// Pure, unit-tested movement rules. No networking or I/O here.
//
// Model: clients send movement *intents* (a direction + facing) once per simulation step,
// tagged with a sequence number. The server applies each intent with step_position(); the
// client runs the exact same function for prediction and replays unacknowledged intents on
// reconciliation. Speed hacks are impossible by construction (the server picks the speed and
// dt), input flooding is capped by InputBudget, and a client whose own predicted positions
// jump implausibly (teleport) or drift too far from the server (desync) gets a hard correction.
#pragma once

#include <cstdint>

#include "game/math.hpp"
#include "game/world_geom.hpp"

namespace ef::game {

struct MoveIntent {
  float move_x = 0.0f;  // world-space direction, length <= 1
  float move_z = 0.0f;
  float yaw = 0.0f;     // facing, radians
};

struct Bounds {
  float min_x = 0.0f;
  float min_z = 0.0f;
  float max_x = 0.0f;
  float max_z = 0.0f;
};

// Wraps an angle into [-pi, pi]. Non-finite input yields 0.
float wrap_angle(float a);

// Clamps direction length to 1, zeros non-finite components, wraps yaw.
MoveIntent sanitize_intent(const MoveIntent& in);

// Applies one sanitized intent for `dt` seconds at `speed`, clamped to `bounds`, snapped to ground_y.
Vec3 step_position(const Vec3& pos, const MoveIntent& intent, float speed, float dt, const Bounds& bounds, float ground_y);

// The zone-aware step (Phase 6): moves, pushes the body (`radius`) out of the collision map, clamps to
// bounds, and sets y from the terrain. The client mirrors this in zone_geom.gd.
Vec3 step_position(const Vec3& pos, const MoveIntent& intent, float speed, float dt, const Bounds& bounds,
                   const CollisionMap& map, const Terrain& terrain, float radius);

// True if moving from `from` to `to` over `steps` simulation steps is physically possible.
bool is_plausible_displacement(const Vec3& from, const Vec3& to, float speed, float dt, std::uint32_t steps, float tolerance);

// True if the client's predicted position is too far from the authoritative one.
bool exceeds_desync(const Vec3& server, const Vec3& client_predicted, float tolerance);

// Token budget: one token per server tick, capped. Each applied movement intent costs one token,
// so a client can never average more than one step per tick no matter how fast it sends.
class InputBudget {
 public:
  explicit InputBudget(std::uint32_t max_tokens) : max_(max_tokens == 0 ? 1 : max_tokens), tokens_(1) {}
  void on_tick() {
    if (tokens_ < max_) ++tokens_;
  }
  bool try_consume() {
    if (tokens_ == 0) return false;
    --tokens_;
    return true;
  }
  std::uint32_t tokens() const { return tokens_; }

 private:
  std::uint32_t max_;
  std::uint32_t tokens_;
};

// True if `seq` is newer than `last` with wraparound (serial number arithmetic).
inline bool seq_newer(std::uint32_t seq, std::uint32_t last) {
  return seq != last && static_cast<std::uint32_t>(seq - last) < 0x80000000u;
}

}  // namespace ef::game
