// Zone combat: targeting, auto-attack, abilities, damage, death, experience, regeneration, and
// skill-ups. The math itself lives in rules.cpp; this file applies it to entities.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

CombatKind swing_kind(SwingResult r) {
  switch (r) {
    case SwingResult::Miss: return CombatKind::Miss;
    case SwingResult::Hit: return CombatKind::Hit;
    case SwingResult::Crit: return CombatKind::Crit;
    case SwingResult::Dodge: return CombatKind::Dodge;
    case SwingResult::Parry: return CombatKind::Parry;
    case SwingResult::Riposte: return CombatKind::Riposte;
  }
  return CombatKind::Miss;
}

std::uint32_t ms(std::uint32_t ticks, float dt) { return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0)); }

std::uint8_t pct(int hp, int max_hp) {
  if (max_hp <= 0 || hp <= 0) return 0;
  return static_cast<std::uint8_t>(std::clamp((hp * 100 + max_hp - 1) / max_hp, 1, 100));
}

}  // namespace

// ---------------------------------------------------------------- helpers

float Zone::reach(const MobEntity& m) const { return rules_.combat.melee_range + 0.5f * m.def->scale; }

bool Zone::in_combat(const PlayerEntity& p) const {
  return p.ever_in_combat && tick_ - p.last_combat_tick <= ticks(rules_.combat.in_combat_seconds);
}

void Zone::mark_combat(PlayerEntity& p) {
  p.last_combat_tick = tick_;
  p.ever_in_combat = true;
}

void Zone::stand(PlayerEntity& p) {
  if (!p.sitting) return;
  p.sitting = false;
  p.changed_tick = tick_;
}

void Zone::combat_event(const Vec3& where, EntityId source, EntityId target, CombatKind kind, std::uint32_t amount,
                        std::string_view ability, ZoneSink& sink) {
  scratch_near_.clear();
  grid_.query(where.x, where.z, rules_.combat.event_radius, scratch_near_);
  const proto::CombatEvent ev{source, target, static_cast<std::uint8_t>(kind), amount, std::string(ability)};
  for (EntityId id : scratch_near_)
    if (const PlayerEntity* p = find(id)) send(sink, *p, ev);
}

void Zone::combat_text(PlayerEntity& p, const std::string& text, ZoneSink& sink) {
  send(sink, p, proto::ChatMessage{kChatCombat, "", text});
}

std::uint32_t Zone::cooldown_ready(const PlayerEntity& p, std::string_view ability) const {
  for (const auto& [id, t] : p.cooldowns)
    if (id == ability) return t;
  return 0;
}

void Zone::set_cooldown(PlayerEntity& p, std::string_view ability, std::uint32_t ready) {
  for (auto& [id, t] : p.cooldowns)
    if (id == ability) {
      t = ready;
      return;
    }
  p.cooldowns.emplace_back(std::string(ability), ready);
}

void Zone::drop_from_threat(EntityId player) {
  for (auto& [id, m] : mobs_) m.threat.remove(player);
}

void Zone::call_assist(MobEntity& m, EntityId target) {
  scratch_near_.clear();
  grid_.query(m.pos.x, m.pos.z, 100.0f, scratch_near_);
  for (EntityId id : scratch_near_) {
    if (id == m.id) continue;
    MobEntity* o = find_mob(id);
    if (!o || !o->alive() || !o->def->assist || o->def->faction != m.def->faction) continue;
    if (o->state != AiState::Idle && o->state != AiState::Wander) continue;
    if (horizontal_distance(o->pos, m.pos) > o->def->assist_radius) continue;
    o->threat.add(target, 1.0);
    set_mob_state(*o, AiState::Chase);
    o->last_melee_tick = tick_;
  }
}

void Zone::engage(MobEntity& m, PlayerEntity& p, double threat, ZoneSink& /*sink*/) {
  if (!m.alive() || m.state == AiState::Leash) return;
  const bool was_calm = m.state == AiState::Idle || m.state == AiState::Wander;
  m.threat.add(p.id, std::max(threat, 0.01));
  mark_combat(p);
  if (was_calm) {
    set_mob_state(m, AiState::Chase);
    m.last_melee_tick = tick_;
    call_assist(m, p.id);
  }
}

