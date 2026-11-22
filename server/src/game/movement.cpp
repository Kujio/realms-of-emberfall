#include "game/movement.hpp"

#include <algorithm>
#include <cmath>

namespace ef::game {

float wrap_angle(float a) {
  if (!std::isfinite(a)) return 0.0f;
  a = std::remainder(a, 2.0f * kPi);  // result in [-pi, pi]
  return a;
}

MoveIntent sanitize_intent(const MoveIntent& in) {
  MoveIntent out;
  out.move_x = std::isfinite(in.move_x) ? in.move_x : 0.0f;
  out.move_z = std::isfinite(in.move_z) ? in.move_z : 0.0f;
  const float len_sq = out.move_x * out.move_x + out.move_z * out.move_z;
  if (len_sq > 1.0f) {
    const float inv = 1.0f / std::sqrt(len_sq);
    out.move_x *= inv;
    out.move_z *= inv;
  }
  out.yaw = wrap_angle(in.yaw);
  return out;
}

Vec3 step_position(const Vec3& pos, const MoveIntent& intent, float speed, float dt, const Bounds& b, float ground_y) {
  const MoveIntent s = sanitize_intent(intent);
  Vec3 out;
  out.x = std::clamp(pos.x + s.move_x * speed * dt, b.min_x, b.max_x);
  out.z = std::clamp(pos.z + s.move_z * speed * dt, b.min_z, b.max_z);
  out.y = ground_y;
  return out;
}

Vec3 step_position(const Vec3& pos, const MoveIntent& intent, float speed, float dt, const Bounds& b, const CollisionMap& map,
                   const Terrain& terrain, float radius) {
  const MoveIntent s = sanitize_intent(intent);
  Vec3 out{pos.x + s.move_x * speed * dt, 0.0f, pos.z + s.move_z * speed * dt};
  if (!map.empty()) out = map.resolve(out, radius);
  out.x = std::clamp(out.x, b.min_x, b.max_x);
  out.z = std::clamp(out.z, b.min_z, b.max_z);
  out.y = terrain.height(out.x, out.z);
  return out;
}

bool is_plausible_displacement(const Vec3& from, const Vec3& to, float speed, float dt, std::uint32_t steps, float tolerance) {
  if (!std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z)) return false;
  const float max_dist = speed * dt * static_cast<float>(std::max<std::uint32_t>(steps, 1)) * tolerance + 0.01f;
  return horizontal_distance_sq(from, to) <= max_dist * max_dist;
}

bool exceeds_desync(const Vec3& server, const Vec3& client_predicted, float tolerance) {
  if (!std::isfinite(client_predicted.x) || !std::isfinite(client_predicted.z)) return true;
  return horizontal_distance_sq(server, client_predicted) > tolerance * tolerance;
}

}  // namespace ef::game
