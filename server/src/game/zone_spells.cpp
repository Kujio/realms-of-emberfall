// Zone spells: casting (gems, cast times, interrupts), the effects of spells and abilities,
// buffs and debuffs (stacking, ticks, expiry, crowd control), the spellbook, trainers, and
// resurrection. The math lives in spells.cpp and rules.cpp; this file applies it to entities.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

std::uint32_t seconds_ms(float s) { return static_cast<std::uint32_t>(std::lround(std::max(0.0f, s) * 1000.0f)); }

bool has_effect(const AbilityDef& a, EffectType t) { return a.has(t); }

// The event that announces a newly placed lasting effect.
CombatKind lasting_kind(const ActiveBuff& b) {
  if (b.beneficial) return CombatKind::Buff;
  const std::uint8_t ctl = b.control();
  if ((ctl & kControlCharm) != 0) return CombatKind::Charm;
  if ((ctl & kControlMez) != 0) return CombatKind::Mez;
  if ((ctl & kControlFear) != 0) return CombatKind::Fear;
  if ((ctl & kControlRoot) != 0) return CombatKind::Root;
  for (const auto& m : b.mods)
    if (m.stat == BuffStat::AttackSpeedPct && m.value < 0.0f) return CombatKind::Slow;
  for (const auto& m : b.mods)
    if (m.stat == BuffStat::MoveSpeedPct && m.value < 0.0f) return CombatKind::Snare;
  return CombatKind::Debuff;
}

std::string body_phrase(const std::string& body) { return body == "undead" ? "the undead" : body + " creatures"; }

}  // namespace

// ---------------------------------------------------------------- lookups

const AbilityDef* Zone::any_ability(std::string_view id) const {
  if (const AbilityDef* a = content_.find_spell(id)) return a;
  if (const AbilityDef* a = content_.find_item_effect(id)) return a;
  return content_.find_ability(id);
}

int Zone::mob_resist(const MobEntity& m, ResistType type) const {
  return std::max(0, base_mob_resist(*m.def, type, rules_.mob_scaling) + resist_bonus(m.buffs, type));
}

int Zone::mob_ac(const MobEntity& m) const {
  return std::max(0, m.stats.ac + static_cast<int>(std::lround(stat_total(m.buffs, BuffStat::Ac))));
}

void Zone::refresh_stats(PlayerEntity& p) {
  // Race, class and level; then worn gear; then buffs.
  const DerivedStats b = apply_gear(p.base_stats, gear_totals(p.inv, content_));
  DerivedStats d = b;
  const HandWeapon main = main_hand(p.inv, *p.class_def, p.level, content_);
  const HandWeapon off = off_hand(p.inv, *p.class_def, p.level, content_);
  d.weapon_damage = main.damage;
  d.weapon_delay = main.delay;
  d.offhand_damage = off.damage;
  d.offhand_delay = off.delay;
  auto add = [&](int base, BuffStat st) { return base + static_cast<int>(std::lround(stat_total(p.buffs, st))); };
  d.ac = std::max(0, add(b.ac, BuffStat::Ac));
  d.max_hp = std::max(1, add(b.max_hp, BuffStat::MaxHp));
  d.max_mana = b.max_mana > 0 ? std::max(0, add(b.max_mana, BuffStat::MaxMana)) : 0;
  d.stats.str = add(b.stats.str, BuffStat::Str);
  d.stats.sta = add(b.stats.sta, BuffStat::Sta);
  d.stats.agi = add(b.stats.agi, BuffStat::Agi);
  d.stats.dex = add(b.stats.dex, BuffStat::Dex);
  d.stats.wis = add(b.stats.wis, BuffStat::Wis);
  d.stats.intel = add(b.stats.intel, BuffStat::Int);
  d.stats.cha = add(b.stats.cha, BuffStat::Cha);
  p.stats = d;
  p.hp = std::min(p.hp, p.stats.max_hp);
  p.mana = std::min(p.mana, p.stats.max_mana);
  p.endurance = std::min(p.endurance, p.stats.max_endurance);
  p.changed_tick = tick_;
}

void Zone::relocate(PlayerEntity& p, const Vec3& pos, float yaw, ZoneSink& sink, const char* reason) {
  const Bounds b = bounds();
  p.pos = settle({std::clamp(pos.x, b.min_x, b.max_x), 0.0f, std::clamp(pos.z, b.min_z, b.max_z)}, kPlayerRadius);
  p.yaw = wrap_angle(yaw);
  // Inputs predicted from the old spot are stale: drop them and hard-correct the client.
  p.queue_size = 0;
  p.has_pred = false;
  p.check_cooldown = kCorrectionCooldownTicks;
  p.changed_tick = tick_;
  p.stats_hash = 0;
  grid_.update(p.id, p.pos.x, p.pos.z);
  if (p.cast.casting()) interrupt_cast(p, CastResult::Interrupted, "Your spell is interrupted.", sink);
  sink.zone_correction(p, reason);
}

// ---------------------------------------------------------------- checks shared by abilities and spells

bool Zone::can_act(PlayerEntity& p, const AbilityDef& a, ZoneSink& sink) {
  const std::uint8_t ctl = control_bits(p.buffs);
  const char* what = a.spell ? "cast " : "use ";
  if (p.dead) return ability_fail(p, a, "You are dead.", sink), false;
  if (p.stunned_until > tick_) return ability_fail(p, a, "You are stunned!", sink), false;
  if ((ctl & kControlMez) != 0) return ability_fail(p, a, "You are mesmerized!", sink), false;
  if ((ctl & kControlFear) != 0) return ability_fail(p, a, "You are too frightened to act!", sink), false;
  if (p.cast.casting()) return ability_fail(p, a, "You are already casting a spell.", sink), false;
  if (p.level < a.level)
    return ability_fail(p, a, "You must be level " + std::to_string(a.level) + " to " + what + a.name + ".", sink), false;
  if (tick_ < cooldown_ready(p, a.id)) return ability_fail(p, a, a.name + " is not ready yet.", sink), false;
  if (a.gcd && tick_ < p.gcd_until) return ability_fail(p, a, "You must wait a moment.", sink), false;
  if (a.resource == Resource::Endurance && p.endurance < a.cost) return ability_fail(p, a, "You are too fatigued to use " + a.name + ".", sink), false;
  if (a.resource == Resource::Mana && p.mana < a.cost) return ability_fail(p, a, "Insufficient mana to " + std::string(what) + a.name + ".", sink), false;
  return true;
}

bool Zone::resolve_destination(const PlayerEntity& p, const std::string& destination, Destination& out) const {
  auto place = [&](const std::string& zone_id, float x, float z, float yaw) {
    const ZoneDef* zd = zone_id == def_.id ? &def_ : content_.find_zone(zone_id);
    if (!zd) return false;
    out = {zd->id, {x, zd->ground_at(x, z), z}, yaw};
    return true;
  };
  if (destination == "safe_spot") return place(def_.id, def_.safe_spawn.x, def_.safe_spawn.z, def_.safe_spawn.yaw);
  if (destination == "bind") {
    if (p.bind_zone.empty() || (p.bind_zone != def_.id && !content_.find_zone(p.bind_zone)))
      return place(def_.id, def_.safe_spawn.x, def_.safe_spawn.z, def_.safe_spawn.yaw);
    return place(p.bind_zone, p.bind_pos.x, p.bind_pos.z, p.bind_yaw);
  }
  if (const CityDef* c = content_.find_city(destination)) return place(c->zone, c->spawn.x, c->spawn.z, c->spawn.yaw);
  if (const TravelPointDef* t = content_.find_travel_point(destination)) return place(t->zone, t->at.x, t->at.z, t->at.yaw);
  return false;
}

void Zone::move_player_to(PlayerEntity& p, const Destination& to, ZoneSink& sink, const char* reason) {
  if (to.zone == def_.id) {
    relocate(p, to.pos, to.yaw, sink, reason);
    return;
  }
  if (p.transferring) return;
  if (p.cast.casting()) interrupt_cast(p, CastResult::Interrupted, "Your spell is interrupted.", sink);
  if (p.looting != 0) close_loot(p, sink, "");
  drop_from_threat(p.id);
  p.transferring = true;
  transfers_.push_back({p.id, p.char_id, to, reason});
}