// ---------------------------------------------------------------- skills

void Zone::unlock_skills(PlayerEntity& p) {
  for (const auto& [sid, cap] : p.class_def->skills) {
    if (skill_cap(cap, p.level) <= 0) continue;
    const bool have = std::any_of(p.skills.begin(), p.skills.end(), [&](const SkillState& s) { return s.id == sid; });
    if (!have) p.skills.push_back({sid, 1});
  }
}

void Zone::send_skills(PlayerEntity& p, ZoneSink& sink) {
  proto::SkillValues msg;
  msg.announce = false;
  for (const auto& s : p.skills) {
    const SkillCap* cap = p.class_def->skill(s.id);
    const int c = cap ? skill_cap(*cap, p.level) : 0;
    if (c <= 0) continue;
    const SkillDef* def = content_.find_skill(s.id);
    msg.skills.push_back({s.id, def ? def->name : s.id, static_cast<std::uint16_t>(std::clamp(s.value, 0, 65535)),
                          static_cast<std::uint16_t>(std::clamp(c, 0, 65535))});
  }
  send(sink, p, msg);
}

void Zone::try_skill_up(PlayerEntity& p, std::string_view skill, ZoneSink& sink) {
  const SkillCap* cap = p.class_def->skill(skill);
  if (!cap) return;
  const int c = skill_cap(*cap, p.level);
  if (c <= 0) return;
  for (auto& s : p.skills) {
    if (s.id != skill) continue;
    if (!rng_.chance(skill_up_chance(s.value, c, rules_.combat))) return;
    ++s.value;
    const SkillDef* def = content_.find_skill(s.id);
    proto::SkillValues msg;
    msg.announce = true;
    msg.skills.push_back({s.id, def ? def->name : s.id, static_cast<std::uint16_t>(std::clamp(s.value, 0, 65535)),
                          static_cast<std::uint16_t>(std::clamp(c, 0, 65535))});
    send(sink, p, msg);
    return;
  }
}

// ---------------------------------------------------------------- inventory and target info

void Zone::send_inventory(PlayerEntity& p, ZoneSink& sink) {
  proto::Inventory inv;
  inv.coin = p.coin;
  inv.slots = static_cast<std::uint8_t>(kInventorySlots);
  for (std::size_t i = 0; i < kInventorySlots; ++i) {
    const auto& s = p.inventory[i];
    if (s.empty()) continue;
    const ItemDef* def = content_.find_item(s.item_id);
    inv.items.push_back({static_cast<std::uint8_t>(i), s.item_id, def ? def->name : s.item_id,
                         static_cast<std::uint16_t>(std::clamp(s.count, 0, 65535))});
  }
  send(sink, p, inv);
}

void Zone::send_target_info(PlayerEntity& p, ZoneSink& sink) {
  proto::TargetInfo t;
  t.entity_id = p.target;
  if (p.target == p.id) {
    t.con = static_cast<std::uint8_t>(Con::White);
    t.level = static_cast<std::uint8_t>(p.level);
    t.hp_pct = pct(p.hp, p.stats.max_hp);
    t.name = p.name;
  } else if (const MobEntity* m = find_mob(p.target); m && m->alive()) {
    t.con = static_cast<std::uint8_t>(con_color(p.level, m->def->level, rules_.con));
    t.level = static_cast<std::uint8_t>(m->def->level);
    t.hp_pct = pct(m->hp, m->stats.max_hp);
    t.name = m->def->name;
  } else if (const PlayerEntity* o = find(p.target)) {
    t.con = static_cast<std::uint8_t>(con_color(p.level, o->level, rules_.con));
    t.level = static_cast<std::uint8_t>(o->level);
    t.hp_pct = pct(o->hp, o->stats.max_hp);
    t.name = o->name;
  } else if (const CorpseEntity* c = find_corpse(p.target)) {
    t.con = static_cast<std::uint8_t>(Con::Grey);
    t.level = static_cast<std::uint8_t>(std::clamp(c->level, 1, 255));
    t.hp_pct = 0;
    t.name = c->name;
  } else {
    p.target = 0;
    t.entity_id = 0;
  }
  send(sink, p, t);
}

