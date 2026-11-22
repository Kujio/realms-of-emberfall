// Zone mobs: spawning, respawning, and driving the AI state machine (mob_ai.hpp).
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

float yaw_toward(float dx, float dz) { return std::atan2(-dx, -dz); }  // forward is -Z at yaw 0

}  // namespace

void Zone::spawn_group_member(std::size_t g) {
  auto& grp = groups_[g];
  const float ang = static_cast<float>(rng_.uniform()) * 2.0f * kPi;
  const float r = grp.spawn.radius * std::sqrt(static_cast<float>(rng_.uniform()));
  const Bounds b = bounds();
  MobEntity m;
  m.id = new_id();
  m.def = grp.def;
  if (grp.rare && !grp.rare_alive && rng_.chance(grp.spawn.rare_chance)) {
    m.def = grp.rare;
    m.rare = true;
    grp.rare_alive = true;
  }
  m.spawn_index = g;
  m.home = settle({std::clamp(grp.spawn.x + std::cos(ang) * r, b.min_x, b.max_x), 0.0f,
                   std::clamp(grp.spawn.z + std::sin(ang) * r, b.min_z, b.max_z)},
                  kMobBodyRadius);
  m.pos = m.home;
  m.dest = m.home;
  m.yaw = static_cast<float>(rng_.uniform()) * 2.0f * kPi - kPi;
  m.stats = mob_stats(*m.def, rules_.mob_scaling);
  m.hp = m.stats.max_hp;
  m.state_tick = tick_;
  m.changed_tick = tick_;
  m.next_wander_tick = tick_ + ticks(static_cast<float>(rng_.range(4, 15)));
  grid_.insert(m.id, m.pos.x, m.pos.z);
  ++grp.alive;
  mobs_.emplace(m.id, std::move(m));
}

EntityId Zone::spawn_mob(const MobDef& def, const Vec3& pos) {
  const Bounds b = bounds();
  MobEntity m;
  m.id = new_id();
  m.owned_def = std::make_shared<const MobDef>(def);  // the caller's definition may not outlive the mob
  m.def = m.owned_def.get();
  m.home = settle({std::clamp(pos.x, b.min_x, b.max_x), 0.0f, std::clamp(pos.z, b.min_z, b.max_z)}, kMobBodyRadius);
  m.pos = m.home;
  m.dest = m.home;
  m.stats = mob_stats(def, rules_.mob_scaling);
  m.hp = m.stats.max_hp;
  m.state_tick = tick_;
  m.changed_tick = tick_;
  m.next_wander_tick = tick_ + ticks(10.0f);
  const EntityId id = m.id;
  grid_.insert(id, m.pos.x, m.pos.z);
  mobs_.emplace(id, std::move(m));
  return id;
}

void Zone::set_mob_state(MobEntity& m, AiState s) {
  const AiState old = m.state;
  m.state = s;
  m.state_tick = tick_;
  m.changed_tick = tick_;
  switch (s) {
    case AiState::Idle:
      m.next_wander_tick = tick_ + ticks(static_cast<float>(rng_.range(6, 18)));
      m.dest = m.pos;
      if (old == AiState::Leash) {
        // Reset after evading home, unless its foes simply vanished (feign death, evade): then it
        // walked home as it was.
        if (!m.forgot) m.hp = m.stats.max_hp;
        m.fled = false;
        m.forgot = false;
      }
      break;
    case AiState::Wander: {
      const float ang = static_cast<float>(rng_.uniform()) * 2.0f * kPi;
      const float r = m.def->wander_radius * std::sqrt(static_cast<float>(rng_.uniform()));
      const Bounds b = bounds();
      m.dest = {std::clamp(m.home.x + std::cos(ang) * r, b.min_x, b.max_x), 0.0f,
                std::clamp(m.home.z + std::sin(ang) * r, b.min_z, b.max_z)};
      if (!nav_.walkable(m.dest.x, m.dest.z)) m.dest = m.home;  // wandering into a wall: stay put
      break;
    }
    case AiState::Leash:
      m.threat.clear();
      m.damage_by.clear();
      m.stunned_until = 0;
      m.dest = m.home;
      break;
    case AiState::Flee:
      m.fled = true;
      break;
    case AiState::Chase:
    case AiState::Aggro:
      if (old == AiState::Idle || old == AiState::Wander) m.swing_ready_tick = std::max(m.swing_ready_tick, tick_ + ticks(0.5f));
      break;
    case AiState::Dead:
    case AiState::Respawn:
      break;
  }
}

EntityId Zone::mob_target(MobEntity& m) {
  for (;;) {
    const EntityId id = m.threat.top();
    if (id == 0) return 0;
    if (const PlayerEntity* p = find(id); p && !p->dead && !p->feigned) return id;
    if (const MobEntity* pet = find_mob(id); pet && pet->alive() && pet->is_pet() && pet->id != m.id) return id;
    m.threat.remove(id);
  }
}

