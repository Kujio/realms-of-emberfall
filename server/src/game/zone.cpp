#include "game/zone.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ef::game {

namespace {

std::uint8_t pct(int hp, int max_hp) {
  if (max_hp <= 0 || hp <= 0) return 0;
  return static_cast<std::uint8_t>(std::clamp((hp * 100 + max_hp - 1) / max_hp, 1, 100));
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001B3ULL; }

}  // namespace

int PlayerEntity::skill(std::string_view skill_id) const {
  // The effective value: 0 while the skill is locked at this level (e.g. after losing a level),
  // otherwise the trained value clamped to the current cap. Training is kept for later.
  const SkillCap* cap = class_def ? class_def->skill(skill_id) : nullptr;
  const int c = cap ? skill_cap(*cap, level) : 0;
  if (c <= 0) return 0;
  for (const auto& s : skills)
    if (s.id == skill_id) return std::min(s.value, c);
  return 0;
}

float PlayerEntity::buff_total(BuffStat stat) const {
  float total = 0.0f;
  for (const auto& b : buffs)
    if (b.stat == stat) total += b.value;
  return total;
}

bool CorpseEntity::empty() const {
  return coin == 0 && std::all_of(items.begin(), items.end(), [](const ItemStackState& s) { return s.empty(); });
}

std::string format_coin(std::uint64_t copper) {
  const std::uint64_t pp = copper / 1000;
  const std::uint64_t gp = (copper / 100) % 10;
  const std::uint64_t sp = (copper / 10) % 10;
  const std::uint64_t cp = copper % 10;
  std::string out;
  auto add = [&](std::uint64_t n, const char* unit) {
    if (n == 0) return;
    if (!out.empty()) out += ", ";
    out += std::to_string(n) + " " + unit;
  };
  add(pp, "platinum");
  add(gp, "gold");
  add(sp, "silver");
  add(cp, "copper");
  return out.empty() ? "no coin" : out;
}

Zone::Zone(const ZoneDef& def, const Content& content, float dt, std::uint64_t seed)
    : def_(def), content_(content), rules_(content.rules), dt_(dt), rng_(seed), grid_(def.interest_radius) {
  for (const auto& sp : def_.spawns) {
    const MobDef* md = content_.find_mob(sp.mob);
    if (!md) continue;  // content validation rejects this; be defensive anyway
    groups_.push_back({md, sp, 0});
  }
  for (std::size_t g = 0; g < groups_.size(); ++g)
    for (int i = 0; i < groups_[g].spawn.count; ++i) spawn_group_member(g);
}

std::uint32_t Zone::ticks(float seconds) const {
  if (!(seconds > 0.0f)) return 0;
  return static_cast<std::uint32_t>(std::min(4.0e9, std::ceil(static_cast<double>(seconds) / static_cast<double>(dt_))));
}

EntityId Zone::new_id() {
  EntityId id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  while (players_.contains(id) || mobs_.contains(id) || corpses_.contains(id)) {
    id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
  }
  return id;
}