// ---------------------------------------------------------------- intents

void Zone::set_target(EntityId player, EntityId target, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p) return;
  // Only things the client can see (or yourself) are targetable.
  const bool visible = target == 0 || target == p->id || std::binary_search(p->known.begin(), p->known.end(), target);
  p->target = visible ? target : 0;
  send_target_info(*p, sink);
}

void Zone::set_auto_attack(EntityId player, bool enabled, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p) return;
  if (!enabled) {
    if (p->auto_attack) combat_text(*p, "Auto attack is off.", sink);
    p->auto_attack = false;
    return;
  }
  if (p->dead) return;
  const MobEntity* m = find_mob(p->target);
  if (!m || !m->alive()) {
    combat_text(*p, p->target == 0 ? "You need a target to attack." : "You can't attack that.", sink);
    p->auto_attack = false;
    return;
  }
  stand(*p);
  if (!p->auto_attack) combat_text(*p, "Auto attack is on.", sink);
  p->auto_attack = true;
}

void Zone::set_sitting(EntityId player, bool sitting, ZoneSink& /*sink*/) {
  PlayerEntity* p = find(player);
  if (!p || p->dead || p->sitting == sitting) return;
  p->sitting = sitting;
  if (sitting) p->auto_attack = false;
  p->changed_tick = tick_;
}

void Zone::ability_fail(PlayerEntity& p, const AbilityDef& a, const std::string& message, ZoneSink& sink) {
  const std::uint32_t cd = cooldown_ready(p, a.id);
  const std::uint32_t cd_left = cd > tick_ ? cd - tick_ : 0;
  const std::uint32_t gcd_left = a.gcd && p.gcd_until > tick_ ? p.gcd_until - tick_ : 0;
  send(sink, p, proto::AbilityResult{false, a.id, message, ms(cd_left, dt_), ms(gcd_left, dt_)});
}

