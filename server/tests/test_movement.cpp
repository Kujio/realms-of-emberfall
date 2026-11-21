#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "game/movement.hpp"

using namespace ef::game;
using Catch::Approx;

namespace {
constexpr Bounds kBounds{-100.0f, -100.0f, 100.0f, 100.0f};
constexpr float kSpeed = 7.0f;
constexpr float kDt = 0.05f;
}  // namespace

TEST_CASE("sanitize_intent clamps direction length and wraps yaw", "[movement]") {
  auto s = sanitize_intent({10.0f, 0.0f, 7.0f});
  CHECK(s.move_x == Approx(1.0f));
  CHECK(s.move_z == Approx(0.0f));
  CHECK(s.yaw == Approx(7.0f - 2.0f * kPi));

  auto diag = sanitize_intent({1.0f, 1.0f, 0.0f});
  CHECK(std::hypot(diag.move_x, diag.move_z) == Approx(1.0f));

  auto small = sanitize_intent({0.3f, 0.4f, 0.0f});
  CHECK(small.move_x == Approx(0.3f));  // sub-unit input (walking) is preserved
}

TEST_CASE("sanitize_intent neutralizes non-finite input", "[movement]") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  auto s = sanitize_intent({nan, inf, nan});
  CHECK(s.move_x == 0.0f);
  CHECK(s.move_z == 0.0f);
  CHECK(s.yaw == 0.0f);
}

TEST_CASE("step_position moves at exactly run speed", "[movement]") {
  const Vec3 start{0, 0, 0};
  auto p = step_position(start, {0.0f, 1.0f, 0.0f}, kSpeed, kDt, kBounds, 0.0f);
  CHECK(p.z == Approx(kSpeed * kDt));
  // A speed-hacked direction vector of length 50 still moves only one step.
  auto hacked = step_position(start, {0.0f, 50.0f, 0.0f}, kSpeed, kDt, kBounds, 0.0f);
  CHECK(hacked.z == Approx(kSpeed * kDt));
}

TEST_CASE("step_position clamps to zone bounds and ground", "[movement]") {
  auto p = step_position({99.9f, 5.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, kSpeed, kDt, kBounds, 0.0f);
  CHECK(p.x == 100.0f);
  CHECK(p.y == 0.0f);
}

TEST_CASE("is_plausible_displacement flags teleports", "[movement]") {
  const Vec3 a{0, 0, 0};
  CHECK(is_plausible_displacement(a, {0, 0, kSpeed * kDt}, kSpeed, kDt, 1, 1.5f));
  CHECK_FALSE(is_plausible_displacement(a, {0, 0, 5.0f}, kSpeed, kDt, 1, 1.5f));
  // Several steps (lost packets) allow proportionally more distance.
  CHECK(is_plausible_displacement(a, {0, 0, 5.0f * kSpeed * kDt}, kSpeed, kDt, 5, 1.5f));
  CHECK_FALSE(is_plausible_displacement(a, {std::numeric_limits<float>::infinity(), 0, 0}, kSpeed, kDt, 1, 1.5f));
}

TEST_CASE("exceeds_desync uses horizontal distance", "[movement]") {
  CHECK_FALSE(exceeds_desync({0, 0, 0}, {1.0f, 50.0f, 1.0f}, 2.0f));
  CHECK(exceeds_desync({0, 0, 0}, {2.0f, 0.0f, 1.0f}, 2.0f));
  CHECK(exceeds_desync({0, 0, 0}, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 2.0f));
}

TEST_CASE("InputBudget caps the long-run input rate at one per tick", "[movement]") {
  InputBudget b(4);
  int applied = 0;
  // A cheating client sends 10 inputs every tick for 100 ticks.
  for (int tick = 0; tick < 100; ++tick) {
    b.on_tick();
    for (int i = 0; i < 10; ++i) applied += b.try_consume() ? 1 : 0;
  }
  CHECK(applied <= 101);
  CHECK(applied >= 99);
}

TEST_CASE("InputBudget banks a few inputs for jitter", "[movement]") {
  InputBudget b(4);
  for (int i = 0; i < 10; ++i) b.on_tick();
  CHECK(b.tokens() == 4);
}

TEST_CASE("seq_newer handles wraparound", "[movement]") {
  CHECK(seq_newer(2, 1));
  CHECK_FALSE(seq_newer(1, 2));
  CHECK_FALSE(seq_newer(5, 5));
  CHECK(seq_newer(1, 0xFFFFFFF0u));
}