bool Zone::resolve_target(PlayerEntity& p, const AbilityDef& a, EntityId requested, EntityId& out, std::string& why, bool landing) {
  const float slack = landing ? rules_.spells.range_slack : 1.0f;
  const EntityId tid = requested != 0 ? requested : p.target;
  auto visible = [&](const Vec3& to) {
    if (collision_.segment_clear(p.pos, to)) return true;
    why = "You cannot see your target.";
    return false;
  };
  for (const auto& e : a.effects) {
    Destination dest;
    if (e.type == EffectType::Teleport && !resolve_destination(p, e.destination, dest)) {
      why = "That place cannot be reached.";
      return false;
    }
    if ((e.type == EffectType::SummonPet || e.type == EffectType::Charm) && p.pet != 0 && find_mob(p.pet))
      return why = "You already have a pet.", false;
  }
  out = 0;
  if (a.has(EffectType::PickLock)) {
    // Pick lock works on a lockbox (an NPC), not on a creature.
    const NpcEntity* n = find_npc(tid);
    if (!n || n->def->role != "lockbox") return why = "There is nothing locked there.", false;
    if (horizontal_distance(p.pos, n->pos) > std::max(a.range, rules_.combat.loot_range) * slack) return why = "You are too far away.", false;
    out = n->id;
    return true;
  }
  switch (a.target) {
    case TargetType::Self:
    case TargetType::Group:  // the caster, its pet, and (from Phase 7) nearby group members
      out = p.id;
      return true;
    case TargetType::Pet: {
      const MobEntity* pet = p.pet != 0 ? find_mob(p.pet) : nullptr;
      if (!pet || !pet->alive()) return why = "You do not have a pet.", false;
      if (horizontal_distance(p.pos, pet->pos) > a.range * slack + 0.5f) return why = "Your pet is out of range.", false;
      if (!visible(pet->pos)) return false;
      out = pet->id;
      return true;
    }
    case TargetType::Friendly: {
      // A friendly spell may land on a pet (anyone's) that the caster targets.
      if (const MobEntity* pet = find_mob(tid); pet && pet->alive() && pet->is_pet()) {
        if (horizontal_distance(p.pos, pet->pos) > a.range * slack + 0.5f) return why = "Your target is out of range.", false;
        if (!visible(pet->pos)) return false;
        out = pet->id;
        return true;
      }
      if (landing && requested != 0 && requested != p.id && !find(tid) && find_mob(tid)) return why = "Your target is gone.", false;
      const PlayerEntity* t = find(tid);
      // At landing, a recorded recipient who has left fails the cast: it is never redirected
      // (and paid for) onto the caster.
      if (landing && requested != 0 && requested != p.id && !t) return why = "Your target is gone.", false;
      if (!t || t->id == p.id) {
        out = p.id;  // a beneficial spell with no player targeted lands on the caster
        return true;
      }
      if (t->dead) return why = "Your target is dead.", false;
      if (horizontal_distance(p.pos, t->pos) > a.range * slack) return why = "Your target is out of range.", false;
      if (!visible(t->pos)) return false;
      out = t->id;
      return true;
    }
    case TargetType::Corpse: {
      const CorpseEntity* c = find_corpse(tid);
      if (!c || !c->player_corpse) return why = "You must target a player's corpse to cast " + a.name + ".", false;
      if (horizontal_distance(p.pos, c->pos) > a.range * slack) return why = "Your target is out of range.", false;
      if (c->rezzed) return why = "That corpse has already been resurrected.", false;
      const PlayerEntity* owner = find_by_char(c->owner_char_id);
      if (!owner) return why = c->owner_name + "'s spirit is not here to answer the call.", false;
      if (owner->id == p.id) return why = "You cannot resurrect yourself.", false;
      if (!visible(c->pos)) return false;
      out = c->id;
      return true;
    }
    case TargetType::Area:
      if (a.range <= 0.0f) return true;  // centred on the caster
      [[fallthrough]];
    case TargetType::Enemy: {
      const MobEntity* m = find_mob(tid);
      if (m && m->is_pet()) return why = "You can't " + std::string(a.spell ? "cast " : "use ") + a.name + " on a pet.", false;
      if (!m || !m->alive()) {
        if (tid != 0 && (find(tid) || find_corpse(tid) || find_npc(tid))) return why = "You can't " + std::string(a.spell ? "cast " : "use ") + a.name + " on that.", false;
        return why = "You need a target for " + a.name + ".", false;
      }
      if (horizontal_distance(p.pos, m->pos) > (a.range + 0.5f * m->def->scale) * slack) return why = "Your target is out of range.", false;
      if (a.target == TargetType::Enemy && !a.requires_body.empty() && m->def->body != a.requires_body)
        return why = a.name + " only affects " + body_phrase(a.requires_body) + ".", false;
      if (!visible(m->pos)) return false;
      out = m->id;
      return true;
    }
  }
  return false;
}

void Zone::pay_and_lock(PlayerEntity& p, const AbilityDef& a, ZoneSink& sink) {
  if (a.resource == Resource::Endurance) p.endurance = std::max(0, p.endurance - a.cost);
  if (a.resource == Resource::Mana) p.mana = std::max(0, p.mana - a.cost);
  set_cooldown(p, a.id, tick_ + ticks(a.cooldown));
  if (a.gcd) p.gcd_until = tick_ + ticks(rules_.combat.gcd_seconds);
  stand(p);
  p.changed_tick = tick_;
  if (!a.skill.empty()) try_skill_up(p, a.skill, sink);
}

// ---------------------------------------------------------------- casting

void Zone::cast_spell(EntityId player, int gem, EntityId target, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  const int usable = gem_count(p.level, rules_.spells);
  if (gem < 0 || gem >= usable || gem >= static_cast<int>(kMaxGems) || p.gems[static_cast<std::size_t>(gem)].empty()) {
    send(sink, p, proto::AbilityResult{false, "", "You have no spell memorized there.", 0, 0});
    return;
  }
  const auto g = static_cast<std::size_t>(gem);
  const AbilityDef* a = content_.find_spell(p.gems[g]);
  if (!a) {
    p.gems[g].clear();
    send_gems(p, sink);
    return;
  }
  if (tick_ < p.gem_ready[g]) return ability_fail(p, *a, "You are still memorizing " + a->name + ".", sink);
  if (!can_act(p, *a, sink)) return;
  if (a->song && p.song.active() && p.song.ability == a) {
    stop_song(p, "You stop singing " + a->name + ".", sink);  // the same song again toggles it off
    return;
  }
  EntityId tid = 0;
  std::string why;
  if (!resolve_target(p, *a, target, tid, why, false)) return ability_fail(p, *a, why, sink);
  if (p.feigned && !a->has(EffectType::FeignDeath)) stand_from_feign(p, sink);
  break_stealth(p, !a->beneficial(), sink);
  if (a->song) {
    pay_and_lock(p, *a, sink);
    start_song(p, *a, gem, tid, sink);
    send(sink, p, proto::AbilityResult{true, a->id, "", ms_of(ticks(a->cooldown), dt_), a->gcd ? ms_of(ticks(rules_.combat.gcd_seconds), dt_) : 0});
    return;
  }
  if (a->cast_time <= 0.0f) {
    pay_and_lock(p, *a, sink);
    if (!a->beneficial()) mark_combat(p);
    resolve_ability(p, *a, tid, sink);
    send(sink, p, proto::AbilityResult{true, a->id, "", ms_of(ticks(a->cooldown), dt_), a->gcd ? ms_of(ticks(rules_.combat.gcd_seconds), dt_) : 0});
    return;
  }
  const std::uint32_t cast_ticks = ticks(a->cast_time);
  p.cast = begin_cast(*a, tid, gem, tick_, cast_ticks, p.pos);
  stand(p);
  p.changed_tick = tick_;
  broadcast(p.pos, proto::CastStart{p.id, tid, a->id, a->name, ms_of(cast_ticks, dt_), static_cast<std::uint8_t>(a->resist)}, sink);
}