void Zone::use_ability(EntityId player, std::string_view ability_id, EntityId target, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  const AbilityDef* a = content_.find_ability(ability_id);
  if (!a || a->class_id != p.class_id) {
    send(sink, p, proto::AbilityResult{false, std::string(ability_id), "You do not know that ability.", 0, 0});
    return;
  }
  if (a->passive()) return ability_fail(p, *a, a->name + " is passive; it works on its own.", sink);
  if (p.level < a->level) return ability_fail(p, *a, "You must be level " + std::to_string(a->level) + " to use " + a->name + ".", sink);
  if (p.dead) return ability_fail(p, *a, "You are dead.", sink);
  if (p.stunned_until > tick_) return ability_fail(p, *a, "You are stunned!", sink);
  if (tick_ < cooldown_ready(p, a->id)) return ability_fail(p, *a, a->name + " is not ready yet.", sink);
  if (a->gcd && tick_ < p.gcd_until) return ability_fail(p, *a, "You must wait a moment.", sink);
  if (a->resource == Resource::Endurance && p.endurance < a->cost) return ability_fail(p, *a, "You are too fatigued to use " + a->name + ".", sink);
  if (a->resource == Resource::Mana && p.mana < a->cost) return ability_fail(p, *a, "Insufficient mana to use " + a->name + ".", sink);

  MobEntity* m = nullptr;
  if (a->target == TargetType::Enemy) {
    const EntityId tid = target != 0 ? target : p.target;
    m = find_mob(tid);
    if (!m || !m->alive()) {
      if (tid != 0 && (find(tid) || find_corpse(tid))) return ability_fail(p, *a, "You can't use " + a->name + " on that.", sink);
      return ability_fail(p, *a, "You need a target for " + a->name + ".", sink);
    }
    if (horizontal_distance(p.pos, m->pos) > a->range + 0.5f * m->def->scale) return ability_fail(p, *a, "Your target is out of range.", sink);
  }

  if (a->resource == Resource::Endurance) p.endurance -= a->cost;
  if (a->resource == Resource::Mana) p.mana -= a->cost;
  const std::uint32_t cd_ticks = ticks(a->cooldown);
  const std::uint32_t gcd_ticks = a->gcd ? ticks(rules_.combat.gcd_seconds) : 0;
  set_cooldown(p, a->id, tick_ + cd_ticks);
  if (a->gcd) p.gcd_until = tick_ + gcd_ticks;
  stand(p);
  p.changed_tick = tick_;
  if (m) mark_combat(p);
  if (!a->skill.empty()) try_skill_up(p, a->skill, sink);

  const auto& cr = rules_.combat;
  const float L = static_cast<float>(p.level);
  bool landed = true;
  for (const auto& e : a->effects) {
    switch (e.type) {
      case EffectType::Damage: {
        if (!m || !m->alive()) break;
        if (m->state == AiState::Leash) {
          combat_event(m->pos, p.id, m->id, CombatKind::Evade, 0, a->name, sink);
          landed = false;
          break;
        }
        const int skill = std::max(p.skill("offense"), a->skill.empty() ? 0 : p.skill(a->skill));
        if (!rng_.chance(hit_chance(skill, m->stats.defense, p.level, m->def->level, cr))) {
          combat_event(m->pos, p.id, m->id, CombatKind::AbilityMiss, 0, a->name, sink);
          engage(*m, p, 1.0, sink);
          landed = false;
          break;
        }
        int dmg = ability_damage(e, p.level, rng_);
        dmg = static_cast<int>(std::lround(static_cast<float>(dmg) * (1.0f + p.buff_total(BuffStat::MeleeDamagePct) / 100.0f)));
        dmg = mitigate(dmg, m->stats.ac, p.level, 1.0f, cr);
        combat_event(m->pos, p.id, m->id, CombatKind::AbilityHit, static_cast<std::uint32_t>(dmg), a->name, sink);
        damage_mob(*m, p, dmg, 0.0, sink);
        break;
      }
      case EffectType::Stun: {
        if (!landed || !m || !m->alive()) break;
        if (stun_lands(p.level, m->def->level, m->def->stun_immune, cr)) {
          m->stunned_until = tick_ + ticks(e.duration);
          m->swing_ready_tick = std::max(m->swing_ready_tick, m->stunned_until);
          m->changed_tick = tick_;
          combat_event(m->pos, p.id, m->id, CombatKind::Stun, ms(ticks(e.duration), dt_), a->name, sink);
          engage(*m, p, 5.0, sink);
        } else {
          combat_event(m->pos, p.id, m->id, CombatKind::StunFail, 0, a->name, sink);
          engage(*m, p, 1.0, sink);
        }
        break;
      }
      case EffectType::Threat:
        if (landed && m && m->alive()) engage(*m, p, static_cast<double>(e.base + e.per_level * L), sink);
        break;
      case EffectType::Taunt: {
        if (!m || !m->alive() || m->state == AiState::Leash) break;
        const SkillCap* cap = p.class_def->skill("taunt");
        const int cap_v = cap ? skill_cap(*cap, p.level) : 0;
        engage(*m, p, 1.0, sink);
        if (rng_.chance(taunt_chance(p.skill("taunt"), cap_v, p.level, m->def->level, cr))) {
          m->threat.taunt(p.id);
          combat_event(m->pos, p.id, m->id, CombatKind::Taunt, 0, a->name, sink);
        } else {
          combat_event(m->pos, p.id, m->id, CombatKind::TauntFail, 0, a->name, sink);
        }
        break;
      }
      case EffectType::Buff: {
        const std::uint32_t expires = tick_ + ticks(e.duration);
        auto it = std::find_if(p.buffs.begin(), p.buffs.end(),
                               [&](const ActiveBuff& b) { return b.ability_id == a->id && b.stat == e.stat; });
        if (it != p.buffs.end()) {
          it->expires_tick = expires;  // refresh
          it->value = e.value;
        } else if (p.buffs.size() < kMaxBuffs) {
          p.buffs.push_back({a->id, a->name, e.stat, e.value, expires});
        }
        // One event per ability, not per stat line.
        if (&e == &*std::find_if(a->effects.begin(), a->effects.end(), [](const EffectDef& x) { return x.type == EffectType::Buff; }))
          combat_event(p.pos, p.id, p.id, CombatKind::Buff, ms(ticks(e.duration), dt_), a->name, sink);
        break;
      }
      case EffectType::HealPct: {
        const int amount = std::max(0, std::min(p.stats.max_hp - p.hp, static_cast<int>(std::lround(static_cast<float>(p.stats.max_hp) * e.value / 100.0f))));
        p.hp += amount;
        combat_event(p.pos, p.id, p.id, CombatKind::Heal, static_cast<std::uint32_t>(amount), a->name, sink);
        // Healing angers everything already fighting the healed player.
        std::size_t engaged = 0;
        for (const auto& [id, mob] : mobs_)
          if (mob.alive() && mob.threat.contains(p.id)) ++engaged;
        const double t = heal_threat(amount, engaged);
        if (t > 0.0)
          for (auto& [id, mob] : mobs_)
            if (mob.alive() && mob.threat.contains(p.id)) mob.threat.add(p.id, t);
        break;
      }
      case EffectType::RestoreEndurancePct: {
        const int amount = static_cast<int>(std::lround(static_cast<float>(p.stats.max_endurance) * e.value / 100.0f));
        p.endurance = std::min(p.stats.max_endurance, p.endurance + std::max(0, amount));
        break;
      }
      case EffectType::AreaThreat: {
        scratch_dead_.clear();
        grid_.query(p.pos.x, p.pos.z, e.radius, scratch_dead_);
        for (EntityId id : scratch_dead_) {
          MobEntity* o = find_mob(id);
          if (!o || !o->alive() || o->threat.empty() || o->state == AiState::Leash) continue;
          engage(*o, p, static_cast<double>(e.base + e.per_level * L), sink);
          combat_event(o->pos, p.id, o->id, CombatKind::Taunt, 0, a->name, sink);
        }
        mark_combat(p);
        break;
      }
    }
  }
  send(sink, p, proto::AbilityResult{true, a->id, "", ms(cd_ticks, dt_), ms(gcd_ticks, dt_)});
}

