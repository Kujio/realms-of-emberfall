// Mob AI state machine. next_ai_state() is a pure function of what the mob can observe; the zone
// applies the side effects of each state (movement, attacks, evade/reset, respawn).
//
//   Idle -> Wander (timer) -> Idle (arrived)
//   Idle/Wander -> Chase (gained a target) -> Aggro (in melee range) <-> Chase
//   Chase/Aggro -> Flee (low HP, once per fight) -> Chase (after flee time)
//   Chase/Aggro/Flee -> Leash (no target, too far from spawn, or kited too long) -> Idle (home)
//   any living state -> Dead (HP 0) -> Respawn (waiting) -> Idle (respawned)
#pragma once

#include <cstdint>

namespace ef::game {

enum class AiState : std::uint8_t { Idle, Wander, Aggro, Chase, Leash, Flee, Dead, Respawn };

struct AiInputs {
  AiState state = AiState::Idle;
  float hp_fraction = 1.0f;
  bool has_target = false;           // top of the threat list is alive and reachable
  float target_distance = 0.0f;
  float distance_from_spawn = 0.0f;
  float state_seconds = 0.0f;        // time spent in the current state
  float seconds_since_melee = 0.0f;  // time since the target was last in melee range
  bool at_destination = false;       // reached the wander point (Wander) or home (Leash)
  bool wander_due = false;           // idle timer elapsed and the mob wanders
  bool fled_this_fight = false;
};

struct AiParams {
  float melee_range = 4.0f;
  float leash_radius = 80.0f;
  float flee_hp_pct = 0.0f;  // 0 = never flees
  float flee_seconds = 6.0f;
  float chase_seconds = 30.0f;
  float respawn_seconds = 60.0f;
};

inline const char* ai_state_name(AiState s) {
  switch (s) {
    case AiState::Idle: return "idle";
    case AiState::Wander: return "wander";
    case AiState::Aggro: return "aggro";
    case AiState::Chase: return "chase";
    case AiState::Leash: return "leash";
    case AiState::Flee: return "flee";
    case AiState::Dead: return "dead";
    case AiState::Respawn: return "respawn";
  }
  return "?";
}

inline AiState next_ai_state(const AiInputs& in, const AiParams& p) {
  const bool alive_state = in.state != AiState::Dead && in.state != AiState::Respawn;
  if (alive_state && in.hp_fraction <= 0.0f) return AiState::Dead;
  switch (in.state) {
    case AiState::Dead: return AiState::Respawn;
    case AiState::Respawn: return in.state_seconds >= p.respawn_seconds ? AiState::Idle : AiState::Respawn;
    case AiState::Leash: return in.at_destination ? AiState::Idle : AiState::Leash;
    case AiState::Idle:
    case AiState::Wander:
      if (in.has_target) return in.target_distance <= p.melee_range ? AiState::Aggro : AiState::Chase;
      if (in.state == AiState::Wander) return in.at_destination ? AiState::Idle : AiState::Wander;
      return in.wander_due ? AiState::Wander : AiState::Idle;
    case AiState::Flee:
      if (!in.has_target || in.distance_from_spawn > p.leash_radius) return AiState::Leash;
      return in.state_seconds >= p.flee_seconds ? AiState::Chase : AiState::Flee;
    case AiState::Chase:
    case AiState::Aggro:
      if (!in.has_target || in.distance_from_spawn > p.leash_radius) return AiState::Leash;
      if (p.flee_hp_pct > 0.0f && !in.fled_this_fight && in.hp_fraction < p.flee_hp_pct) return AiState::Flee;
      if (in.target_distance <= p.melee_range) return AiState::Aggro;
      if (in.seconds_since_melee >= p.chase_seconds) return AiState::Leash;
      return AiState::Chase;
  }
  return AiState::Idle;
}

}  // namespace ef::game
