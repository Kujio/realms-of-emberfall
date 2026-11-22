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

float PlayerEntity::buff_total(BuffStat stat) const { return stat_total(buffs, stat); }

bool PlayerEntity::knows_spell(std::string_view spell_id) const {
  return std::find(spellbook.begin(), spellbook.end(), spell_id) != spellbook.end();
}

bool PlayerEntity::stealthed() const { return hidden || feigned || stat_total(buffs, BuffStat::Invisible) > 0.0f; }

bool CorpseEntity::empty() const {
  return coin == 0 && std::all_of(items.begin(), items.end(), [](const CorpseItem& s) { return s.empty(); });
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
    : def_(def), content_(content), rules_(content.rules), dt_(dt), rng_(seed), grid_(def.interest_radius),
      collision_(def.obstacles, def.walls, def.layout ? def.layout->tiles : TileGrid{}) {
  nav_.build(bounds(), def_.nav_cell, collision_, kMobBodyRadius + 0.1f);
  for (const auto& sp : def_.spawns) {
    const MobDef* md = content_.find_mob(sp.mob);
    if (!md) continue;  // content validation rejects this; be defensive anyway
    groups_.push_back({md, sp, 0, sp.rare.empty() ? nullptr : content_.find_mob(sp.rare), false});
  }
  for (std::size_t g = 0; g < groups_.size(); ++g)
    for (int i = 0; i < groups_[g].spawn.count; ++i) spawn_group_member(g);
  for (const auto& n : content_.npcs) {
    if (n.zone != def_.id) continue;
    NpcEntity e;
    e.id = new_id();
    e.def = &n;
    const Bounds b = bounds();
    e.pos = {std::clamp(n.x, b.min_x, b.max_x), 0.0f, std::clamp(n.z, b.min_z, b.max_z)};
    e.pos.y = def_.ground_at(e.pos.x, e.pos.z);
    e.yaw = wrap_angle(n.yaw);
    grid_.insert(e.id, e.pos.x, e.pos.z);
    npcs_.emplace(e.id, e);
  }
}

std::uint32_t Zone::ticks(float seconds) const {
  if (!(seconds > 0.0f)) return 0;
  // The epsilon absorbs float error (1.95 s at 0.05 s/tick is 39 ticks, not 40).
  return static_cast<std::uint32_t>(std::min(4.0e9, std::ceil(static_cast<double>(seconds) / static_cast<double>(dt_) - 1e-4)));
}

EntityId Zone::new_id() {
  EntityId id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  while (players_.contains(id) || mobs_.contains(id) || corpses_.contains(id) || npcs_.contains(id)) {
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
  p.gender = static_cast<std::uint8_t>(std::clamp(init.gender, 0, 1));
  p.xp = std::min(init.xp, total_xp_for_level(rules_.max_level, rules_.xp));
  // The stored level is advisory; XP decides (it can only differ after a rules change).
  p.level = level_for_xp(p.xp, rules_.max_level, rules_.xp);
  if (init.xp == 0 && init.level > 1) {
    p.level = std::clamp(init.level, 1, rules_.max_level);
    p.xp = total_xp_for_level(p.level, rules_.xp);
  }
  p.base_stats = derive_stats(*p.race_def, *p.class_def, p.level);
  p.stats = p.base_stats;
  p.hp = init.hp < 0 ? p.stats.max_hp : std::clamp(init.hp, 1, p.stats.max_hp);
  p.mana = init.mana < 0 ? p.stats.max_mana : std::clamp(init.mana, 0, p.stats.max_mana);
  p.endurance = init.endurance < 0 ? p.stats.max_endurance : std::clamp(init.endurance, 0, p.stats.max_endurance);
  p.coin = init.coin;
  p.bank_coin = init.bank_coin;
  // Items load into their saved slots; a slot that no longer exists (a bag that shrank, a removed item)
  // drops into the pack, or the bank, so nothing is silently lost.
  std::vector<ItemStackState> homeless;
  for (int pass = 0; pass < 2; ++pass)  // bags first, so their contents have somewhere to go
    for (const auto& [slot, stack] : init.items) {
      if (slot < 0 || slot > 0xFFFF || stack.empty()) continue;
      const ItemDef* idef = content_.find_item(stack.item_id);
      if (!idef) continue;
      const auto code = static_cast<SlotCode>(slot);
      if ((pass == 0) == is_bag_content(code)) continue;
      ItemStackState* dst = slot_valid(p.inv, code, content_, rules_.economy.bank_slots) ? p.inv.at(code) : nullptr;
      if (!dst || !dst->empty()) {
        homeless.push_back(stack);
        continue;
      }
      *dst = stack;
      dst->expires_tick = 0;
      // A summoned item keeps the time it had left (none recorded = it never fades).
      for (const auto& [s2, secs] : init.item_seconds)
        if (s2 == slot && secs > 0.0f) dst->expires_tick = tick_ + std::max<std::uint32_t>(1, ticks(std::min(secs, 86400.0f)));
    }
  for (auto& h : homeless) {
    h.expires_tick = 0;
    if (game::give_item(p.inv, content_, h)) continue;
    for (std::size_t b2 = 0; b2 < static_cast<std::size_t>(std::clamp(rules_.economy.bank_slots, 1, 16)) && !h.empty(); ++b2)
      if (p.inv.bank[b2].empty()) std::swap(p.inv.bank[b2], h);
    if (!h.empty()) ++p.unplaced_items;  // no room anywhere: the game server refuses entry rather than lose it
  }
  for (const auto& s : init.skills)
    if (p.class_def->skill(s.id)) p.skills.push_back(s);
  unlock_skills(p);
  p.bind_zone = init.bind_zone.empty() ? def_.id : init.bind_zone;
  p.bind_pos = init.bind_pos;
  p.bind_yaw = init.bind_yaw;
  restore_saved(p, init);  // spellbook, gems, buffs, recast timers (and stats with buffs)
  // Vitals again, against the buffed maximums (a saved HP buff keeps its extra health).
  p.hp = init.hp < 0 ? p.stats.max_hp : std::clamp(init.hp, 1, p.stats.max_hp);
  p.mana = init.mana < 0 ? p.stats.max_mana : std::clamp(init.mana, 0, p.stats.max_mana);
  const Bounds b = bounds();
  p.pos = settle({std::clamp(init.pos.x, b.min_x, b.max_x), 0.0f, std::clamp(init.pos.z, b.min_z, b.max_z)}, kPlayerRadius);
  p.yaw = wrap_angle(init.yaw);
  p.arrived_tick = tick_;
  p.budget = InputBudget(rules_.movement.input_budget_max);
  p.changed_tick = tick_;
  p.held_sig = held_item(p, EquipSlot::Primary) + "|" + held_item(p, EquipSlot::Secondary) + "|" + held_item(p, EquipSlot::Ranged);
  grid_.insert(id, p.pos.x, p.pos.z);
  players_.emplace(id, std::move(p));
  return id;
}

void Zone::remove_player(EntityId id, ZoneSink& sink) {
  auto it = players_.find(id);
  if (it == players_.end()) return;
  PlayerEntity& p = it->second;
  if (p.looting != 0) close_loot(p, sink, "");
  trade_cancel(id, p.name + " is no longer here; the trade is cancelled.", sink);
  if (p.dead) respawn(id, sink);
  if (p.pet != 0)
    if (MobEntity* pet = find_mob(p.pet)) release_pet(*pet, false, nullptr, sink);
  p.pet = 0;
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
  send_character_sheet(*p, sink, true);
  send_spellbook(*p, sink);
  send_gems(*p, sink);
  send_buff_list(*p, p->id, sink);
  send_player_stats(*p, sink, true);
  send_pet_status(*p, sink, true);
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
  // Dead, stunned, rooted, mesmerized, or feared players still have their inputs consumed and
  // acknowledged, but cannot move. (Fear on players holds them in place until fear movement exists.)
  const bool frozen = p.dead || p.stunned_until > tick_ || (control_bits(p.buffs) & (kControlRoot | kControlMez | kControlFear)) != 0;
  const float speed_mult = move_speed_multiplier(p.buffs) * (p.sneaking ? rules_.stealth.sneak_speed_mult : 1.0f);
  const float speed = rules_.movement.run_speed * speed_mult;
  // Plausibility uses the faster of the old and new speeds: the client learns a new speed a moment late.
  const float check_speed = rules_.movement.run_speed * std::max(1.0f, speed_mult);
  for (std::uint32_t n = 0; n < kMaxInputsPerTick && p.queue_size > 0 && p.budget.try_consume(); ++n) {
    PendingInput in = p.queue[p.queue_head];
    p.queue_head = (p.queue_head + 1) % PlayerEntity::kQueueCap;
    --p.queue_size;
    if (frozen) {
      in.intent.move_x = 0.0f;
      in.intent.move_z = 0.0f;
    }
    const MoveIntent clean = sanitize_intent(in.intent);
    const bool moving = clean.move_x != 0.0f || clean.move_z != 0.0f;
    if (p.sitting && moving) stand(p);
    if (moving && p.feigned) stand_from_feign(p, sink);
    if (moving && p.hidden && !p.sneaking) {
      p.hidden = false;
      p.changed_tick = tick_;
      combat_text(p, "You are no longer hidden.", sink);
    }

    const Vec3 next = step_position(p.pos, in.intent, speed, dt_, b, collision_, def_.terrain, kPlayerRadius);
    if (p.check_cooldown == 0) {
      if (p.has_pred) {
        const std::uint32_t steps = in.seq - p.last_pred_seq;
        if (!is_plausible_displacement(p.last_pred, in.predicted, check_speed, dt_, steps,
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
  check_zone_lines(p);
}

Vec3 Zone::settle(Vec3 pos, float radius) const {
  const Bounds b = bounds();
  // Push out, clamp, and repeat while the clamp put the body back into something (a wall at the edge).
  for (int round = 0; round < 3; ++round) {
    pos = collision_.resolve(pos, radius);
    const float cx = std::clamp(pos.x, b.min_x, b.max_x);
    const float cz = std::clamp(pos.z, b.min_z, b.max_z);
    const bool clamped = cx != pos.x || cz != pos.z;
    pos.x = cx;
    pos.z = cz;
    if (!clamped || !collision_.overlaps(pos.x, pos.z, radius)) break;
  }
  if (collision_.overlaps(pos.x, pos.z, radius)) {
    // Still stuck (a wall against the zone's edge): the nearest free spot on rings around it.
    const Vec3 at = pos;
    bool found = false;
    for (int ring = 1; ring <= 60 && !found; ++ring)
      for (int k = 0; k < 16 && !found; ++k) {
        const float a = static_cast<float>(k) * kPi / 8.0f;
        const float r = 0.5f * static_cast<float>(ring);
        const float x = std::clamp(at.x + std::cos(a) * r, b.min_x, b.max_x);
        const float z = std::clamp(at.z + std::sin(a) * r, b.min_z, b.max_z);
        if (!collision_.overlaps(x, z, radius)) {
          pos.x = x;
          pos.z = z;
          found = true;
        }
      }
  }
  pos.y = def_.ground_at(pos.x, pos.z);
  return pos;
}

const ZoneLineDef* Zone::zone_line_at(float x, float z) const {
  for (const auto& l : def_.zone_lines)
    if (l.contains(x, z)) return &l;
  return nullptr;
}

void Zone::check_zone_lines(PlayerEntity& p) {
  if (p.dead || p.transferring || def_.zone_lines.empty()) return;
  if (tick_ - p.arrived_tick < ticks(1.0f)) return;  // just arrived: never bounce straight back
  const ZoneLineDef* l = zone_line_at(p.pos.x, p.pos.z);
  if (!l) return;
  const ZoneDef* to = content_.find_zone(l->to);
  const ZoneEntryDef* e = to ? to->find_entry(l->entry) : nullptr;
  if (!e) return;  // content validation guarantees both
  ZoneTransfer t;
  t.player = p.id;
  t.char_id = p.char_id;
  t.to = {to->id, {e->at.x, to->ground_at(e->at.x, e->at.z), e->at.z}, e->at.yaw};
  t.reason = "zone line";
  p.transferring = true;
  transfers_.push_back(std::move(t));
}

std::vector<ZoneTransfer> Zone::take_transfers() {
  std::vector<ZoneTransfer> out;
  out.swap(transfers_);
  return out;
}

bool Zone::has_player_corpses() const {
  return std::any_of(corpses_.begin(), corpses_.end(), [](const auto& kv) { return kv.second.player_corpse; });
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
  if (auto it = npcs_.find(id); it != npcs_.end()) {
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
    // An illusion of a race draws the player as that race; of a model, as that model.
    std::string race = o.race;
    std::string model;
    if (const std::string* ill = illusion_of(o)) {
      if (content_.find_race(*ill)) race = *ill;
      else model = *ill;
    }
    s = proto::EntitySpawn{o.id, static_cast<std::uint8_t>(EntityKind::Player), o.name, race, o.class_id,
                           static_cast<std::uint8_t>(o.level), o.pos.x, o.pos.y, o.pos.z, o.yaw,
                           pct(o.hp, o.stats.max_hp), flags, 1.0f, o.gender, player_flags2(o), model, 0,
                           held_item(o, EquipSlot::Primary), held_item(o, EquipSlot::Secondary), held_item(o, EquipSlot::Ranged)};
    return true;
  }
  if (auto it = mobs_.find(id); it != mobs_.end() && it->second.alive()) {
    const MobEntity& m = it->second;
    s = proto::EntitySpawn{m.id, static_cast<std::uint8_t>(EntityKind::Mob), m.def->name, m.def->model, "",
                           static_cast<std::uint8_t>(m.def->level), m.pos.x, m.pos.y, m.pos.z, m.yaw,
                           pct(m.hp, m.stats.max_hp), 0, m.def->scale, 0, mob_flags2(m), "", m.owner, "", "", ""};
    return true;
  }
  if (auto it = corpses_.find(id); it != corpses_.end()) {
    const CorpseEntity& c = it->second;
    s = proto::EntitySpawn{c.id, static_cast<std::uint8_t>(EntityKind::Corpse), c.name, c.model, c.class_id,
                           static_cast<std::uint8_t>(std::clamp(c.level, 1, 255)), c.pos.x, c.pos.y, c.pos.z, c.yaw, 0,
                           kFlagDead, c.scale, c.gender, 0, "", 0, "", "", ""};
    return true;
  }
  if (auto it = npcs_.find(id); it != npcs_.end()) {
    const NpcEntity& n = it->second;
    // race carries the model, class_id the role (the client shows "trainer").
    s = proto::EntitySpawn{n.id, static_cast<std::uint8_t>(EntityKind::Npc), n.def->name, n.def->model, n.def->role,
                           static_cast<std::uint8_t>(std::clamp(n.def->level, 1, 255)), n.pos.x, n.pos.y, n.pos.z, n.yaw, 100,
                           0, 1.0f, 0, 0, "", 0, "", "", ""};
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
    if (id == who || std::binary_search(other.known.begin(), other.known.end(), who)) send(sink, other, spawn);  // the player too: its own look
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
        const std::uint8_t ctl = control_bits(o.buffs);
        if ((ctl & kControlMez) != 0) flags |= kFlagMezzed;
        if ((ctl & kControlRoot) != 0) flags |= kFlagRooted;
        if ((ctl & kControlFear) != 0) flags |= kFlagFleeing;
        snap.entities.push_back({o.id, o.pos.x, o.pos.y, o.pos.z, o.yaw, pct(o.hp, o.stats.max_hp), flags, player_flags2(o)});
      } else {
        const MobEntity& m = mobs_.at(kid);
        std::uint8_t flags = 0;
        if (m.state == AiState::Aggro || m.state == AiState::Chase) flags |= kFlagInCombat;
        if (m.state == AiState::Leash) flags |= kFlagEvading;
        if (m.stunned_until > tick_) flags |= kFlagStunned;
        const std::uint8_t ctl = control_bits(m.buffs);
        if (m.state == AiState::Flee || (ctl & kControlFear) != 0) flags |= kFlagFleeing;
        if ((ctl & kControlMez) != 0) flags |= kFlagMezzed;
        if ((ctl & kControlRoot) != 0) flags |= kFlagRooted;
        snap.entities.push_back({m.id, m.pos.x, m.pos.y, m.pos.z, m.yaw, pct(m.hp, m.stats.max_hp), flags, mob_flags2(m)});
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
  const std::uint8_t ctl = control_bits(p.buffs);
  if ((ctl & kControlMez) != 0) flags |= kFlagMezzed;
  if ((ctl & kControlRoot) != 0) flags |= kFlagRooted;
  if ((ctl & kControlFear) != 0) flags |= kFlagFleeing;
  s.flags = flags;
  s.auto_attack = p.auto_attack;
  s.target_id = p.target;
  s.run_speed = rules_.movement.run_speed * move_speed_multiplier(p.buffs) * (p.sneaking ? rules_.stealth.sneak_speed_mult : 1.0f);
  s.flags2 = player_flags2(p);
  std::uint64_t h = 0xCBF29CE484222325ULL;
  for (std::uint64_t v : {std::uint64_t{s.level}, std::uint64_t{s.xp}, std::uint64_t{s.xp_next}, static_cast<std::uint64_t>(s.hp),
                          static_cast<std::uint64_t>(s.max_hp), static_cast<std::uint64_t>(s.mana), static_cast<std::uint64_t>(s.endurance),
                          static_cast<std::uint64_t>(s.max_endurance), std::uint64_t{s.flags}, std::uint64_t{s.auto_attack},
                          std::uint64_t{s.target_id}, static_cast<std::uint64_t>(s.max_mana), std::uint64_t{s.ac}, std::uint64_t{s.flags2},
                          static_cast<std::uint64_t>(std::lround(s.run_speed * 100.0f))})
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
  path_budget_ = 24;
  for (auto& [id, p] : players_) {
    if (p.transferring) continue;
    tick_player_effects(p, sink);  // before combat: an effect never applies on or after its expiry tick
    apply_inputs(p, sink);
    grid_.update(id, p.pos.x, p.pos.z);
    cast_tick(p, sink);
    song_tick(p, sink);
    if (p.rez.corpse != 0 && tick_ > p.rez.expires_tick) p.rez = {};
  }
  for (auto& [id, m] : mobs_) {
    if (!m.alive()) continue;
    tick_mob_effects(m, sink);
    if (!m.alive()) continue;
    if (m.is_pet()) pet_think(m, sink);
    else mob_think(m, sink);
    if (m.alive()) grid_.update(id, m.pos.x, m.pos.z);
  }
  for (auto& [id, p] : players_) player_combat(p, sink);
  process_respawns();
  expire_corpses(sink);
  tick_rolls(sink);
  if (tick_ % 10 == 0) {
    tick_trades(sink);
    for (auto& [id, p] : players_) check_windows(p, sink);
  }
  if (tick_ % 20 == 0) restock_vendors();
  tick_travel(sink);
  const std::uint32_t meditate_every = std::max<std::uint32_t>(1, ticks(3.0f));
  for (auto& [id, p] : players_) {
    regen(p);
    if (p.sitting && p.stats.max_mana > 0 && p.mana < p.stats.max_mana && tick_ % meditate_every == 0) try_skill_up(p, "meditate", sink);
    if (tick_ % 20 == 0) expire_items(p, sink);
    // Walking away closes the loot window.
    if (p.looting != 0) {
      const CorpseEntity* c = find_corpse(p.looting);
      if (!c || horizontal_distance(p.pos, c->pos) > rules_.combat.loot_range * 1.5f) close_loot(p, sink, "You are too far away to loot that corpse.");
    }
  }
  for (auto& [id, p] : players_) update_interest(p, sink);
  flush_buff_lists(sink);
  send_snapshots(sink);
  for (auto& [id, p] : players_) {
    send_player_stats(p, sink, false);
    send_pet_status(p, sink, false);
    if (p.inv_dirty) send_inventory(p, sink);
    send_character_sheet(p, sink, false);
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

NpcEntity* Zone::find_npc(EntityId id) {
  auto it = npcs_.find(id);
  return it == npcs_.end() ? nullptr : &it->second;
}

void Zone::for_each_npc(const std::function<void(const NpcEntity&)>& fn) const {
  for (const auto& [id, n] : npcs_) fn(n);
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