void Zone::respawn(EntityId player, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp || !pp->dead) return;
  PlayerEntity& p = *pp;
  p.dead = false;
  p.hp = p.stats.max_hp;
  p.mana = p.stats.max_mana;
  p.endurance = p.stats.max_endurance;
  p.hp_acc = p.mana_acc = p.end_acc = 0.0f;
  const Bounds b = bounds();
  if (p.bind_zone == def_.id) {
    p.pos = {std::clamp(p.bind_pos.x, b.min_x, b.max_x), def_.ground_y, std::clamp(p.bind_pos.z, b.min_z, b.max_z)};
    p.yaw = wrap_angle(p.bind_yaw);
  } else {
    // Cross-zone binds arrive with zone lines (Phase 6); until then use this zone's safe spot.
    p.pos = {def_.safe_spawn.x, def_.ground_y, def_.safe_spawn.z};
    p.yaw = def_.safe_spawn.yaw;
  }
  // Inputs predicted from the death spot are stale: drop them and hard-correct the client.
  p.queue_size = 0;
  p.has_pred = false;
  p.check_cooldown = kCorrectionCooldownTicks;
  p.changed_tick = tick_;
  p.stats_hash = 0;
  grid_.update(p.id, p.pos.x, p.pos.z);
  sink.zone_correction(p, "respawn");
  send(sink, p, proto::ChatMessage{kChatSystem, "", "You return to your bind point."});
}

// ---------------------------------------------------------------- the combat round

