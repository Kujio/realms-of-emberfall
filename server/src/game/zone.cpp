#include "game/zone.hpp"

#include <algorithm>
#include <iterator>

namespace ef::game {

Zone::Zone(const ZoneDef& def, const MovementRules& rules, float dt)
    : def_(def), rules_(rules), dt_(dt), grid_(def.interest_radius) {}

EntityId Zone::add_player(const ZonePlayerInit& init) {
  const EntityId id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  PlayerEntity p;
  p.id = id;
  p.session = init.session;
  p.char_id = init.char_id;
  p.name = init.name;
  p.race = init.race;
  p.class_id = init.class_id;
  p.level = init.level;
  const Bounds b = bounds();
  p.pos = {std::clamp(init.pos.x, b.min_x, b.max_x), def_.ground_y, std::clamp(init.pos.z, b.min_z, b.max_z)};
  p.yaw = wrap_angle(init.yaw);
  p.budget = InputBudget(rules_.input_budget_max);
  grid_.insert(id, p.pos.x, p.pos.z);
  players_.emplace(id, std::move(p));
  return id;
}

void Zone::remove_player(EntityId id, ZoneSink& sink) {
  if (players_.erase(id) == 0) return;
  grid_.remove(id);
  for (auto& [other_id, other] : players_) {
    auto it = std::lower_bound(other.known.begin(), other.known.end(), id);
    if (it != other.known.end() && *it == id) {
      other.known.erase(it);
      sink.zone_despawn(other, id);
    }
  }
}

InputResult Zone::queue_input(EntityId id, std::uint32_t seq, const MoveIntent& intent, const Vec3& predicted) {
  auto* p = find(id);
  if (!p) return InputResult::UnknownEntity;
  if (p->has_seq && !seq_newer(seq, p->last_queued_seq)) return InputResult::Stale;
  if (p->queue_size >= PlayerEntity::kQueueCap) return InputResult::QueueFull;
  const std::size_t slot = (p->queue_head + p->queue_size) % PlayerEntity::kQueueCap;
  p->queue[slot] = PendingInput{seq, intent, predicted};
  ++p->queue_size;
  p->last_queued_seq = seq;
  p->has_seq = true;
  return InputResult::Queued;
}

void Zone::apply_inputs(PlayerEntity& p, ZoneSink& sink) {
  p.budget.on_tick();
  if (p.check_cooldown > 0) --p.check_cooldown;
  const Bounds b = bounds();
  const char* correction = nullptr;
  for (std::uint32_t n = 0; n < kMaxInputsPerTick && p.queue_size > 0 && p.budget.try_consume(); ++n) {
    const PendingInput in = p.queue[p.queue_head];
    p.queue_head = (p.queue_head + 1) % PlayerEntity::kQueueCap;
    --p.queue_size;

    const Vec3 next = step_position(p.pos, in.intent, rules_.run_speed, dt_, b, def_.ground_y);
    if (p.check_cooldown == 0) {
      if (p.has_pred) {
        const std::uint32_t steps = in.seq - p.last_pred_seq;
        if (!is_plausible_displacement(p.last_pred, in.predicted, rules_.run_speed, dt_, steps, rules_.teleport_tolerance)) {
          sink.zone_violation(p, "teleport");
          correction = "teleport";
        }
      }
      if (!correction && exceeds_desync(next, in.predicted, rules_.desync_tolerance)) correction = "desync";
    }
    p.pos = next;
    p.yaw = sanitize_intent(in.intent).yaw;
    p.last_applied_seq = in.seq;
    p.last_pred = in.predicted;
    p.last_pred_seq = in.seq;
    p.has_pred = true;
  }
  if (correction) {
    sink.zone_correction(p, correction);
    p.has_pred = false;
    p.check_cooldown = kCorrectionCooldownTicks;
  }
}

void Zone::update_interest(PlayerEntity& p, ZoneSink& sink) {
  const float r = def_.interest_radius;
  const float r_out = r * kInterestHysteresis;
  scratch_candidates_.clear();
  grid_.query(p.pos.x, p.pos.z, r_out, scratch_candidates_);

  scratch_known_.clear();
  for (EntityId cid : scratch_candidates_) {
    if (cid == p.id) continue;
    const auto& other = players_.at(cid);
    const bool already = std::binary_search(p.known.begin(), p.known.end(), cid);
    if (already || horizontal_distance_sq(p.pos, other.pos) <= r * r) scratch_known_.push_back(cid);
  }
  std::sort(scratch_known_.begin(), scratch_known_.end());

  // Despawn: known but no longer visible.
  scratch_diff_.clear();
  std::set_difference(p.known.begin(), p.known.end(), scratch_known_.begin(), scratch_known_.end(), std::back_inserter(scratch_diff_));
  for (EntityId gone : scratch_diff_) sink.zone_despawn(p, gone);

  // Spawn: newly visible.
  scratch_diff_.clear();
  std::set_difference(scratch_known_.begin(), scratch_known_.end(), p.known.begin(), p.known.end(), std::back_inserter(scratch_diff_));
  for (EntityId added : scratch_diff_) sink.zone_spawn(p, players_.at(added));

  p.known.swap(scratch_known_);
}

void Zone::tick(std::uint32_t server_tick, ZoneSink& sink) {
  for (auto& [id, p] : players_) {
    apply_inputs(p, sink);
    grid_.update(id, p.pos.x, p.pos.z);
  }
  for (auto& [id, p] : players_) update_interest(p, sink);

  auto& snap = scratch_snapshot_;
  for (auto& [id, p] : players_) {
    snap.server_tick = server_tick;
    snap.ack_seq = p.last_applied_seq;
    snap.self_x = p.pos.x;
    snap.self_y = p.pos.y;
    snap.self_z = p.pos.z;
    snap.entities.clear();
    for (EntityId kid : p.known) {
      const auto& o = players_.at(kid);
      snap.entities.push_back(proto::EntityState{o.id, o.pos.x, o.pos.y, o.pos.z, o.yaw});
    }
    sink.zone_snapshot(p, snap);
  }
}

PlayerEntity* Zone::find(EntityId id) {
  auto it = players_.find(id);
  return it == players_.end() ? nullptr : &it->second;
}

const PlayerEntity* Zone::find(EntityId id) const {
  auto it = players_.find(id);
  return it == players_.end() ? nullptr : &it->second;
}

void Zone::for_each_player(const std::function<void(const PlayerEntity&)>& fn) const {
  for (const auto& [id, p] : players_) fn(p);
}

void Zone::players_near(const Vec3& pos, float radius, std::vector<EntityId>& out) const {
  grid_.query(pos.x, pos.z, radius, out);
}

}  // namespace ef::game