void Zone::stop_cast(EntityId player, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p) return;
  if (p->cast.casting()) interrupt_cast(*p, CastResult::Cancelled, "You stop casting.", sink);
  if (p->song.active()) stop_song(*p, "You stop singing.", sink);
}

void Zone::interrupt_cast(PlayerEntity& p, CastResult result, const std::string& message, ZoneSink& sink) {
  if (!p.cast.casting()) return;
  const std::string id = p.cast.ability ? p.cast.ability->id : "";
  end_cast(p.cast);
  broadcast(p.pos, proto::CastEnd{p.id, id, static_cast<std::uint8_t>(result), message}, sink);
}

void Zone::cast_tick(PlayerEntity& p, ZoneSink& sink) {
  if (!p.cast.casting()) return;
  const std::uint8_t ctl = control_bits(p.buffs);
  if (p.dead || p.stunned_until > tick_ || (ctl & (kControlMez | kControlFear)) != 0) {
    interrupt_cast(p, CastResult::Interrupted, "Your spell is interrupted.", sink);
    return;
  }
  if (moved_while_casting(p.cast, p.pos, rules_.spells.move_interrupt_distance)) {
    interrupt_cast(p, CastResult::Interrupted, "Your spell is interrupted: you moved.", sink);
    return;
  }
  if (cast_complete(p.cast, tick_)) finish_cast(p, sink);
}

void Zone::finish_cast(PlayerEntity& p, ZoneSink& sink) {
  const CastState c = p.cast;
  end_cast(p.cast);
  const AbilityDef& a = *c.ability;
  auto fail = [&](const std::string& message) {
    broadcast(p.pos, proto::CastEnd{p.id, a.id, static_cast<std::uint8_t>(CastResult::Failed), message}, sink);
  };
  if (c.gem >= 0 && (c.gem >= gem_count(p.level, rules_.spells) || p.gems[static_cast<std::size_t>(c.gem)] != a.id))
    return fail("Your spell fades: it is no longer memorized.");
  const ItemDef* item = c.item_slot != kNoSlot ? content_.find_item(c.item_id) : nullptr;
  if (c.item_slot != kNoSlot) {
    const ItemStackState* s = p.inv.at(c.item_slot);
    if (!item || !s || s->empty() || s->item_id != c.item_id) return fail("The item is gone.");
  }
  if (a.resource == Resource::Mana && p.mana < a.cost) return fail("Insufficient mana to cast " + a.name + ".");
  if (a.resource == Resource::Endurance && p.endurance < a.cost) return fail("You are too fatigued to use " + a.name + ".");
  EntityId tid = 0;
  std::string why;
  if (!resolve_target(p, a, c.target, tid, why, true)) return fail(why);
  pay_and_lock(p, a, sink);
  std::uint32_t cd = ticks(a.cooldown);
  if (item) {
    if (item->click.recast > a.cooldown) {
      cd = ticks(item->click.recast);
      set_cooldown(p, a.id, tick_ + cd);
    }
    if (item->kind == "consumable") {
      ItemStackState* s = p.inv.at(c.item_slot);
      if (s && --s->count <= 0) *s = {};
      mark_inventory(p);
    }
  }
  broadcast(p.pos, proto::CastEnd{p.id, a.id, static_cast<std::uint8_t>(CastResult::Completed), ""}, sink);
  if (!a.beneficial()) mark_combat(p);
  resolve_ability(p, a, tid, sink);
  send(sink, p, proto::AbilityResult{true, a.id, "", ms_of(cd, dt_), a.gcd ? ms_of(ticks(rules_.combat.gcd_seconds), dt_) : 0});
}

// ---------------------------------------------------------------- effects

void Zone::resolve_ability(PlayerEntity& p, const AbilityDef& a, EntityId target, ZoneSink& sink) {
  if (a.has(EffectType::PickLock)) return pick_lock(p, a, target, sink);
  switch (a.target) {
    case TargetType::Self:
      apply_beneficial(p, a, p, sink);
      return;
    case TargetType::Group: {
      const EntityId pet_id = p.pet;
      apply_beneficial(p, a, p, sink);
      // Group spells also reach the caster's pet (lasting effects and heals only).
      if (MobEntity* pet = pet_id != 0 ? find_mob(pet_id) : nullptr; pet && pet->alive() && pet->owner == p.id &&
                                                                       horizontal_distance(p.pos, pet->pos) <= std::max(30.0f, a.range))
        apply_to_pet(p, a, *pet, sink);
      return;
    }
    case TargetType::Pet:
      if (MobEntity* pet = find_mob(target); pet && pet->alive() && pet->is_pet()) apply_to_pet(p, a, *pet, sink);
      return;
    case TargetType::Friendly: {
      if (MobEntity* pet = find_mob(target); pet && pet->alive() && pet->is_pet()) return apply_to_pet(p, a, *pet, sink);
      PlayerEntity* t = find(target);
      apply_beneficial(p, a, t ? *t : p, sink);
      return;
    }
    case TargetType::Corpse:
      resurrect(p, a, target, sink);
      return;
    case TargetType::Enemy:
      if (MobEntity* m = hostile_mob(target)) apply_detrimental(p, a, *m, sink);
      return;
    case TargetType::Area: {
      Vec3 centre = p.pos;
      if (a.range > 0.0f) {
        const MobEntity* m = hostile_mob(target);
        if (!m) return;
        centre = m->pos;
      }
      // Copy the hits: applying effects reuses the zone's scratch buffers.
      std::vector<EntityId> near;
      grid_.query(centre.x, centre.z, a.radius, near);
      std::vector<std::pair<float, EntityId>> hits;
      for (EntityId id : near) {
        const MobEntity* m = hostile_mob(id);
        if (!m) continue;
        if (!a.requires_body.empty() && m->def->body != a.requires_body) continue;
        if (!collision_.segment_clear(centre, m->pos)) continue;
        hits.emplace_back(horizontal_distance_sq(centre, m->pos), id);
      }
      std::sort(hits.begin(), hits.end());
      const auto cap = static_cast<std::size_t>(a.max_targets > 0 ? a.max_targets : rules_.spells.max_area_targets);
      if (hits.size() > cap) hits.resize(cap);
      for (const auto& [d, id] : hits)
        if (MobEntity* m = hostile_mob(id); m && find(p.id)) apply_detrimental(p, a, *m, sink);
      return;
    }
  }
}

StackAction Zone::place_buff(std::vector<ActiveBuff>& host, ActiveBuff&& b) {
  const auto d = decide_stacking(host, b, rules_.spells.buff_slots, rules_.spells.debuff_slots);
  if (d.action == StackAction::Add) host.push_back(std::move(b));
  else if (d.action == StackAction::Replace) host[d.index] = std::move(b);
  return d.action;
}