void Zone::player_combat(PlayerEntity& p, ZoneSink& sink) {
  if (p.dead || !p.auto_attack || p.stunned_until > tick_) return;
  MobEntity* m = find_mob(p.target);
  if (!m || !m->alive()) {
    p.auto_attack = false;
    return;
  }
  if (tick_ < p.swing_ready_tick) return;
  if (horizontal_distance(p.pos, m->pos) > reach(*m)) {
    if (tick_ - p.range_warn_tick >= ticks(3.0f) || p.range_warn_tick == 0) {
      combat_text(p, "Your target is too far away, get closer!", sink);
      p.range_warn_tick = tick_;
    }
    return;
  }
  stand(p);
  p.swing_ready_tick = tick_ + std::max<std::uint32_t>(1, ticks(p.stats.weapon_delay));
  player_swing(p, *m, sink, false);
  const int da = p.skill("double_attack");
  if (da > 0 && m->alive() && !p.dead) {
    try_skill_up(p, "double_attack", sink);
    if (rng_.chance(double_attack_chance(da, rules_.combat))) player_swing(p, *m, sink, false);
  }
}

void Zone::player_swing(PlayerEntity& p, MobEntity& m, ZoneSink& sink, bool counter) {
  if (!m.alive() || p.dead) return;
  mark_combat(p);
  if (m.state == AiState::Leash) {
    combat_event(m.pos, p.id, m.id, CombatKind::Evade, 0, "", sink);
    return;
  }
  const AttackerView a{p.level, p.skill("offense"), player_hit_range(p.stats, p.level),
                       p.stats.crit_chance + p.buff_total(BuffStat::CritChancePct) / 100.0f,
                       1.0f + p.buff_total(BuffStat::MeleeDamagePct) / 100.0f};
  const int avoid = m.stats.avoid_skill;
  // A counter-swing can be dodged or parried but not riposted again.
  const DefenderView d{m.def->level, m.stats.defense, m.stats.ac, avoid, avoid, counter ? 0 : avoid / 2, 1.0f};
  try_skill_up(p, "offense", sink);
  const SwingOutcome out = resolve_swing(a, d, rules_.combat, rng_);
  combat_event(m.pos, p.id, m.id, swing_kind(out.result), static_cast<std::uint32_t>(out.damage), "", sink);
  if (out.damage > 0) damage_mob(m, p, out.damage, 0.0, sink);
  else engage(m, p, 1.0, sink);
  if (out.result == SwingResult::Riposte && !counter && m.alive() && m.stunned_until <= tick_) mob_swing(m, p, sink, true);
}

void Zone::mob_swing(MobEntity& m, PlayerEntity& p, ZoneSink& sink, bool counter) {
  if (!m.alive() || p.dead) return;
  const AttackerView a{m.def->level, m.stats.offense, m.stats.hit, 0.0f, 1.0f};
  const DefenderView d{p.level, p.skill("defense"), p.stats.ac, p.skill("dodge"), p.skill("parry"), counter ? 0 : p.skill("riposte"),
                       std::max(0.0f, 1.0f + p.buff_total(BuffStat::DamageTakenPct) / 100.0f)};
  try_skill_up(p, "defense", sink);
  for (const char* s : {"dodge", "parry", "riposte"})
    if (p.skill(s) > 0) try_skill_up(p, s, sink);
  const SwingOutcome out = resolve_swing(a, d, rules_.combat, rng_);
  mark_combat(p);
  stand(p);
  combat_event(p.pos, m.id, p.id, swing_kind(out.result), static_cast<std::uint32_t>(out.damage), "", sink);
  if (out.damage > 0) damage_player(p, out.damage, m, sink);
  if (out.result == SwingResult::Riposte && !counter && !p.dead && p.stunned_until <= tick_) player_swing(p, m, sink, true);
}