bool Zone::foe_position(EntityId id, Vec3& out) const {
  if (const PlayerEntity* p = find(id)) {
    out = p->pos;
    return true;
  }
  if (auto it = mobs_.find(id); it != mobs_.end() && it->second.alive()) {
    out = it->second.pos;
    return true;
  }
  return false;
}

void Zone::mob_move_toward(MobEntity& m, const Vec3& to, float speed, float stop_distance) {
  if ((control_bits(m.buffs) & kControlRoot) != 0) speed = 0.0f;  // rooted: turn to face, but stay put
  speed *= move_speed_multiplier(m.buffs);                          // snares
  Vec3 goal = to;
  float stop = stop_distance;
  // Walls in the way: follow a path (refreshed when the goal moves or it grows old), else go straight.
  if (!nav_.empty() && speed > 0.0f && horizontal_distance_sq(m.pos, to) > (stop_distance + 0.5f) * (stop_distance + 0.5f) &&
      !nav_.line_walkable(m.pos, to)) {
    const bool stale = m.path_next >= m.path.size() || horizontal_distance_sq(m.path_goal, to) > 9.0f || tick_ - m.path_tick > ticks(3.0f);
    if (stale && path_budget_ > 0) {
      --path_budget_;
      m.path_goal = to;
      m.path_tick = tick_;
      m.path_next = 0;
      if (!nav_.find_path(m.pos, to, m.path)) m.path.clear();
    }
    while (m.path_next < m.path.size() && horizontal_distance_sq(m.pos, m.path[m.path_next]) < 0.25f) ++m.path_next;
    if (m.path_next < m.path.size() && m.path_next + 1 < m.path.size()) {
      goal = m.path[m.path_next];
      stop = 0.0f;
    } else if (m.path_next < m.path.size()) {
      goal = m.path[m.path_next];
    }
  } else if (!m.path.empty()) {
    m.path.clear();
    m.path_next = 0;
  }
  const float dx = goal.x - m.pos.x;
  const float dz = goal.z - m.pos.z;
  const float d = std::sqrt(dx * dx + dz * dz);
  if (d > 1e-4f) m.yaw = yaw_toward(dx, dz);
  if (d <= stop + 1e-3f || speed <= 0.0f) return;
  const float step = std::min(speed * dt_, d - stop);
  m.pos = settle({m.pos.x + dx / d * step, 0.0f, m.pos.z + dz / d * step}, kMobBodyRadius);
  m.changed_tick = tick_;
}