void Zone::apply_detrimental(PlayerEntity& p, const AbilityDef& a, MobEntity& m, ZoneSink& sink) {
  if (m.state == AiState::Leash) {
    combat_event(m.pos, p.id, m.id, CombatKind::Evade, 0, a.name, sink, a.resist);
    return;
  }
  const auto& cr = rules_.combat;
  float fraction = 1.0f;
  const bool magical = a.spell || a.item_effect;  // item effects resolve like spells (resists, no weapon roll)
  if (magical) {
    const float chance = resist_chance(a.resist, mob_resist(m, a.resist) + a.resist_adjust, p.level, m.def->level, rules_.spells);
    const ResistRoll rr = roll_resist(chance, has_effect(a, EffectType::Damage), rules_.spells, rng_);
    if (rr.resisted) {
      combat_event(m.pos, p.id, m.id, CombatKind::Resist, 0, a.name, sink, a.resist);
      engage(m, p, 1.0, sink);
      return;
    }
    fraction = rr.fraction;
  }
  const EntityId mid = m.id;
  bool landed = true;
  bool damaged = false;
  for (const auto& e : a.effects) {
    if (!m.alive() || !find(p.id)) return;
    switch (e.type) {
      case EffectType::Damage: {
        if (!magical) {  // a physical ability: weapon-skill hit roll and armor
          const int skill = std::max(p.skill("offense"), a.skill.empty() ? 0 : p.skill(a.skill));
          if (!rng_.chance(hit_chance(skill, m.stats.defense, p.level, m.def->level, cr))) {
            combat_event(m.pos, p.id, m.id, CombatKind::AbilityMiss, 0, a.name, sink);
            engage(m, p, 1.0, sink);
            landed = false;
            break;
          }
          int dmg = ability_damage(e, p.level, rng_);
          if (a.requires_weapon == "archery") {  // the bow (and arrows) add their share
            const ItemDef* bow = worn(p, EquipSlot::Ranged);
            const ItemDef* ammo = worn(p, EquipSlot::Ammo);
            const float extra = static_cast<float>((bow ? bow->damage : 0) + (ammo ? ammo->damage : 0)) * p.class_def->combat.weapon_mult;
            dmg += static_cast<int>(std::lround(extra * cr.ranged_weapon_share));
          }
          dmg = static_cast<int>(std::lround(static_cast<float>(dmg) * (1.0f + p.buff_total(BuffStat::MeleeDamagePct) / 100.0f)));
          dmg = mitigate(dmg, mob_ac(m), p.level, 1.0f, cr);
          combat_event(m.pos, p.id, m.id, CombatKind::AbilityHit, static_cast<std::uint32_t>(dmg), a.name, sink);
          damaged = true;
          damage_mob(m, p, dmg, 0.0, sink);
          break;
        }
        const int raw = roll_spread(effect_amount(e, p.level), rules_.spells.damage_spread, rng_);
        const int dmg = std::max(1, static_cast<int>(std::lround(static_cast<float>(raw) * fraction)));
        combat_event(m.pos, p.id, m.id, CombatKind::SpellHit, static_cast<std::uint32_t>(dmg), a.name, sink, a.resist);
        damaged = true;
        damage_mob(m, p, dmg, 0.0, sink);
        break;
      }
      case EffectType::Stun: {
        if (!landed) break;
        const bool capped = e.max_target_level > 0 && m.def->level > e.max_target_level;
        const bool lands = magical ? !m.def->stun_immune && !capped : stun_lands(p.level, m.def->level, m.def->stun_immune, cr);
        if (lands) {
          m.stunned_until = tick_ + ticks(e.duration);
          m.swing_ready_tick = std::max(m.swing_ready_tick, m.stunned_until);
          m.changed_tick = tick_;
          combat_event(m.pos, p.id, m.id, CombatKind::Stun, ms_of(ticks(e.duration), dt_), a.name, sink, a.resist);
          engage(m, p, 5.0, sink);
        } else {
          combat_event(m.pos, p.id, m.id, CombatKind::StunFail, 0, a.name, sink, a.resist);
          engage(m, p, 1.0, sink);
        }
        break;
      }
      case EffectType::Threat:
        if (landed) engage(m, p, static_cast<double>(e.base + e.per_level * static_cast<float>(p.level)), sink);
        break;
      case EffectType::Taunt: {
        const SkillCap* cap = p.class_def->skill("taunt");
        const int cap_v = cap ? skill_cap(*cap, p.level) : 0;
        engage(m, p, 1.0, sink);
        if (rng_.chance(taunt_chance(p.skill("taunt"), cap_v, p.level, m.def->level, cr))) {
          m.threat.taunt(p.id);
          combat_event(m.pos, p.id, m.id, CombatKind::Taunt, 0, a.name, sink);
        } else {
          combat_event(m.pos, p.id, m.id, CombatKind::TauntFail, 0, a.name, sink);
        }
        break;
      }
      case EffectType::Lifetap: {
        // Spell damage that flows back to the caster as healing.
        const int raw = roll_spread(effect_amount(e, p.level), rules_.spells.damage_spread, rng_);
        const int dmg = std::max(1, static_cast<int>(std::lround(static_cast<float>(raw) * fraction)));
        const int dealt = std::min(dmg, m.hp);
        combat_event(m.pos, p.id, m.id, CombatKind::Lifetap, static_cast<std::uint32_t>(dmg), a.name, sink, a.resist);
        damaged = true;
        damage_mob(m, p, dmg, 0.0, sink);
        if (!p.dead) {
          const int back = static_cast<int>(std::lround(static_cast<float>(dealt) * (e.value > 0.0f ? e.value / 100.0f : 1.0f)));
          heal(p, p, back, a, CombatKind::Heal, sink);
        }
        break;
      }
      case EffectType::ThreatDrop:
        threat_drop(p, a, e, &m, sink);
        break;
      case EffectType::Dispel: {
        // On an enemy, a dispel strips its beneficial effects.
        const auto idx = dispel_targets(m.buffs, true, static_cast<int>(e.value));
        for (std::size_t i : idx) m.buffs.erase(m.buffs.begin() + static_cast<std::ptrdiff_t>(i));
        if (!idx.empty()) m.buffs_dirty = true;
        combat_event(m.pos, p.id, m.id, CombatKind::Dispel, static_cast<std::uint32_t>(idx.size()), a.name, sink, a.resist);
        break;
      }
      default:
        break;  // lasting effects are placed below; beneficial-only effects never reach here
    }
  }
  if (!find(p.id)) return;
  MobEntity* alive = find_mob(mid);
  if (!alive || !alive->alive() || !landed) return;
  // A lasting line capped below the target's level (a root, mez, or fear) is dropped on its own;
  // the spell's other lasting lines still land.
  const bool any_capped = std::any_of(a.effects.begin(), a.effects.end(), [&](const EffectDef& e) {
    return is_lasting(e.type) && e.max_target_level > 0 && m.def->level > e.max_target_level;
  });
  AbilityDef eligible;
  if (any_capped) {
    eligible = a;
    std::erase_if(eligible.effects, [&](const EffectDef& e) { return is_lasting(e.type) && e.max_target_level > 0 && m.def->level > e.max_target_level; });
    combat_event(m.pos, p.id, m.id, CombatKind::Immune, 0, a.name, sink, a.resist);
  }
  ActiveBuff b = build_buff(any_capped ? eligible : a, p.level, p.id, p.char_id, p.name, tick_, 1.0f / dt_, rules_.spells);
  if (!has_lasting_part(b)) {
    if (!damaged && !a.has(EffectType::ThreatDrop)) engage(m, p, any_capped ? 1.0 : buff_threat(a.level, rules_.spells), sink);
    return;
  }
  if ((b.control() & kControlCharm) != 0) {
    // Charm: the mob becomes the caster's pet for the charm's duration (or until it breaks).
    if (m.def->charm_immune) {
      combat_event(m.pos, p.id, m.id, CombatKind::Immune, 0, a.name, sink, a.resist);
      engage(m, p, buff_threat(a.level, rules_.spells), sink);
      return;
    }
    const std::uint32_t dur = ms_of(b.expires_tick - tick_, dt_);
    if (place_buff(m.buffs, std::move(b)) == StackAction::NoSlot) {
      combat_text(p, "Your " + a.name + " did not take hold: your target cannot hold any more effects.", sink);
      return;
    }
    m.buffs_dirty = true;
    if (make_charmed(p, m, sink)) combat_event(m.pos, p.id, m.id, CombatKind::Charm, dur, a.name, sink, a.resist);
    return;
  }
  const CombatKind kind = lasting_kind(b);
  const std::uint32_t dur = ms_of(b.expires_tick - tick_, dt_);
  switch (place_buff(m.buffs, std::move(b))) {
    case StackAction::Add:
    case StackAction::Replace:
      m.buffs_dirty = true;
      m.changed_tick = tick_;
      combat_event(m.pos, p.id, m.id, kind, dur, a.name, sink, a.resist);
      break;
    case StackAction::Blocked:
      combat_text(p, "Your " + a.name + " did not take hold: a stronger effect is already there.", sink);
      break;
    case StackAction::NoSlot:
      combat_text(p, "Your " + a.name + " did not take hold: your target cannot hold any more effects.", sink);
      break;
  }
  if (!damaged) engage(m, p, buff_threat(a.level, rules_.spells), sink);
}