void Zone::damage_mob(MobEntity& m, PlayerEntity& attacker, int damage, double bonus_threat, ZoneSink& sink) {
  if (!m.alive() || damage <= 0) return;
  // Loot and XP rights count only the HP actually removed, so an overkill finishing blow
  // cannot outweigh the player who did the real work.
  const int credited = std::min(damage, m.hp);
  m.hp -= damage;
  m.changed_tick = tick_;
  auto it = std::find_if(m.damage_by.begin(), m.damage_by.end(), [&](const auto& e) { return e.first == attacker.char_id; });
  if (it != m.damage_by.end()) it->second += credited;
  else m.damage_by.emplace_back(attacker.char_id, credited);
  engage(m, attacker, static_cast<double>(damage) + bonus_threat, sink);
  if (m.hp <= 0) kill_mob(m, &attacker, sink);
}

void Zone::damage_player(PlayerEntity& p, int damage, const MobEntity& source, ZoneSink& sink) {
  if (p.dead || damage <= 0) return;
  p.hp -= damage;
  p.changed_tick = tick_;
  if (p.hp <= 0) kill_player(p, source, sink);
}

void Zone::kill_mob(MobEntity& m, PlayerEntity* killer, ZoneSink& sink) {
  // Loot and XP go to whoever did the most damage and is still here (the group, in Phase 7).
  PlayerEntity* winner = nullptr;
  int best = 0;
  for (const auto& [cid, dmg] : m.damage_by) {
    PlayerEntity* p = find_by_char(cid);
    if (p && !p->dead && dmg > best) {
      best = dmg;
      winner = p;
    }
  }
  if (!winner && killer && !killer->dead) winner = killer;
  combat_event(m.pos, killer ? killer->id : 0, m.id, CombatKind::Death, 0, "", sink);
  m.hp = 0;
  m.threat.clear();
  m.damage_by.clear();
  m.stunned_until = 0;
  set_mob_state(m, AiState::Dead);
  grid_.remove(m.id);
  if (m.spawn_index < groups_.size()) --groups_[m.spawn_index].alive;
  for (auto& [id, p] : players_) {
    if (p.target != m.id) continue;
    p.target = 0;
    p.auto_attack = false;
    send_target_info(p, sink);
  }
  make_mob_corpse(m, winner ? winner->char_id : 0);
  if (winner) {
    const std::uint64_t base = kill_xp(winner->level, m.def->level, rules_.xp, rules_.con);
    const auto shares = split_group_xp(base, {winner->level}, rules_.xp);  // group hook: one member today
    if (shares.front() > 0) award_xp(*winner, shares.front(), sink);
    else combat_text(*winner, "You gain no experience from such an easy foe.", sink);
  }
}

void Zone::kill_player(PlayerEntity& p, const MobEntity& killer, ZoneSink& sink) {
  if (p.looting != 0) close_loot(p, sink, "");
  p.dead = true;
  p.hp = 0;
  p.auto_attack = false;
  p.sitting = false;
  p.stunned_until = 0;
  p.target = 0;
  p.buffs.clear();
  p.changed_tick = tick_;
  drop_from_threat(p.id);
  combat_event(p.pos, killer.id, p.id, CombatKind::Death, 0, "", sink);

  const std::uint64_t loss = death_xp_loss(p.xp, p.level, rules_.death, rules_.xp);
  p.xp -= loss;
  recompute_level(p, sink, true, false);  // zone_player_died below saves character and corpse together

  CorpseEntity c;
  c.id = new_id();
  c.db_id = static_cast<std::int64_t>((rng_.next() >> 2) | 1ULL);
  c.player_corpse = true;
  c.owner_char_id = p.char_id;
  c.loot_char_id = p.char_id;
  c.name = p.name + "'s corpse";
  c.owner_name = p.name;
  c.model = "humanoid";
  c.level = p.level;
  c.pos = p.pos;
  c.yaw = p.yaw;
  c.coin = p.coin;
  for (auto& s : p.inventory) {
    if (!s.empty()) c.items.push_back(s);
    s = {};
  }
  c.xp_lost = loss;
  c.expires_tick = tick_ + ticks(rules_.death.player_corpse_seconds);
  p.coin = 0;
  const EntityId cid = c.id;
  grid_.insert(cid, c.pos.x, c.pos.z);
  auto& corpse = corpses_.emplace(cid, std::move(c)).first->second;

  send(sink, p, proto::PlayerDied{killer.def->name, static_cast<std::uint32_t>(std::min<std::uint64_t>(loss, 0xFFFFFFFFu)), cid});
  send_inventory(p, sink);
  send_target_info(p, sink);
  sink.zone_player_died(p, corpse);
}

