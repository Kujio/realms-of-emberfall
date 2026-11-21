#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "zone_support.hpp"

using Catch::Approx;

using namespace zt;

TEST_CASE("players in range see each other; far players do not", "[zone]") {
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  const auto b = zone.add_player(at(10, 0, "B"));
  const auto c = zone.add_player(at(300, 0, "C"));
  zone.tick(1, sink);
  CHECK(sink.spawned(a, b));
  CHECK(sink.spawned(b, a));
  CHECK_FALSE(sink.spawned(a, c));
  CHECK_FALSE(sink.spawned(c, a));
  REQUIRE(sink.last_snapshot.contains(a));
  CHECK(sink.last_snapshot[a].entities.size() == 1);
  CHECK(sink.last_snapshot[c].entities.empty());
}

TEST_CASE("interest uses hysteresis for despawn", "[zone]") {
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  const auto b = zone.add_player(at(45, 0, "B"));
  zone.tick(1, sink);
  REQUIRE(sink.spawned(a, b));
  // Walk B east until it is between r (50) and r*1.15 (57.5): still known.
  std::uint32_t seq = 0;
  auto* pb = zone.find(b);
  while (pb->pos.x < 54.0f) {
    zone.queue_input(b, ++seq, {1, 0, 0}, {pb->pos.x + 0.35f, 0, 0});
    zone.tick(seq + 1, sink);
  }
  CHECK_FALSE(sink.despawned(a, b));
  while (pb->pos.x < 60.0f) {
    zone.queue_input(b, ++seq, {1, 0, 0}, {pb->pos.x + 0.35f, 0, 0});
    zone.tick(seq + 1, sink);
  }
  CHECK(sink.despawned(a, b));
}

TEST_CASE("remove_player despawns for watchers", "[zone]") {
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  const auto b = zone.add_player(at(5, 5, "B"));
  zone.tick(1, sink);
  zone.remove_player(b, sink);
  CHECK(sink.despawned(a, b));
  CHECK(zone.player_count() == 1);
}

TEST_CASE("server simulates movement from intents and acks sequence numbers", "[zone]") {
  const MovementRules& rules = content().rules.movement;
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  float pred = 0;
  for (std::uint32_t s = 1; s <= 20; ++s) {
    pred += rules.run_speed * kDt;
    REQUIRE(zone.queue_input(a, s, {0, 1, 0}, {0, 0, pred}) == InputResult::Queued);
    zone.tick(s, sink);
  }
  CHECK(zone.find(a)->pos.z == Approx(rules.run_speed * 1.0f));
  CHECK(sink.last_snapshot[a].ack_seq == 20);
  CHECK(sink.corrections.empty());
  CHECK(sink.violations.empty());
  CHECK(zone.queue_input(a, 20, {0, 1, 0}, {}) == InputResult::Stale);
  CHECK(zone.queue_input(a, 3, {0, 1, 0}, {}) == InputResult::Stale);
}

TEST_CASE("flooding inputs cannot exceed run speed", "[zone]") {
  const MovementRules& rules = content().rules.movement;
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  std::uint32_t seq = 0;
  const int ticks = 100;
  for (int t = 0; t < ticks; ++t) {
    for (int i = 0; i < 10; ++i) zone.queue_input(a, ++seq, {0, 1, 0}, {0, 0, 0});
    zone.tick(static_cast<std::uint32_t>(t), sink);
  }
  const float max_legal = rules.run_speed * kDt * (ticks + static_cast<float>(rules.input_budget_max));
  CHECK(zone.find(a)->pos.z <= max_legal);
}

TEST_CASE("teleport claims are flagged and corrected", "[zone]") {
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  zone.queue_input(a, 1, {0, 1, 0}, {0, 0, 0.35f});
  zone.tick(1, sink);
  zone.queue_input(a, 2, {0, 1, 0}, {0, 0, 150.0f});  // claims to be 150 m away
  zone.tick(2, sink);
  REQUIRE(sink.violations.size() == 1);
  CHECK(sink.violations[0].second == "teleport");
  REQUIRE(sink.corrections.size() == 1);
  CHECK(zone.find(a)->pos.z == Approx(0.70f));  // server position is unaffected by the claim
}

TEST_CASE("gradual desync triggers a correction without a violation", "[zone]") {
  const MovementRules& rules = content().rules.movement;
  Zone zone(test_zone(), content(), kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  // Client predicts moving forward while sending zero intents (e.g. bad prediction code).
  float pred = 0;
  for (std::uint32_t s = 1; s <= 10; ++s) {
    pred += rules.run_speed * kDt;
    zone.queue_input(a, s, {0, 0, 0}, {0, 0, pred});
    zone.tick(s, sink);
  }
  CHECK(sink.violations.empty());
  REQUIRE_FALSE(sink.corrections.empty());
  CHECK(sink.corrections[0].second == "desync");
}

TEST_CASE("spawn positions are clamped into bounds", "[zone]") {
  Zone zone(test_zone(), content(), kDt);
  const auto a = zone.add_player(at(9999, -9999, "A"));
  CHECK(zone.find(a)->pos.x == 500.0f);
  CHECK(zone.find(a)->pos.z == -500.0f);
}
