#pragma once

#include <cmath>

namespace ef::game {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline float horizontal_distance_sq(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x;
  const float dz = a.z - b.z;
  return dx * dx + dz * dz;
}

inline float horizontal_distance(const Vec3& a, const Vec3& b) { return std::sqrt(horizontal_distance_sq(a, b)); }

inline constexpr float kPi = 3.14159265358979323846f;

}  // namespace ef::game