void Zone::apply_beneficial(PlayerEntity& caster, const AbilityDef& a, PlayerEntity& t, ZoneSink& sink) {
  const float L = static_cast<float>(caster.level);
  for (const auto& e : a.effects) {
    switch (e.type) {
      case EffectType::Heal:
        heal(caster, t, roll_spread(effect_amount(e, caster.level), rules_.spells.damage_spread, rng_), a, CombatKind::Heal, sink);
        break;
      case EffectType::HealPct:
        heal(caster, t, static_cast<int>(std::lround(static_cast<float>(t.stats.max_hp) * e.value / 100.0f)), a, CombatKind::Heal, sink);
        break;
      case EffectType::RestoreEndurancePct: {
        const int amount = static_cast<int>(std::lround(static_cast<float>(t.stats.max_endurance) * e.value / 100.0f));
        t.endurance = std::min(t.stats.max_endurance, t.endurance + std::max(0, amount));
        t.changed_tick = tick_;
        break;
      }
      case EffectType::RestoreManaPct: {
        const int amount = std::min(t.stats.max_mana - t.mana, static_cast<int>(std::lround(static_cast<float>(t.stats.max_mana) * e.value / 100.0f)));
        t.mana += std::max(0, amount);
        t.changed_tick = tick_;
        combat_event(t.pos, caster.id, t.id, CombatKind::ManaRestore, static_cast<std::uint32_t>(std::max(0, amount)), a.name, sink, a.resist);
        break;
      }
      case EffectType::AreaThreat: {
        scratch_dead_.clear();
        grid_.query(caster.pos.x, caster.pos.z, e.radius, scratch_dead_);
        const std::vector<EntityId> near(scratch_dead_);  // engage() reuses the scratch buffers
        for (EntityId id : near) {
          MobEntity* o = find_mob(id);
          if (!o || !o->alive() || o->threat.empty() || o->state == AiState::Leash) continue;
          engage(*o, caster, static_cast<double>(e.base + e.per_level * L), sink);
          combat_event(o->pos, caster.id, o->id, CombatKind::Taunt, 0, a.name, sink);
        }
        mark_combat(caster);
        break;
      }
      case EffectType::Dispel: {
        // On a friend, a dispel (a cure) removes detrimental effects.
        const auto idx = dispel_targets(t.buffs, false, static_cast<int>(e.value));
        for (std::size_t i : idx) t.buffs.erase(t.buffs.begin() + static_cast<std::ptrdiff_t>(i));
        if (!idx.empty()) {
          t.buffs_dirty = true;
          refresh_stats(t);
        }
        combat_event(t.pos, caster.id, t.id, CombatKind::Dispel, static_cast<std::uint32_t>(idx.size()), a.name, sink, a.resist);
        break;
      }
      case EffectType::SummonPet:
        if (t.id == caster.id) summon_pet(caster, a, e, sink);
        break;
      case EffectType::SummonItem:
        summon_item(t, e, sink);
        break;
      case EffectType::FeignDeath:
        feign_death(caster, a, e, sink);
        break;
      case EffectType::Hide:
        try_hide(caster, a, sink);
        break;
      case EffectType::Sneak:
        toggle_sneak(caster, sink);
        break;
      case EffectType::ThreatDrop:
        threat_drop(caster, a, e, nullptr, sink);
        break;
      case EffectType::Track:
        track(caster, e, sink);
        break;
      case EffectType::HpToMana: {
        const int want = effect_amount(e, caster.level);
        const int amount = std::min({want, t.hp - 1, t.stats.max_mana - t.mana});
        if (amount <= 0) {
          combat_text(caster, t.hp <= 1 ? "You are too weak to give up more of your life." : "Your mana is already full.", sink);
          break;
        }
        t.hp -= amount;
        t.mana += amount;
        t.changed_tick = tick_;
        combat_event(t.pos, caster.id, t.id, CombatKind::ManaRestore, static_cast<std::uint32_t>(amount), a.name, sink, a.resist);
        break;
      }
      case EffectType::Teleport: {
        const Vec3 from = t.pos;
        Destination dest;
        if (!resolve_destination(t, e.destination, dest)) break;
        if (t.looting != 0) close_loot(t, sink, "");
        drop_from_threat(t.id);  // whatever was chasing loses track
        combat_event(from, caster.id, t.id, CombatKind::Teleport, 0, a.name, sink, a.resist);
        move_player_to(t, dest, sink, "teleport");
        break;
      }
      default:
        break;  // lasting effects below
    }
  }
  ActiveBuff b = build_buff(a, caster.level, caster.id, caster.char_id, caster.name, tick_, 1.0f / dt_, rules_.spells);
  if (!has_lasting_part(b)) return;
  const std::uint32_t dur = ms_of(b.expires_tick - tick_, dt_);
  const bool illusion = !b.illusion.empty();
  switch (place_buff(t.buffs, std::move(b))) {
    case StackAction::Add:
    case StackAction::Replace:
      t.buffs_dirty = true;
      refresh_stats(t);
      combat_event(t.pos, caster.id, t.id, illusion ? CombatKind::Illusion : CombatKind::Buff, dur, a.name, sink, a.resist);
      if (illusion) respawn_spot_watchers(t.id, sink);
      // Buffing someone in a fight angers what they are fighting (not a whole camp).
      healer_threat(caster, t, buff_threat(a.level, rules_.spells), false, sink);
      break;
    case StackAction::Blocked:
      combat_text(caster, "Your " + a.name + " would not take hold on " + (t.id == caster.id ? std::string("you") : t.name) +
                              ": a stronger effect is already active.", sink);
      break;
    case StackAction::NoSlot:
      combat_text(caster, "Your " + a.name + " would not take hold: no room for more effects.", sink);
      break;
  }
}

void Zone::heal(PlayerEntity& healer, PlayerEntity& t, int amount, const AbilityDef& a, CombatKind kind, ZoneSink& sink) {
  if (t.dead) return;
  const int actual = std::max(0, std::min(t.stats.max_hp - t.hp, amount));
  t.hp += actual;
  t.changed_tick = tick_;
  combat_event(t.pos, healer.id, t.id, kind, static_cast<std::uint32_t>(actual), a.name, sink, a.resist);
  healer_threat(healer, t, static_cast<double>(actual), true, sink);
}

// Healer aggro: healing (or buffing) a player angers every living mob that is already fighting
// that player and is within heal_aggro_radius of the healer. Heal threat is split between those
// mobs (threat.hpp: heal_threat); buff threat is a flat amount per mob. Mobs not yet in the fight
// never notice.
void Zone::healer_threat(PlayerEntity& healer, const PlayerEntity& t, double amount, bool split, ZoneSink& sink) {
  if (amount <= 0.0 || healer.dead) return;
  const float r = rules_.spells.heal_aggro_radius;
  std::vector<EntityId> engaged;
  for (const auto& [id, m] : mobs_)
    if (m.alive() && m.state != AiState::Leash && m.threat.contains(t.id) && horizontal_distance(m.pos, healer.pos) <= r)
      engaged.push_back(id);
  if (engaged.empty()) return;
  const double per = split ? heal_threat(static_cast<int>(std::lround(amount)), engaged.size()) : amount;
  if (per <= 0.0) return;
  for (EntityId id : engaged) {
    MobEntity* m = find_mob(id);
    if (!m || !m->alive()) continue;
    if (healer.id == t.id) {
      m->threat.add(healer.id, per);
      mark_combat(healer);
    } else {
      engage(*m, healer, per, sink);
    }
  }
}

// ---------------------------------------------------------------- resurrection

