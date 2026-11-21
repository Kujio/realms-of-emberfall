// Threat table and mob AI state machine (both pure).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "game/mob_ai.hpp"
#include "game/threat.hpp"

using namespace ef::game;

TEST_CASE("threat: damage orders the list, taunt jumps to the top", "[threat]") {
  ThreatTable t;
  CHECK(t.top() == 0);
  t.add(1, 100);
  t.add(2, 50);
  CHECK(t.top() == 1);
  t.add(2, 60);
  CHECK(t.top() == 2);
  CHECK(t.threat_of(2) == 110);
  CHECK(t.taunt(1));
  CHECK(t.top() == 1);
  CHECK(t.threat_of(1) == 111);
  CHECK_FALSE(t.taunt(1));  // already on top: no change
  CHECK(t.taunt(3));        // a fresh taunter goes straight to the top too
  CHECK(t.top() == 3);
  t.remove(3);
  CHECK(t.top() == 1);
  t.add(1, -1000);  // never negative
  CHECK(t.threat_of(1) == 0);
  CHECK(t.top() == 2);
  CHECK(t.size() == 2);
  t.clear();
  CHECK(t.empty());
}

TEST_CASE("threat: ties keep the earlier entry; healing threat splits", "[threat]") {
  ThreatTable t;
  t.add(5, 10);
  t.add(6, 10);
  CHECK(t.top() == 5);
  CHECK(heal_threat(100, 2) == 25.0);
  CHECK(heal_threat(100, 0) == 0.0);
  CHECK(heal_threat(0, 3) == 0.0);
}

namespace {

AiParams params() {
  AiParams p;
  p.melee_range = 4;
  p.leash_radius = 80;
  p.flee_hp_pct = 0.15f;
  p.flee_seconds = 6;
  p.chase_seconds = 30;
  p.respawn_seconds = 60;
  return p;
}

AiInputs fighting(AiState s, float dist) {
  AiInputs in;
  in.state = s;
  in.has_target = true;
  in.target_distance = dist;
  in.distance_from_spawn = 10;
  return in;
}

}  // namespace

TEST_CASE("AI: calm mobs wander on a timer and react to targets", "[ai]") {
  const auto p = params();
  AiInputs in;
  CHECK(next_ai_state(in, p) == AiState::Idle);
  in.wander_due = true;
  CHECK(next_ai_state(in, p) == AiState::Wander);
  in.state = AiState::Wander;
  in.wander_due = false;
  CHECK(next_ai_state(in, p) == AiState::Wander);
  in.at_destination = true;
  CHECK(next_ai_state(in, p) == AiState::Idle);
  in.has_target = true;
  in.target_distance = 20;
  CHECK(next_ai_state(in, p) == AiState::Chase);
  in.target_distance = 2;
  CHECK(next_ai_state(in, p) == AiState::Aggro);
}

TEST_CASE("AI: chase and melee follow range", "[ai]") {
  const auto p = params();
  CHECK(next_ai_state(fighting(AiState::Chase, 3), p) == AiState::Aggro);
  CHECK(next_ai_state(fighting(AiState::Aggro, 10), p) == AiState::Chase);
  CHECK(next_ai_state(fighting(AiState::Aggro, 1), p) == AiState::Aggro);
}

TEST_CASE("AI: leash when the target is gone, too far from home, or kiting forever", "[ai]") {
  const auto p = params();
  auto in = fighting(AiState::Chase, 10);
  in.has_target = false;
  CHECK(next_ai_state(in, p) == AiState::Leash);
  in = fighting(AiState::Aggro, 1);
  in.distance_from_spawn = 81;
  CHECK(next_ai_state(in, p) == AiState::Leash);
  in = fighting(AiState::Chase, 10);
  in.seconds_since_melee = 31;
  CHECK(next_ai_state(in, p) == AiState::Leash);
  // Leashing ignores everything until home.
  in = fighting(AiState::Leash, 1);
  CHECK(next_ai_state(in, p) == AiState::Leash);
  in.at_destination = true;
  CHECK(next_ai_state(in, p) == AiState::Idle);
}

TEST_CASE("AI: low health flees once, then returns to the fight", "[ai]") {
  const auto p = params();
  auto in = fighting(AiState::Aggro, 1);
  in.hp_fraction = 0.1f;
  CHECK(next_ai_state(in, p) == AiState::Flee);
  in.fled_this_fight = true;
  CHECK(next_ai_state(in, p) == AiState::Aggro);
  in = fighting(AiState::Flee, 8);
  in.state_seconds = 2;
  CHECK(next_ai_state(in, p) == AiState::Flee);
  in.state_seconds = 6;
  CHECK(next_ai_state(in, p) == AiState::Chase);
  in.distance_from_spawn = 100;
  CHECK(next_ai_state(in, p) == AiState::Leash);
  auto no_flee = params();
  no_flee.flee_hp_pct = 0;
  in = fighting(AiState::Aggro, 1);
  in.hp_fraction = 0.01f;
  CHECK(next_ai_state(in, no_flee) == AiState::Aggro);
}

TEST_CASE("AI: death, respawn timer, and back to idle", "[ai]") {
  const auto p = params();
  for (AiState s : {AiState::Idle, AiState::Wander, AiState::Chase, AiState::Aggro, AiState::Flee, AiState::Leash}) {
    auto in = fighting(s, 1);
    in.hp_fraction = 0;
    CHECK(next_ai_state(in, p) == AiState::Dead);
  }
  AiInputs in;
  in.state = AiState::Dead;
  CHECK(next_ai_state(in, p) == AiState::Respawn);
  in.state = AiState::Respawn;
  in.state_seconds = 59;
  CHECK(next_ai_state(in, p) == AiState::Respawn);
  in.state_seconds = 60;
  CHECK(next_ai_state(in, p) == AiState::Idle);
  CHECK(std::string(ai_state_name(AiState::Leash)) == "leash");
}
