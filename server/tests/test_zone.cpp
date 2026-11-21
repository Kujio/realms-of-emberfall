#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>

#include "game/zone.hpp"

using namespace ef::game;
using Catch::Approx;

namespace {

struct RecordingSink final : ZoneSink {
  std::multimap<EntityId, EntityId> spawns;    // to -> who
  std::multimap<EntityId, EntityId> despawns;  // to -> who
  std::map<EntityId, ef::proto::Snapshot> last_snapshot;
  std::vector<std::pair<EntityId, std::string>> corrections;
  std::vector<std::pair<EntityId, std::string>> violations;

  void zone_spawn(const PlayerEntity& to, const PlayerEntity& who) override { spawns.emplace(to.id, who.id); }
  void zone_despawn(const PlayerEntity& to, EntityId who) override { despawns.emplace(to.id, who); }
  void zone_snapshot(const PlayerEntity& to, const ef::proto::Snapshot& s) override { last_snapshot[to.id] = s; }
  void zone_correction(const PlayerEntity& to, const char* r) override { corrections.emplace_back(to.id, r); }
  void zone_violation(const PlayerEntity& who, const char* r) override { violations.emplace_back(who.id, r); }

  bool spawned(EntityId to, EntityId who) const {
    auto [a, b] = spawns.equal_range(to);
    return std::any_of(a, b, [&](const auto& kv) { return kv.second == who; });
  }
  bool despawned(EntityId to, EntityId who) const {
    auto [a, b] = despawns.equal_range(to);
    return std::any_of(a, b, [&](const auto& kv) { return kv.second == who; });
  }
};

ZoneDef test_zone() {
  ZoneDef z;
  z.id = "test";
  z.name = "Test";
  z.min_x = -500;
  z.min_z = -500;
  z.max_x = 500;
  z.max_z = 500;
  z.interest_radius = 50;
  z.say_radius = 30;
  return z;
}

ZonePlayerInit at(float x, float z, const char* name) {
  ZonePlayerInit i;
  i.name = name;
  i.pos = {x, 0, z};
  return i;
}

constexpr float kDt = 0.05f;

}  // namespace

TEST_CASE("players in range see each other; far players do not", "[zone]") {
  Zone zone(test_zone(), MovementRules{}, kDt);
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
  Zone zone(test_zone(), MovementRules{}, kDt);
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
  Zone zone(test_zone(), MovementRules{}, kDt);
  RecordingSink sink;
  const auto a = zone.add_player(at(0, 0, "A"));
  const auto b = zone.add_player(at(5, 5, "B"));
  zone.tick(1, sink);
  zone.remove_player(b, sink);
  CHECK(sink.despawned(a, b));
  CHECK(zone.player_count() == 1);
}

TEST_CASE("server simulates movement from intents and acks sequence numbers", "[zone]") {
  MovementRules rules;
  Zone zone(test_zone(), rules, kDt);
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
  MovementRules rules;
  Zone zone(test_zone(), rules, kDt);
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
  MovementRules rules;
  Zone zone(test_zone(), rules, kDt);
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
  MovementRules rules;
  Zone zone(test_zone(), rules, kDt);
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
  Zone zone(test_zone(), MovementRules{}, kDt);
  const auto a = zone.add_player(at(9999, -9999, "A"));
  CHECK(zone.find(a)->pos.x == 500.0f);
  CHECK(zone.find(a)->pos.z == -500.0f);
}