void Zone::resurrect(PlayerEntity& caster, const AbilityDef& a, EntityId corpse, ZoneSink& sink) {
  CorpseEntity* c = find_corpse(corpse);
  if (!c || !c->player_corpse || c->rezzed) return;
  PlayerEntity* owner = find_by_char(c->owner_char_id);
  if (!owner) return;
  float pct = 0.0f;
  for (const auto& e : a.effects)
    if (e.type == EffectType::Resurrect) pct = std::max(pct, e.value);
  const std::uint32_t offer_id = next_rez_offer_++;
  if (next_rez_offer_ == 0) next_rez_offer_ = 1;
  owner->rez = RezOffer{offer_id, c->id, pct, caster.id, caster.name, tick_ + ticks(rules_.spells.rez_offer_seconds)};
  send(sink, *owner, proto::RezOffer{offer_id, caster.name, static_cast<std::uint8_t>(std::clamp(std::lround(pct), 0L, 100L)), c->id,
                                     seconds_ms(rules_.spells.rez_offer_seconds)});
  combat_event(c->pos, caster.id, c->id, CombatKind::Resurrect, static_cast<std::uint32_t>(std::lround(pct)), a.name, sink, a.resist);
  send(sink, caster, proto::ChatMessage{kChatSystem, "", "You call to " + c->owner_name + "'s spirit. They must accept the resurrection."});
}

void Zone::rez_response(EntityId player, std::uint32_t offer_id, bool accept, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (p.rez.corpse == 0 || p.rez.id != offer_id) return;  // stale: a newer offer replaced it (or none)
  const RezOffer o = p.rez;
  p.rez = {};
  if (o.corpse == 0) return;
  auto tell = [&](PlayerEntity& to, const std::string& text) { send(sink, to, proto::ChatMessage{kChatSystem, "", text}); };
  if (tick_ > o.expires_tick) return tell(p, "The resurrection has faded; it was not accepted in time.");
  CorpseEntity* c = find_corpse(o.corpse);
  if (!c || c->owner_char_id != p.char_id || c->rezzed) return tell(p, "Your corpse can no longer be resurrected.");
  PlayerEntity* caster = find(o.caster);
  if (!accept) {
    tell(p, "You decline the resurrection.");
    if (caster) tell(*caster, p.name + " declines your resurrection.");
    return;
  }
  const std::uint64_t restore = rez_xp(c->xp_lost, o.pct);
  c->xp_lost = 0;
  c->rezzed = true;
  if (p.dead) {
    p.dead = false;
    p.hp = std::max(1, static_cast<int>(std::lround(static_cast<float>(p.stats.max_hp) * rules_.spells.rez_hp_pct)));
    p.mana = static_cast<int>(std::lround(static_cast<float>(p.stats.max_mana) * rules_.spells.rez_hp_pct));
    p.endurance = static_cast<int>(std::lround(static_cast<float>(p.stats.max_endurance) * rules_.spells.rez_hp_pct));
    p.hp_acc = p.mana_acc = p.end_acc = 0.0f;
  }
  relocate(p, c->pos, c->yaw, sink, "resurrect");
  const std::uint64_t cap = total_xp_for_level(rules_.max_level, rules_.xp);
  const std::uint64_t gained = std::min(restore, cap - std::min(cap, p.xp));
  p.xp += gained;
  tell(p, "You have been resurrected by " + o.caster_name + (gained > 0 ? "! You regain " + std::to_string(gained) + " experience." : "!"));
  if (caster) tell(*caster, p.name + " accepts your resurrection.");
  combat_event(c->pos, o.caster, p.id, CombatKind::Resurrect, static_cast<std::uint32_t>(std::min<std::uint64_t>(gained, 0xFFFFFFFFu)), "", sink);
  recompute_level(p, sink, true, false);  // the corpse write below saves the character too
  respawn_spot_watchers(p.id, sink);
  sink.zone_corpse_resurrected(*c, p);
}

// ---------------------------------------------------------------- ticking effects

namespace {

struct PendingTick {
  EntityId caster;
  int amount;
  std::string caster_name;
  std::string ability;
  ResistType school;
  float drain;
};

// Takes every periodic tick due by `now` (a line's final tick lands on its expiry), then drops the
// lines that have ended. Returns true if any line ended.
bool collect_ticks(ActiveBuff& b, std::uint32_t now, std::vector<PendingTick>& out) {
  for (auto& l : b.periodics)
    while (l.next_tick <= now && l.next_tick <= l.expires_tick) {
      l.next_tick += l.interval;
      out.push_back({b.caster, l.amount, b.caster_name, b.name, b.school, l.drain});
    }
  return prune_lines(b, now);
}

}  // namespace

void Zone::tick_player_effects(PlayerEntity& p, ZoneSink& sink) {
  std::vector<PendingTick> ticks_due;
  bool changed = false;
  const std::string* look_before = illusion_of(p);
  const std::string before = look_before ? *look_before : std::string();
  for (auto it = p.buffs.begin(); it != p.buffs.end();) {
    changed |= collect_ticks(*it, tick_, ticks_due);
    if (has_lasting_part(*it) && it->expires_tick > tick_) {
      ++it;
      continue;
    }
    // Only the player hears about their own effects fading.
    send(sink, p, proto::CombatEvent{it->caster, p.id, static_cast<std::uint8_t>(CombatKind::BuffFade), 0, it->name, static_cast<std::uint8_t>(it->school)});
    it = p.buffs.erase(it);
    changed = true;
  }
  if (changed) {
    p.buffs_dirty = true;
    refresh_stats(p);
    const std::string* look_after = illusion_of(p);
    if ((look_after ? *look_after : std::string()) != before) respawn_spot_watchers(p.id, sink);
  }
  for (const auto& t : ticks_due) {
    if (p.dead) return;
    if (t.amount > 0) {
      const int actual = std::max(0, std::min(p.stats.max_hp - p.hp, t.amount));
      p.hp += actual;
      p.changed_tick = tick_;
      combat_event(p.pos, t.caster, p.id, CombatKind::HotTick, static_cast<std::uint32_t>(actual), t.ability, sink, t.school);
      if (PlayerEntity* healer = find(t.caster)) healer_threat(*healer, p, static_cast<double>(actual), true, sink);
    } else {
      combat_event(p.pos, t.caster, p.id, CombatKind::DotTick, static_cast<std::uint32_t>(-t.amount), t.ability, sink, t.school);
      damage_player(p, -t.amount, t.caster, t.caster_name.empty() ? t.ability : t.caster_name, sink);
    }
  }
}

void Zone::tick_mob_effects(MobEntity& m, ZoneSink& sink) {
  std::vector<PendingTick> ticks_due;
  for (auto it = m.buffs.begin(); it != m.buffs.end();) {
    ActiveBuff& b = *it;
    if (b.harms_over_time() && b.caster != 0 && !find(b.caster)) {  // DoTs end when their caster leaves
      std::erase_if(b.periodics, [](const PeriodicLine& l) { return l.amount < 0; });
      m.buffs_dirty = true;
    }
    if (collect_ticks(b, tick_, ticks_due)) {
      m.buffs_dirty = true;
      m.changed_tick = tick_;
    }
    if (has_lasting_part(b) && b.expires_tick > tick_) {
      ++it;
      continue;
    }
    if (PlayerEntity* caster = find(b.caster))
      send(sink, *caster, proto::CombatEvent{caster->id, m.id, static_cast<std::uint8_t>(CombatKind::BuffFade), 0, b.name, static_cast<std::uint8_t>(b.school)});
    it = m.buffs.erase(it);
    m.buffs_dirty = true;
    m.changed_tick = tick_;
  }
  const EntityId mid = m.id;
  for (const auto& t : ticks_due) {
    MobEntity* mm = find_mob(mid);
    if (!mm || !mm->alive()) return;
    if (t.amount < 0) {
      PlayerEntity* caster = find(t.caster);
      combat_event(mm->pos, t.caster, mid, t.drain > 0.0f ? CombatKind::DrainTick : CombatKind::DotTick, static_cast<std::uint32_t>(-t.amount),
                   t.ability, sink, t.school);
      if (caster) {
        const int dealt = std::min(-t.amount, mm->hp);
        damage_mob(*mm, *caster, -t.amount, 0.0, sink, false);
        // A drain's tick flows back to its caster (no healer threat: it is the caster's own life).
        if (t.drain > 0.0f && !caster->dead && caster->hp < caster->stats.max_hp) {
          const int back = std::min(caster->stats.max_hp - caster->hp, static_cast<int>(std::lround(static_cast<float>(dealt) * t.drain)));
          if (back > 0) {
            caster->hp += back;
            caster->changed_tick = tick_;
            combat_event(caster->pos, caster->id, caster->id, CombatKind::HotTick, static_cast<std::uint32_t>(back), t.ability, sink, t.school);
          }
        }
      } else {  // a world-cast DoT (force_effect): damage without loot or XP credit
        mm->hp -= -t.amount;
        mm->changed_tick = tick_;
        on_mob_damaged(*mm, false, sink);
        if (mm->hp <= 0) kill_mob(*mm, nullptr, sink);
      }
    } else {
      mm->hp = std::min(mm->stats.max_hp, mm->hp + t.amount);
      mm->changed_tick = tick_;
      combat_event(mm->pos, t.caster, mid, CombatKind::HotTick, static_cast<std::uint32_t>(t.amount), t.ability, sink, t.school);
    }
  }
}