void Zone::mob_think(MobEntity& m, ZoneSink& sink) {
  const auto& sc = rules_.mob_scaling;
  const std::uint8_t ctl = control_bits(m.buffs);
  // Stunned and mesmerized mobs do nothing at all (and notice nothing).
  const bool stunned = m.stunned_until > tick_ || (ctl & kControlMez) != 0;
  const bool calm = m.state == AiState::Idle || m.state == AiState::Wander;

  // Proximity aggro (staggered): the first living, non-trivial player in range.
  if (calm && !stunned && m.def->aggro_radius > 0.0f && (tick_ + m.id) % static_cast<std::uint32_t>(sc.aggro_scan_ticks) == 0) {
    scratch_near_.clear();
    grid_.query(m.pos.x, m.pos.z, m.def->aggro_radius, scratch_near_);
    PlayerEntity* pick = nullptr;
    const float sneak_r = m.def->aggro_radius * rules_.stealth.sneak_aggro_mult;
    for (EntityId id : scratch_near_) {
      PlayerEntity* p = find(id);
      if (!p || p->dead || p->stealthed() || con_color(p->level, m.def->level, rules_.con) == Con::Grey) continue;
      // Only foes: a faction that does not hate the player leaves them be (guards and the like).
      if (!m.def->faction.empty() && content_.find_faction(m.def->faction) && !attacks_on_sight(regard(*p, m.def->faction))) continue;
      // Sneaking players have to come closer before they are noticed.
      if (p->sneaking && horizontal_distance(p->pos, m.pos) > sneak_r) continue;
      pick = p;
      break;
    }
    if (pick) engage(m, *pick, 1.0, sink);  // after the loop: engage() reuses scratch_near_
  }

  const EntityId tid = m.state == AiState::Leash ? 0 : mob_target(m);
  Vec3 tpos;
  const bool has_t = tid != 0 && foe_position(tid, tpos);
  const float r = reach(m);
  if (!stunned && (ctl & kControlFear) != 0) {
    // Feared: run from whoever cast it (or the top of the threat list), never attack, and keep
    // the current AI state for when the fear ends.
    Vec3 from = has_t ? tpos : m.home;
    for (const auto& b : m.buffs)
      if ((b.control() & kControlFear) != 0)
        if (const PlayerEntity* c = find(b.caster)) from = c->pos;
    const float dx = m.pos.x - from.x;
    const float dz = m.pos.z - from.z;
    const float d = std::max(0.001f, std::sqrt(dx * dx + dz * dz));
    mob_move_toward(m, {m.pos.x + dx / d * 5.0f, m.pos.y, m.pos.z + dz / d * 5.0f}, m.def->run_speed * 0.6f, 0.0f);
    m.last_melee_tick = tick_;  // fleeing in fear is not being kited
    return;
  }
  // A rooted mob cannot close the distance; it waits rather than giving up the chase.
  if ((ctl & kControlRoot) != 0) m.last_melee_tick = tick_;
  AiInputs in;
  in.state = m.state;
  in.hp_fraction = static_cast<float>(m.hp) / static_cast<float>(std::max(1, m.stats.max_hp));
  in.has_target = has_t;
  in.target_distance = has_t ? horizontal_distance(m.pos, tpos) : 0.0f;
  in.distance_from_spawn = horizontal_distance(m.pos, m.home);
  in.state_seconds = static_cast<float>(tick_ - m.state_tick) * dt_;
  if (has_t && in.target_distance <= r) m.last_melee_tick = tick_;
  in.seconds_since_melee = static_cast<float>(tick_ - m.last_melee_tick) * dt_;
  in.at_destination = horizontal_distance(m.pos, m.dest) < 0.5f;
  in.wander_due = m.def->wander_radius > 0.0f && tick_ >= m.next_wander_tick;
  in.fled_this_fight = m.fled;
  const AiParams params{r, m.def->leash_radius, m.def->flee_hp_pct, sc.flee_seconds, sc.chase_seconds, m.def->respawn_seconds};
  const AiState next = stunned ? m.state : next_ai_state(in, params);
  if (next == AiState::Leash && m.state != AiState::Leash && !m.def->specials.empty()) reset_specials(m, sink);
  if (next != m.state) set_mob_state(m, next);
  if (stunned) return;

  const float speed = m.def->run_speed;
  switch (m.state) {
    case AiState::Idle:
      if (m.hp < m.stats.max_hp) {  // calm mobs recover 1% per second
        m.hp_acc += 0.01f * static_cast<float>(m.stats.max_hp) * dt_;
        if (m.hp_acc >= 1.0f) {
          const int whole = static_cast<int>(m.hp_acc);
          m.hp = std::min(m.stats.max_hp, m.hp + whole);
          m.hp_acc -= static_cast<float>(whole);
          m.changed_tick = tick_;
        }
      }
      break;
    case AiState::Wander:
      mob_move_toward(m, m.dest, speed * 0.4f, 0.0f);
      break;
    case AiState::Chase:
      if (has_t) mob_move_toward(m, tpos, speed, r * 0.7f);
      if (has_t && !m.def->specials.empty()) mob_specials(m, tid, sink);  // casts and summons reach a kiter
      break;
    case AiState::Aggro:
      if (!has_t) break;
      mob_move_toward(m, tpos, speed, r * 0.7f);  // follow a target edging away
      if (!m.def->specials.empty()) {
        const EntityId mid = m.id;
        mob_specials(m, tid, sink);
        if (!find_mob(mid) || !m.alive()) break;
      }
      if (tick_ >= m.swing_ready_tick && horizontal_distance(m.pos, tpos) <= r) {
        m.swing_ready_tick = tick_ + std::max<std::uint32_t>(1, ticks(m.def->attack_delay * attack_delay_multiplier(m.buffs)));
        if (PlayerEntity* tp = find(tid)) mob_swing(m, *tp, sink, false);
        else if (MobEntity* pet = find_mob(tid)) mob_swing_pet(m, *pet, sink);
      }
      break;
    case AiState::Flee:
      if (has_t) {
        const float dx = m.pos.x - tpos.x;
        const float dz = m.pos.z - tpos.z;
        const float d = std::max(0.001f, std::sqrt(dx * dx + dz * dz));
        mob_move_toward(m, {m.pos.x + dx / d * 5.0f, m.pos.y, m.pos.z + dz / d * 5.0f}, speed * 0.6f, 0.0f);
      }
      break;
    case AiState::Leash:
      mob_move_toward(m, m.home, speed * 1.5f, 0.0f);
      break;
    case AiState::Dead:
    case AiState::Respawn:
      break;
  }
}

void Zone::process_respawns() {
  for (auto it = mobs_.begin(); it != mobs_.end();) {
    MobEntity& m = it->second;
    if (m.alive()) {
      ++it;
      continue;
    }
    if (m.spawn_index >= groups_.size()) {  // hand-placed: gone for good
      it = mobs_.erase(it);
      continue;
    }
    AiInputs in;
    in.state = m.state;
    in.state_seconds = static_cast<float>(tick_ - m.state_tick) * dt_;
    AiParams params;
    params.respawn_seconds = m.def->respawn_seconds;
    const AiState next = next_ai_state(in, params);
    if (next == AiState::Idle) {
      // A respawn is a new entity: clients saw the old one despawn when it died.
      const std::size_t g = m.spawn_index;
      if (m.rare) groups_[g].rare_alive = false;
      it = mobs_.erase(it);
      spawn_group_member(g);
      continue;
    }
    if (next != m.state) set_mob_state(m, next);
    ++it;
  }
}

}  // namespace ef::game