EntityId Zone::add_player(const ZonePlayerInit& init) {
  const EntityId id = new_id();
  PlayerEntity p;
  p.id = id;
  p.session = init.session;
  p.char_id = init.char_id;
  p.name = init.name;
  p.race_def = content_.find_race(init.race);
  p.class_def = content_.find_class(init.class_id);
  if (!p.race_def) p.race_def = &content_.races.front();
  if (!p.class_def) p.class_def = &content_.classes.front();
  p.race = p.race_def->id;
  p.class_id = p.class_def->id;
  p.xp = std::min(init.xp, total_xp_for_level(rules_.max_level, rules_.xp));
  // The stored level is advisory; XP decides (it can only differ after a rules change).
  p.level = level_for_xp(p.xp, rules_.max_level, rules_.xp);
  if (init.xp == 0 && init.level > 1) {
    p.level = std::clamp(init.level, 1, rules_.max_level);
    p.xp = total_xp_for_level(p.level, rules_.xp);
  }
  p.stats = derive_stats(*p.race_def, *p.class_def, p.level);
  p.hp = init.hp < 0 ? p.stats.max_hp : std::clamp(init.hp, 1, p.stats.max_hp);
  p.mana = init.mana < 0 ? p.stats.max_mana : std::clamp(init.mana, 0, p.stats.max_mana);
  p.endurance = init.endurance < 0 ? p.stats.max_endurance : std::clamp(init.endurance, 0, p.stats.max_endurance);
  p.coin = init.coin;
  for (const auto& [slot, stack] : init.items) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= kInventorySlots || stack.empty() || !content_.find_item(stack.item_id)) continue;
    p.inventory[static_cast<std::size_t>(slot)] = stack;
  }
  for (const auto& s : init.skills)
    if (p.class_def->skill(s.id)) p.skills.push_back(s);
  unlock_skills(p);
  p.bind_zone = init.bind_zone.empty() ? def_.id : init.bind_zone;
  p.bind_pos = init.bind_pos;
  p.bind_yaw = init.bind_yaw;
  const Bounds b = bounds();
  p.pos = {std::clamp(init.pos.x, b.min_x, b.max_x), def_.ground_y, std::clamp(init.pos.z, b.min_z, b.max_z)};
  p.yaw = wrap_angle(init.yaw);
  p.budget = InputBudget(rules_.movement.input_budget_max);
  p.changed_tick = tick_;
  grid_.insert(id, p.pos.x, p.pos.z);
  players_.emplace(id, std::move(p));
  return id;
}

void Zone::remove_player(EntityId id, ZoneSink& sink) {
  auto it = players_.find(id);
  if (it == players_.end()) return;
  PlayerEntity& p = it->second;
  if (p.looting != 0) close_loot(p, sink, "");
  if (p.dead) respawn(id, sink);
  drop_from_threat(id);
  players_.erase(it);
  grid_.remove(id);
  for (auto& [other_id, other] : players_) {
    auto k = std::lower_bound(other.known.begin(), other.known.end(), id);
    if (k != other.known.end() && *k == id) {
      other.known.erase(k);
      other.sent_tick.erase(id);
      send(sink, other, proto::EntityDespawn{id});
    }
    if (other.target == id) other.target = 0;
  }
}