// Damage breaks mez, and direct hits sometimes break root. Only the broken control line goes: a
// DoT or debuff in the same spell keeps running.
void Zone::on_mob_damaged(MobEntity& m, bool direct, ZoneSink& sink) {
  for (auto it = m.buffs.begin(); it != m.buffs.end();) {
    const std::uint8_t ctl = it->control();
    const bool mez = (ctl & kControlMez) != 0;
    const bool root_breaks = direct && (ctl & kControlRoot) != 0 && rng_.chance(rules_.spells.root_break_chance);
    if (!mez && !root_breaks) {
      ++it;
      continue;
    }
    combat_event(m.pos, it->caster, m.id, mez ? CombatKind::MezBreak : CombatKind::RootBreak, 0, it->name, sink, it->school);
    strip_control(*it, static_cast<std::uint8_t>((mez ? kControlMez : 0) | (root_breaks ? kControlRoot : 0)));
    m.buffs_dirty = true;
    m.changed_tick = tick_;
    if (has_lasting_part(*it)) ++it;
    else it = m.buffs.erase(it);
  }
}

StackAction Zone::force_effect(EntityId target, const AbilityDef& spell, int caster_level, ZoneSink& sink) {
  ActiveBuff b = build_buff(spell, caster_level, 0, 0, "", tick_, 1.0f / dt_, rules_.spells);
  if (!has_lasting_part(b)) return StackAction::Blocked;
  if (PlayerEntity* p = find(target)) {
    const StackAction act = place_buff(p->buffs, std::move(b));
    if (act == StackAction::Add || act == StackAction::Replace) {
      p->buffs_dirty = true;
      refresh_stats(*p);
      cast_tick(*p, sink);  // mez or fear interrupts a cast at once
    }
    return act;
  }
  if (MobEntity* m = find_mob(target); m && m->alive()) {
    const StackAction act = place_buff(m->buffs, std::move(b));
    if (act == StackAction::Add || act == StackAction::Replace) {
      m->buffs_dirty = true;
      m->changed_tick = tick_;
    }
    return act;
  }
  return StackAction::Blocked;
}

// ---------------------------------------------------------------- wire updates

void Zone::send_buff_list(const PlayerEntity& to, EntityId of, ZoneSink& sink) {
  proto::BuffList msg;
  msg.entity_id = of;
  const std::vector<ActiveBuff>* buffs = nullptr;
  if (const PlayerEntity* p = find(of)) buffs = &p->buffs;
  else if (MobEntity* m = find_mob(of); m && m->alive()) buffs = &m->buffs;
  if (buffs) {
    for (const auto& b : *buffs) {
      if (msg.buffs.size() >= 48) break;
      msg.buffs.push_back({b.ability_id, b.name, ms_of(b.expires_tick > tick_ ? b.expires_tick - tick_ : 0, dt_), b.beneficial,
                           b.caster_name.substr(0, 32), static_cast<std::uint8_t>(b.school)});
    }
  }
  send(sink, to, msg);
}

void Zone::flush_buff_lists(ZoneSink& sink) {
  for (auto& [id, p] : players_) {
    if (!p.buffs_dirty) continue;
    p.buffs_dirty = false;
    send_buff_list(p, p.id, sink);
    for (const auto& [oid, o] : players_)
      if (oid != id && o.target == id) send_buff_list(o, id, sink);
  }
  for (auto& [id, m] : mobs_) {
    if (!m.buffs_dirty) continue;
    m.buffs_dirty = false;
    for (const auto& [oid, o] : players_)
      if (o.target == id) send_buff_list(o, id, sink);
  }
}

void Zone::send_spellbook(PlayerEntity& p, ZoneSink& sink) {
  proto::SpellBook book;
  book.capacity = static_cast<std::uint16_t>(rules_.spells.spellbook_size);
  std::vector<const AbilityDef*> known;
  for (const auto& id : p.spellbook)
    if (const AbilityDef* a = content_.find_spell(id)) known.push_back(a);
  std::stable_sort(known.begin(), known.end(), [](const AbilityDef* a, const AbilityDef* b) { return a->level < b->level; });
  for (const AbilityDef* a : known) {
    if (book.spells.size() >= 128) break;
    book.spells.push_back({a->id, a->name, a->type, static_cast<std::uint8_t>(std::clamp(a->level, 1, 255)),
                           static_cast<std::uint16_t>(std::clamp(a->cost, 0, 65535)), seconds_ms(a->cast_time), seconds_ms(a->cooldown),
                           seconds_ms(a->lasting_seconds()), a->range, static_cast<std::uint8_t>(a->target),
                           static_cast<std::uint8_t>(a->resist), a->description, a->song});
  }
  send(sink, p, book);
}

void Zone::send_gems(PlayerEntity& p, ZoneSink& sink) {
  proto::GemBar bar;
  bar.unlocked = static_cast<std::uint8_t>(std::min<int>(gem_count(p.level, rules_.spells), static_cast<int>(kMaxGems)));
  for (std::size_t g = 0; g < kMaxGems; ++g) {
    const std::string& id = p.gems[g];
    const std::uint32_t ready = id.empty() ? 0 : std::max(p.gem_ready[g], cooldown_ready(p, id));
    bar.gems.push_back({id, ms_of(ready > tick_ ? ready - tick_ : 0, dt_)});
  }
  send(sink, p, bar);
}

// ---------------------------------------------------------------- spellbook and gems

void Zone::memorize_spell(EntityId player, int gem, std::string_view spell_id, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto tell = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (gem < 0 || gem >= gem_count(p.level, rules_.spells) || gem >= static_cast<int>(kMaxGems)) return tell("You cannot use that spell gem yet.");
  const auto g = static_cast<std::size_t>(gem);
  if (p.cast.casting()) return tell("You cannot memorize spells while casting.");
  if (spell_id.empty()) {
    if (!p.gems[g].empty()) {
      p.gems[g].clear();
      send_gems(p, sink);
      sink.zone_character_changed(p);
    }
    return;
  }
  const AbilityDef* a = content_.find_spell(spell_id);
  if (!a || !p.knows_spell(spell_id)) return tell("That spell is not in your spellbook.");
  if (in_combat(p)) return tell("You cannot memorize spells in combat.");
  if (p.level < a->level) return tell("You are not experienced enough to memorize " + a->name + ".");
  if (p.gems[g] == a->id) return;
  for (auto& other : p.gems)
    if (other == a->id) other.clear();  // a spell sits in one gem at a time
  p.gems[g] = a->id;
  p.gem_ready[g] = tick_ + ticks(rules_.spells.memorize_seconds);
  tell("You have memorized " + a->name + ".");
  send_gems(p, sink);
  sink.zone_character_changed(p);
}