// ---------------------------------------------------------------- experience and levels

void Zone::grant_xp(EntityId player, std::uint64_t amount, ZoneSink& sink) {
  if (PlayerEntity* p = find(player)) award_xp(*p, amount, sink);
}

void Zone::award_xp(PlayerEntity& p, std::uint64_t amount, ZoneSink& sink) {
  const std::uint64_t cap = total_xp_for_level(rules_.max_level, rules_.xp);
  if (amount == 0 || p.xp >= cap) return;
  const std::uint64_t gained = std::min(amount, cap - p.xp);
  p.xp += gained;
  combat_text(p, "You gain experience! (" + std::to_string(gained) + " XP)", sink);
  recompute_level(p, sink, true);
}

void Zone::recompute_level(PlayerEntity& p, ZoneSink& sink, bool announce, bool save) {
  const int level = level_for_xp(p.xp, rules_.max_level, rules_.xp);
  if (level == p.level) return;
  const int old = p.level;
  p.level = level;
  p.stats = derive_stats(*p.race_def, *p.class_def, p.level);
  p.hp = std::min(p.hp, p.stats.max_hp);
  p.mana = std::min(p.mana, p.stats.max_mana);
  p.endurance = std::min(p.endurance, p.stats.max_endurance);
  unlock_skills(p);
  p.changed_tick = tick_;
  if (announce) {
    if (level > old) {
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You have gained a level! Welcome to level " + std::to_string(level) + "!"});
      for (const AbilityDef* a : content_.abilities_for(p.class_id))
        if (a->level > old && a->level <= level)
          send(sink, p, proto::ChatMessage{kChatSystem, "", "You have learned " + a->name + (a->passive() ? " (passive)." : "!")});
    } else {
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You have lost a level. You are now level " + std::to_string(level) + "."});
    }
  }
  send_skills(p, sink);
  respawn_spot_watchers(p.id, sink);
  if (save) sink.zone_character_changed(p);
}

// ---------------------------------------------------------------- upkeep

void Zone::regen(PlayerEntity& p) {
  if (p.dead) return;
  const bool ic = in_combat(p);
  const auto& r = rules_.combat.regen;
  auto step = [&](int& value, int max, float& acc, float per_second) {
    if (value >= max) {
      acc = 0.0f;
      return;
    }
    acc += per_second * dt_;
    if (acc >= 1.0f) {
      const int whole = static_cast<int>(acc);
      value = std::min(max, value + whole);
      acc -= static_cast<float>(whole);
      p.changed_tick = tick_;
    }
  };
  step(p.hp, p.stats.max_hp, p.hp_acc, hp_regen(p.level, ic, p.sitting, r));
  step(p.endurance, p.stats.max_endurance, p.end_acc, endurance_regen(p.level, ic, p.sitting, r));
  if (p.stats.max_mana > 0) step(p.mana, p.stats.max_mana, p.mana_acc, mana_regen(p.level, ic, p.sitting, r));
}

void Zone::expire_buffs(PlayerEntity& p, ZoneSink& sink) {
  for (auto it = p.buffs.begin(); it != p.buffs.end();) {
    if (it->expires_tick > tick_) {
      ++it;
      continue;
    }
    const std::string name = it->name;
    const std::string ability = it->ability_id;
    it = p.buffs.erase(it);
    // Announce once per ability (a buff may carry several stat lines).
    const bool more = std::any_of(p.buffs.begin(), p.buffs.end(), [&](const ActiveBuff& b) { return b.ability_id == ability; });
    if (!more) send(sink, p, proto::CombatEvent{p.id, p.id, static_cast<std::uint8_t>(CombatKind::BuffFade), 0, name});
  }
}

}  // namespace ef::game