void Zone::send_initial_state(EntityId id, ZoneSink& sink) {
  PlayerEntity* p = find(id);
  if (!p) return;
  proto::AbilityList list;
  for (const AbilityDef* a : content_.abilities_for(p->class_id)) {
    proto::AbilityInfo info;
    info.id = a->id;
    info.name = a->name;
    info.type = a->type;
    info.level = static_cast<std::uint8_t>(std::clamp(a->level, 1, 255));
    info.resource = static_cast<std::uint8_t>(a->resource);
    info.cost = static_cast<std::uint16_t>(std::clamp(a->cost, 0, 65535));
    info.cooldown_ms = static_cast<std::uint32_t>(std::lround(a->cooldown * 1000.0f));
    info.range = a->range;
    info.target = static_cast<std::uint8_t>(a->target);
    info.gcd = a->gcd;
    info.description = a->description;
    list.abilities.push_back(std::move(info));
    if (list.abilities.size() >= 64) break;
  }
  send(sink, *p, list);
  send_skills(*p, sink);
  send_inventory(*p, sink);
  send_player_stats(*p, sink, true);
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
  // Dead or stunned players still have their inputs consumed and acknowledged, but cannot move.
  const bool frozen = p.dead || p.stunned_until > tick_;
  for (std::uint32_t n = 0; n < kMaxInputsPerTick && p.queue_size > 0 && p.budget.try_consume(); ++n) {
    PendingInput in = p.queue[p.queue_head];
    p.queue_head = (p.queue_head + 1) % PlayerEntity::kQueueCap;
    --p.queue_size;
    if (frozen) {
      in.intent.move_x = 0.0f;
      in.intent.move_z = 0.0f;
    }
    const MoveIntent clean = sanitize_intent(in.intent);
    if (p.sitting && (clean.move_x != 0.0f || clean.move_z != 0.0f)) stand(p);

    const Vec3 next = step_position(p.pos, in.intent, rules_.movement.run_speed, dt_, b, def_.ground_y);
    if (p.check_cooldown == 0) {
      if (p.has_pred) {
        const std::uint32_t steps = in.seq - p.last_pred_seq;
        if (!is_plausible_displacement(p.last_pred, in.predicted, rules_.movement.run_speed, dt_, steps,
                                       rules_.movement.teleport_tolerance)) {
          sink.zone_violation(p, "teleport");
          correction = "teleport";
        }
      }
      if (!correction && exceeds_desync(next, in.predicted, rules_.movement.desync_tolerance)) correction = frozen ? "frozen" : "desync";
    }
    if (next.x != p.pos.x || next.z != p.pos.z || clean.yaw != p.yaw) p.changed_tick = tick_;
    p.pos = next;
    if (!p.dead) p.yaw = clean.yaw;
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

bool Zone::entity_position(EntityId id, Vec3& out) const {
  if (auto it = players_.find(id); it != players_.end()) {
    out = it->second.pos;
    return true;
  }
  if (auto it = mobs_.find(id); it != mobs_.end() && it->second.alive()) {
    out = it->second.pos;
    return true;
  }
  if (auto it = corpses_.find(id); it != corpses_.end()) {
    out = it->second.pos;
    return true;
  }
  return false;
}

bool Zone::spawn_message(EntityId id, proto::EntitySpawn& s) const {
  if (auto it = players_.find(id); it != players_.end()) {
    const PlayerEntity& o = it->second;
    std::uint8_t flags = 0;
    if (o.dead) flags |= kFlagDead;
    if (o.sitting) flags |= kFlagSitting;
    s = proto::EntitySpawn{o.id, static_cast<std::uint8_t>(EntityKind::Player), o.name, o.race, o.class_id,
                           static_cast<std::uint8_t>(o.level), o.pos.x, o.pos.y, o.pos.z, o.yaw,
                           pct(o.hp, o.stats.max_hp), flags, 1.0f};
    return true;
  }
  if (auto it = mobs_.find(id); it != mobs_.end() && it->second.alive()) {
    const MobEntity& m = it->second;
    s = proto::EntitySpawn{m.id, static_cast<std::uint8_t>(EntityKind::Mob), m.def->name, m.def->model, "",
                           static_cast<std::uint8_t>(m.def->level), m.pos.x, m.pos.y, m.pos.z, m.yaw,
                           pct(m.hp, m.stats.max_hp), 0, m.def->scale};
    return true;
  }
  if (auto it = corpses_.find(id); it != corpses_.end()) {
    const CorpseEntity& c = it->second;
    s = proto::EntitySpawn{c.id, static_cast<std::uint8_t>(EntityKind::Corpse), c.name, c.model, "",
                           static_cast<std::uint8_t>(std::clamp(c.level, 1, 255)), c.pos.x, c.pos.y, c.pos.z, c.yaw, 0,
                           kFlagDead, c.scale};
    return true;
  }
  return false;
}

void Zone::update_interest(PlayerEntity& p, ZoneSink& sink) {
  const float r = def_.interest_radius;
  const float r_out = r * kInterestHysteresis;
  scratch_candidates_.clear();
  grid_.query(p.pos.x, p.pos.z, r_out, scratch_candidates_);

  scratch_known_.clear();
  for (EntityId cid : scratch_candidates_) {
    if (cid == p.id) continue;
    Vec3 pos;
    if (!entity_position(cid, pos)) continue;
    const bool already = std::binary_search(p.known.begin(), p.known.end(), cid);
    if (already || horizontal_distance_sq(p.pos, pos) <= r * r) scratch_known_.push_back(cid);
  }
  std::sort(scratch_known_.begin(), scratch_known_.end());

  // Despawn: known but no longer visible (moved away, died, decayed, or left).
  scratch_diff_.clear();
  std::set_difference(p.known.begin(), p.known.end(), scratch_known_.begin(), scratch_known_.end(), std::back_inserter(scratch_diff_));
  for (EntityId gone : scratch_diff_) {
    send(sink, p, proto::EntityDespawn{gone});
    p.sent_tick.erase(gone);
    if (p.target == gone) p.target = 0;
  }

  // Spawn: newly visible.
  scratch_diff_.clear();
  std::set_difference(scratch_known_.begin(), scratch_known_.end(), p.known.begin(), p.known.end(), std::back_inserter(scratch_diff_));
  proto::EntitySpawn spawn;
  for (EntityId added : scratch_diff_)
    if (spawn_message(added, spawn)) {
      send(sink, p, spawn);
      p.sent_tick[added] = tick_;  // the spawn carries the current state
    }

  p.known.swap(scratch_known_);
}

// Watchers of `who` get a fresh EntitySpawn (level, name, or state changed); clients update in place.
void Zone::respawn_spot_watchers(EntityId who, ZoneSink& sink) {
  proto::EntitySpawn spawn;
  if (!spawn_message(who, spawn)) return;
  for (auto& [id, other] : players_)
    if (std::binary_search(other.known.begin(), other.known.end(), who)) send(sink, other, spawn);
}

void Zone::send_snapshots(ZoneSink& sink) {
  auto& snap = scratch_snapshot_;
  for (auto& [id, p] : players_) {
    snap.server_tick = tick_;
    snap.ack_seq = p.last_applied_seq;
    snap.self_x = p.pos.x;
    snap.self_y = p.pos.y;
    snap.self_z = p.pos.z;
    snap.entities.clear();
    // An entity is due when it changed recently (sent twice, the channel is unreliable), changed
    // since this watcher last got it, or has not been sent for a second. If more than 256 are due,
    // the ones waiting longest go first, so nothing can starve.
    auto& due = scratch_due_;
    due.clear();
    for (EntityId kid : p.known) {
      std::uint32_t changed = 0;
      if (auto pit = players_.find(kid); pit != players_.end()) changed = pit->second.changed_tick;
      else if (auto mit = mobs_.find(kid); mit != mobs_.end() && mit->second.alive()) changed = mit->second.changed_tick;
      else continue;  // corpses are static: their spawn message is enough
      const auto st = p.sent_tick.find(kid);
      const std::uint32_t last = st == p.sent_tick.end() ? 0 : st->second;
      if (changed + 1 >= tick_ || changed > last || tick_ - last >= kHeartbeatTicks) due.emplace_back(last, kid);
    }
    constexpr std::size_t kMaxEntities = 256;
    if (due.size() > kMaxEntities) {
      std::nth_element(due.begin(), due.begin() + kMaxEntities, due.end());
      due.resize(kMaxEntities);
    }
    for (const auto& [last, kid] : due) {
      p.sent_tick[kid] = tick_;
      if (auto pit = players_.find(kid); pit != players_.end()) {
        const PlayerEntity& o = pit->second;
        std::uint8_t flags = 0;
        if (o.dead) flags |= kFlagDead;
        if (o.sitting) flags |= kFlagSitting;
        if (o.stunned_until > tick_) flags |= kFlagStunned;
        if (in_combat(o)) flags |= kFlagInCombat;
        snap.entities.push_back({o.id, o.pos.x, o.pos.y, o.pos.z, o.yaw, pct(o.hp, o.stats.max_hp), flags});
      } else {
        const MobEntity& m = mobs_.at(kid);
        std::uint8_t flags = 0;
        if (m.state == AiState::Aggro || m.state == AiState::Chase) flags |= kFlagInCombat;
        if (m.state == AiState::Flee) flags |= kFlagFleeing;
        if (m.state == AiState::Leash) flags |= kFlagEvading;
        if (m.stunned_until > tick_) flags |= kFlagStunned;
        snap.entities.push_back({m.id, m.pos.x, m.pos.y, m.pos.z, m.yaw, pct(m.hp, m.stats.max_hp), flags});
      }
    }
    send(sink, p, snap);
  }
}

void Zone::send_player_stats(PlayerEntity& p, ZoneSink& sink, bool force) {
  proto::PlayerStats s;
  s.level = static_cast<std::uint8_t>(p.level);
  const std::uint64_t base = total_xp_for_level(p.level, rules_.xp);
  const bool capped = p.level >= rules_.max_level;
  s.xp = capped ? 0 : static_cast<std::uint32_t>(std::min<std::uint64_t>(p.xp - std::min(p.xp, base), 0xFFFFFFFFu));
  s.xp_next = capped ? 0 : static_cast<std::uint32_t>(std::min<std::uint64_t>(xp_to_next(p.level, rules_.xp), 0xFFFFFFFFu));
  s.hp = p.hp;
  s.max_hp = p.stats.max_hp;
  s.mana = p.mana;
  s.max_mana = p.stats.max_mana;
  s.endurance = p.endurance;
  s.max_endurance = p.stats.max_endurance;
  s.ac = static_cast<std::uint16_t>(std::clamp(p.stats.ac, 0, 65535));
  std::uint8_t flags = 0;
  if (p.dead) flags |= kFlagDead;
  if (p.sitting) flags |= kFlagSitting;
  if (p.stunned_until > tick_) flags |= kFlagStunned;
  if (in_combat(p)) flags |= kFlagInCombat;
  s.flags = flags;
  s.auto_attack = p.auto_attack;
  s.target_id = p.target;
  std::uint64_t h = 0xCBF29CE484222325ULL;
  for (std::uint64_t v : {std::uint64_t{s.level}, std::uint64_t{s.xp}, std::uint64_t{s.xp_next}, static_cast<std::uint64_t>(s.hp),
                          static_cast<std::uint64_t>(s.max_hp), static_cast<std::uint64_t>(s.mana), static_cast<std::uint64_t>(s.endurance),
                          static_cast<std::uint64_t>(s.max_endurance), std::uint64_t{s.flags}, std::uint64_t{s.auto_attack},
                          std::uint64_t{s.target_id}})
    h = mix(h, v);
  const bool changed = h != p.stats_hash;
  // On change at most every other tick; otherwise a 1 s heartbeat (the channel is unreliable).
  if (!force && !(changed && tick_ - p.stats_sent_tick >= 2) && tick_ - p.stats_sent_tick < kHeartbeatTicks) return;
  p.stats_hash = h;
  p.stats_sent_tick = tick_;
  send(sink, p, s);
}

void Zone::tick(std::uint32_t server_tick, ZoneSink& sink) {
  tick_ = server_tick;
  for (auto& [id, p] : players_) {
    expire_buffs(p, sink);  // before combat: a buff never applies on or after its expiry tick
    apply_inputs(p, sink);
    grid_.update(id, p.pos.x, p.pos.z);
  }
  for (auto& [id, m] : mobs_) {
    if (!m.alive()) continue;
    mob_think(m, sink);
    if (m.alive()) grid_.update(id, m.pos.x, m.pos.z);
  }
  for (auto& [id, p] : players_) player_combat(p, sink);
  process_respawns();
  expire_corpses(sink);
  for (auto& [id, p] : players_) {
    regen(p);
    // Walking away closes the loot window.
    if (p.looting != 0) {
      const CorpseEntity* c = find_corpse(p.looting);
      if (!c || horizontal_distance(p.pos, c->pos) > rules_.combat.loot_range * 1.5f) close_loot(p, sink, "You are too far away to loot that corpse.");
    }
  }
  for (auto& [id, p] : players_) update_interest(p, sink);
  send_snapshots(sink);
  for (auto& [id, p] : players_) send_player_stats(p, sink, false);
}

PlayerEntity* Zone::find(EntityId id) {
  auto it = players_.find(id);
  return it == players_.end() ? nullptr : &it->second;
}

const PlayerEntity* Zone::find(EntityId id) const {
  auto it = players_.find(id);
  return it == players_.end() ? nullptr : &it->second;
}

PlayerEntity* Zone::find_by_char(std::int64_t char_id) {
  if (char_id == 0) return nullptr;
  for (auto& [id, p] : players_)
    if (p.char_id == char_id) return &p;
  return nullptr;
}

MobEntity* Zone::find_mob(EntityId id) {
  auto it = mobs_.find(id);
  return it == mobs_.end() ? nullptr : &it->second;
}

CorpseEntity* Zone::find_corpse(EntityId id) {
  auto it = corpses_.find(id);
  return it == corpses_.end() ? nullptr : &it->second;
}

void Zone::for_each_player(const std::function<void(const PlayerEntity&)>& fn) const {
  for (const auto& [id, p] : players_) fn(p);
}

void Zone::for_each_mob(const std::function<void(const MobEntity&)>& fn) const {
  for (const auto& [id, m] : mobs_) fn(m);
}

void Zone::for_each_corpse(const std::function<void(const CorpseEntity&)>& fn) const {
  for (const auto& [id, c] : corpses_) fn(c);
}

void Zone::players_near(const Vec3& pos, float radius, std::vector<EntityId>& out) const {
  grid_.query(pos.x, pos.z, radius, out);
}

}  // namespace ef::game