void Zone::scribe_spell(EntityId player, int slot, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto tell = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (p.trade != 0) return tell("You cannot do that while trading.");
  const auto code = static_cast<SlotCode>(std::clamp(slot, 0, 0xFFFF));
  ItemStackState* sp = slot >= 0 && slot <= 0xFFFF && !in_bank(code) && slot_valid(p.inv, code, content_, rules_.economy.bank_slots) ? p.inv.at(code) : nullptr;
  if (!sp || sp->empty()) return tell("There is nothing there to scribe.");
  auto& stack = *sp;
  const ItemDef* item = content_.find_item(stack.item_id);
  if (!item || item->kind != "scroll") return tell("That is not a spell scroll.");
  const AbilityDef* a = content_.find_spell(item->spell);
  if (!a) return tell("The writing on that scroll is illegible.");
  if (a->class_id != p.class_id) {
    const ClassDef* cl = content_.find_class(a->class_id);
    return tell("Only a " + (cl ? cl->name : a->class_id) + " can scribe " + a->name + ".");
  }
  if (p.level < a->level) return tell("You must be level " + std::to_string(a->level) + " to scribe " + a->name + ".");
  if (p.knows_spell(a->id)) return tell("You already know " + a->name + ".");
  if (p.spellbook.size() >= static_cast<std::size_t>(rules_.spells.spellbook_size)) return tell("Your spellbook is full.");
  if (p.cast.casting()) return tell("You cannot scribe while casting.");
  if (--stack.count <= 0) stack = {};
  p.spellbook.push_back(a->id);
  tell("You have scribed " + a->name + " into your spellbook.");
  mark_inventory(p);
  send_inventory(p, sink);
  send_spellbook(p, sink);
  sink.zone_character_changed(p);
}

// ---------------------------------------------------------------- trainers

void Zone::trainer_open(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  const NpcEntity* n = find_npc(npc);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto tell = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (!n || !std::binary_search(p.known.begin(), p.known.end(), npc)) return tell("You don't see anyone to talk to.");
  if (p.dead) return tell("You are dead.");
  if (horizontal_distance(p.pos, n->pos) > rules_.combat.loot_range * 1.5f) return tell("You are too far away to talk to " + n->def->name + ".");
  const bool teaches = std::find(n->def->classes.begin(), n->def->classes.end(), p.class_id) != n->def->classes.end();
  if (!teaches) return send(sink, p, proto::ChatMessage{kChatSay, n->def->name, "I have nothing to teach a " + p.class_def->name + ". Seek your own guild."});
  proto::TrainerWindow w;
  w.npc_id = n->id;
  w.name = n->def->name;
  w.greeting = n->def->greeting;
  for (const auto& id : n->def->sells) {
    const ItemDef* it = content_.find_item(id);
    const AbilityDef* a = it ? content_.find_spell(it->spell) : nullptr;
    if (!a || a->class_id != p.class_id || w.entries.size() >= 128) continue;
    w.entries.push_back({it->id, a->id, a->name, static_cast<std::uint8_t>(std::clamp(a->level, 1, 255)),
                         static_cast<std::uint32_t>(std::max(0, it->value)), p.knows_spell(a->id)});
  }
  std::stable_sort(w.entries.begin(), w.entries.end(), [](const auto& a, const auto& b) { return a.level < b.level; });
  send(sink, p, w);
}

void Zone::trainer_buy(EntityId player, EntityId npc, std::string_view item_id, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  const NpcEntity* n = find_npc(npc);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto tell = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (!n || !std::binary_search(p.known.begin(), p.known.end(), npc)) return tell("You don't see anyone to buy from.");
  if (p.dead) return tell("You are dead.");
  if (horizontal_distance(p.pos, n->pos) > rules_.combat.loot_range * 1.5f) return tell("You are too far away to trade with " + n->def->name + ".");
  if (p.trade != 0) return tell("You cannot do that while trading.");
  const bool sold = std::find(n->def->sells.begin(), n->def->sells.end(), item_id) != n->def->sells.end();
  const ItemDef* it = sold ? content_.find_item(item_id) : nullptr;
  const AbilityDef* a = it ? content_.find_spell(it->spell) : nullptr;
  const bool teaches = std::find(n->def->classes.begin(), n->def->classes.end(), p.class_id) != n->def->classes.end();
  if (!it || !a || !teaches || a->class_id != p.class_id) return tell(n->def->name + " does not sell that to you.");
  const auto price = static_cast<std::uint64_t>(std::max(0, it->value));
  if (p.coin < price) return tell("You cannot afford " + it->name + " (" + format_coin(price) + ").");
  ItemStackState stack{it->id, 1};
  if (!give_item(p, stack)) return tell("Your pack is full.");
  p.coin -= price;
  tell("You buy " + it->name + " for " + format_coin(price) + ".");
  send_inventory(p, sink);
  sink.zone_character_changed(p);
}

// ---------------------------------------------------------------- persistence

void Zone::restore_saved(PlayerEntity& p, const ZonePlayerInit& init) {
  const bool fresh = init.spellbook.empty();
  for (const auto& id : fresh ? p.class_def->starter_spells : init.spellbook) {
    const AbilityDef* a = content_.find_spell(id);
    if (!a || a->class_id != p.class_id || p.knows_spell(id)) continue;
    if (p.spellbook.size() >= static_cast<std::size_t>(rules_.spells.spellbook_size)) break;
    p.spellbook.push_back(id);
  }
  const bool no_gems = std::all_of(init.gems.begin(), init.gems.end(), [](const std::string& g) { return g.empty(); });
  const auto& gems = fresh && no_gems ? p.class_def->starter_spells : init.gems;
  for (std::size_t g = 0; g < kMaxGems && g < gems.size(); ++g)
    if (p.knows_spell(gems[g]) && std::find(p.gems.begin(), p.gems.end(), gems[g]) == p.gems.end()) p.gems[g] = gems[g];
  for (const auto& sb : init.buffs) {
    const AbilityDef* a = any_ability(sb.ability_id);
    if (!a || !a->beneficial() || !(sb.seconds_left > 0.0f)) continue;
    ActiveBuff b = build_buff(*a, std::clamp(sb.caster_level, 1, rules_.max_level), 0, 0, sb.caster_name, tick_, 1.0f / dt_, rules_.spells);
    if (!has_lasting_part(b)) continue;
    // Every line ends no later than the saved time left; a tick that was due soon stays due soon.
    const std::uint32_t ends = tick_ + std::min(b.expires_tick - tick_, ticks(sb.seconds_left));
    b.expires_tick = ends;
    for (auto& l : b.mods) l.expires_tick = std::min(l.expires_tick, ends);
    for (auto& l : b.controls) l.expires_tick = std::min(l.expires_tick, ends);
    for (auto& l : b.periodics) {
      l.expires_tick = std::min(l.expires_tick, ends);
      if (sb.next_tick_seconds > 0.0f) l.next_tick = tick_ + std::min(l.interval, std::max<std::uint32_t>(1, ticks(sb.next_tick_seconds)));
    }
    place_buff(p.buffs, std::move(b));
  }
  for (const auto& t : init.timers)
    if (any_ability(t.ability_id) && t.seconds_left > 0.0f) set_cooldown(p, t.ability_id, tick_ + ticks(std::min(t.seconds_left, 86400.0f)));
  refresh_stats(p);
}

std::vector<SavedBuff> Zone::saved_buffs(const PlayerEntity& p) const {
  std::vector<SavedBuff> out;
  for (const auto& b : p.buffs)
    if (b.beneficial && b.expires_tick > tick_) {
      float next = 0.0f;
      if (!b.periodics.empty() && b.periodics.front().next_tick > tick_) next = static_cast<float>(b.periodics.front().next_tick - tick_) * dt_;
      out.push_back({b.ability_id, b.caster_level, b.caster_name, static_cast<float>(b.expires_tick - tick_) * dt_, next});
    }
  return out;
}

std::vector<SavedTimer> Zone::saved_timers(const PlayerEntity& p) const {
  std::vector<SavedTimer> out;
  for (const auto& [id, ready] : p.cooldowns)
    if (ready > tick_) out.push_back({id, static_cast<float>(ready - tick_) * dt_});
  return out;
}

}  // namespace ef::game
