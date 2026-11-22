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

// Melee reach grows with the mob's body (its model's collision radius, scaled with the mob).
float Zone::reach(const MobEntity& m) const { return rules_.combat.melee_range + m.def->collision_radius * m.def->scale; }

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
                        std::string_view ability, ZoneSink& sink, ResistType school) {
  broadcast(where, proto::CombatEvent{source, target, static_cast<std::uint8_t>(kind), amount, std::string(ability), static_cast<std::uint8_t>(school)}, sink);
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
  for (auto& [id, m] : mobs_) {
    m.threat.remove(player);
    if (m.is_pet() && m.pet_target == player) m.pet_target = 0;
  }
}

void Zone::call_assist(MobEntity& m, EntityId target) {
  scratch_near_.clear();
  grid_.query(m.pos.x, m.pos.z, 100.0f, scratch_near_);
  for (EntityId id : scratch_near_) {
    if (id == m.id) continue;
    MobEntity* o = find_mob(id);
    if (!o || !o->alive() || o->is_pet() || !o->def->assist || o->def->faction != m.def->faction) continue;
    if (o->state != AiState::Idle && o->state != AiState::Wander) continue;
    if (horizontal_distance(o->pos, m.pos) > o->def->assist_radius) continue;
    o->threat.add(target, 1.0);
    set_mob_state(*o, AiState::Chase);
    o->last_melee_tick = tick_;
  }
}

double Zone::scaled_threat(const PlayerEntity& p, double threat) const {
  return threat * threat_scale(p.class_def ? p.class_def->combat.threat_mult : 1.0f, p.buff_total(BuffStat::ThreatPct));
}

void Zone::engage(MobEntity& m, PlayerEntity& p, double threat, ZoneSink& /*sink*/) {
  if (!m.alive() || m.state == AiState::Leash || m.is_pet()) return;
  mark_combat(p);
  engage_id(m, p.id, scaled_threat(p, threat));
}

void Zone::engage_id(MobEntity& m, EntityId id, double threat) {
  if (!m.alive() || m.state == AiState::Leash || m.is_pet()) return;
  const bool was_calm = m.state == AiState::Idle || m.state == AiState::Wander;
  m.threat.add(id, std::max(threat, 0.01));
  m.forgot = false;
  if (was_calm) {
    set_mob_state(m, AiState::Chase);
    m.last_melee_tick = tick_;
    call_assist(m, id);
  }
}

// A damage shield burns a melee attacker that landed a hit. The defender (a player, or a pet on its
// owner's behalf) gets the credit and the threat.
void Zone::reflect_damage_shield(MobEntity& attacker, EntityId defender, int shield, ZoneSink& sink) {
  if (shield <= 0 || !attacker.alive()) return;
  if (PlayerEntity* p = find(defender)) {
    combat_event(attacker.pos, p->id, attacker.id, CombatKind::DamageShield, static_cast<std::uint32_t>(shield), "", sink);
    damage_mob(attacker, *p, shield, 0.0, sink, true);
    return;
  }
  MobEntity* pet = find_mob(defender);
  if (!pet || !pet->is_pet()) return;
  PlayerEntity* owner = find(pet->owner);
  if (!owner) return;
  combat_event(attacker.pos, pet->id, attacker.id, CombatKind::DamageShield, static_cast<std::uint32_t>(shield), "", sink);
  damage_mob(attacker, *owner, shield, 0.0, sink, true, pet);
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
  p.inv_dirty = false;
  proto::Inventory inv;
  inv.coin = p.coin;
  inv.bank_coin = p.bank_coin;
  inv.bank_slots = static_cast<std::uint8_t>(std::clamp(rules_.economy.bank_slots, 1, 16));
  p.inv.for_each([&](SlotCode code, const ItemStackState& s) {
    if (inv.items.size() >= 600) return;
    const std::uint32_t left = s.expires_tick > tick_ ? s.expires_tick - tick_ : 0;
    inv.items.push_back({code, s.item_id, static_cast<std::uint16_t>(std::clamp(s.count, 0, 65535)),
                         s.expires_tick == 0 ? 0u : std::max<std::uint32_t>(1, ms(left, dt_))});
  });
  send(sink, p, inv);
}

void Zone::send_target_info(PlayerEntity& p, ZoneSink& sink) {
  proto::TargetInfo t;
  t.entity_id = p.target;
  t.regard = kNoRegard;
  if (p.target == p.id) {
    t.con = static_cast<std::uint8_t>(Con::White);
    t.level = static_cast<std::uint8_t>(p.level);
    t.hp_pct = pct(p.hp, p.stats.max_hp);
    t.name = p.name;
  } else if (const MobEntity* m = find_mob(p.target); m && m->alive()) {
    t.con = static_cast<std::uint8_t>(m->is_pet() ? Con::White : con_color(p.level, m->def->level, rules_.con));
    t.level = static_cast<std::uint8_t>(m->def->level);
    t.hp_pct = pct(m->hp, m->stats.max_hp);
    t.name = m->def->name;
    if (const FactionDef* fd = m->is_pet() ? nullptr : content_.find_faction(m->def->faction)) {
      t.regard = static_cast<std::uint8_t>(regard(p, fd->id));
      t.faction = fd->name.substr(0, 48);
    }
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
  } else if (const NpcEntity* n = find_npc(p.target)) {
    t.con = static_cast<std::uint8_t>(Con::White);
    t.level = static_cast<std::uint8_t>(std::clamp(n->def->level, 1, 255));
    t.hp_pct = 100;
    t.name = n->def->name;
    if (const FactionDef* fd = content_.find_faction(n->def->faction)) {
      t.regard = static_cast<std::uint8_t>(regard(p, fd->id));
      t.faction = fd->name.substr(0, 48);
    }
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
  if (p->target != 0 && (find(p->target) || find_mob(p->target))) send_buff_list(*p, p->target, sink);
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
  const MobEntity* m = hostile_mob(p->target);
  if (!m) {
    combat_text(*p, p->target == 0 ? "You need a target to attack." : "You can't attack that.", sink);
    p->auto_attack = false;
    return;
  }
  stand(*p);
  if (p->feigned) stand_from_feign(*p, sink);
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
  if (!can_act(p, *a, sink)) return;
  EntityId tid = 0;
  std::string why;
  if (!resolve_target(p, *a, target, tid, why, false)) return ability_fail(p, *a, why, sink);
  if (a->requires_weapon == "archery") {
    const ItemDef* bow = worn(p, EquipSlot::Ranged);
    if (!bow || bow->weapon != "archery") return ability_fail(p, *a, "You need a bow in your ranged slot to use " + a->name + ".", sink);
  }
  if (a->position == "behind") {
    const MobEntity* m = find_mob(tid);
    if (m && !is_behind(m->pos, m->yaw, p.pos, rules_.combat.behind_arc_degrees))
      return ability_fail(p, *a, "You must be behind your target to use " + a->name + ".", sink);
  }
  // Any action but feigning stands a feigning player up; acting against an enemy ends hiding.
  if (p.feigned && !a->has(EffectType::FeignDeath)) stand_from_feign(p, sink);
  if (!a->has(EffectType::Hide) && !a->has(EffectType::Sneak)) break_stealth(p, !a->beneficial(), sink);
  if (a->cast_time > 0.0f) {  // abilities with a cast time go through the cast bar (no gem)
    const std::uint32_t cast_ticks = ticks(a->cast_time);
    p.cast = begin_cast(*a, tid, -1, tick_, cast_ticks, p.pos);
    stand(p);
    broadcast(p.pos, proto::CastStart{p.id, tid, a->id, a->name, ms(cast_ticks, dt_), static_cast<std::uint8_t>(a->resist)}, sink);
    return;
  }
  pay_and_lock(p, *a, sink);
  if (!a->beneficial()) mark_combat(p);
  resolve_ability(p, *a, tid, sink);
  const std::uint32_t gcd_ticks = a->gcd ? ticks(rules_.combat.gcd_seconds) : 0;
  send(sink, p, proto::AbilityResult{true, a->id, "", ms(ticks(a->cooldown), dt_), ms(gcd_ticks, dt_)});
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
  Destination dest;
  if (!resolve_destination(p, "bind", dest)) dest = {def_.id, {def_.safe_spawn.x, def_.ground_at(def_.safe_spawn.x, def_.safe_spawn.z), def_.safe_spawn.z}, def_.safe_spawn.yaw};
  send(sink, p, proto::ChatMessage{kChatSystem, "", "You return to your bind point."});
  move_player_to(p, dest, sink, "respawn");
}

// ---------------------------------------------------------------- the combat round

void Zone::player_combat(PlayerEntity& p, ZoneSink& sink) {
  if (p.dead || !p.auto_attack || p.stunned_until > tick_) return;
  if (p.cast.casting() || (control_bits(p.buffs) & (kControlMez | kControlFear)) != 0) return;  // no swings while casting or held
  MobEntity* m = hostile_mob(p.target);
  if (!m) {
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
  if (p.feigned) stand_from_feign(p, sink);
  break_stealth(p, true, sink);
  const float buff_speed = p.buff_total(BuffStat::AttackSpeedPct);
  p.swing_ready_tick = tick_ + std::max<std::uint32_t>(1, ticks(swing_delay(p.stats.weapon_delay, p.stats.haste, buff_speed)));
  const EntityId mid = m->id;
  const ItemDef* main_weapon = worn(p, EquipSlot::Primary);
  player_swing(p, *m, sink, false, 1.0f, main_weapon);
  const int da = p.skill("double_attack");
  if (da > 0 && hostile_mob(mid) && !p.dead) {
    try_skill_up(p, "double_attack", sink);
    if (rng_.chance(double_attack_chance(da, rules_.combat))) player_swing(p, *m, sink, false, 1.0f, main_weapon);
  }
  // Dual wield: the off-hand weapon (or a second fist) swings too, for its share of a main-hand hit.
  const int dw = p.skill("dual_wield");
  if (dw > 0 && p.stats.offhand_damage > 0.0f && hostile_mob(mid) && !p.dead) {
    try_skill_up(p, "dual_wield", sink);
    if (rng_.chance(dual_wield_chance(dw, rules_.combat))) {
      const float share = rules_.combat.offhand_damage_mult * p.stats.offhand_damage / std::max(1.0f, p.stats.weapon_damage);
      player_swing(p, *m, sink, false, share, worn(p, EquipSlot::Secondary));
    }
  }
}

void Zone::player_swing(PlayerEntity& p, MobEntity& m, ZoneSink& sink, bool counter, float weapon_mult, const ItemDef* weapon) {
  if (!m.alive() || p.dead || m.is_pet()) return;
  mark_combat(p);
  if (m.state == AiState::Leash) {
    combat_event(m.pos, p.id, m.id, CombatKind::Evade, 0, "", sink);
    return;
  }
  const int ed = p.skill("enhanced_damage");
  if (ed > 0) try_skill_up(p, "enhanced_damage", sink);
  const AttackerView a{p.level, p.skill("offense"), player_hit_range(p.stats, p.level),
                       p.stats.crit_chance + p.buff_total(BuffStat::CritChancePct) / 100.0f,
                       std::max(0.05f, (1.0f + p.buff_total(BuffStat::MeleeDamagePct) / 100.0f) * enhanced_damage_mult(ed, rules_.combat) * weapon_mult)};
  const int avoid = m.stats.avoid_skill;
  // A counter-swing can be dodged or parried but not riposted again.
  const DefenderView d{m.def->level, m.stats.defense, mob_ac(m), avoid, avoid, counter ? 0 : avoid / 2, 1.0f};
  try_skill_up(p, "offense", sink);
  const SwingOutcome out = resolve_swing(a, d, rules_.combat, rng_);
  combat_event(m.pos, p.id, m.id, swing_kind(out.result), static_cast<std::uint32_t>(out.damage), "", sink);
  if (out.damage > 0) {
    const EntityId mid = m.id;
    damage_mob(m, p, out.damage, 0.0, sink);
    if (MobEntity* still = find_mob(mid); still && still->alive() && find(p.id)) try_proc(p, weapon, *still, sink);
    if (!find_mob(mid) || !m.alive()) return;
  } else {
    engage(m, p, 1.0, sink);
  }
  if (out.result == SwingResult::Riposte && !counter && m.alive() && m.stunned_until <= tick_) mob_swing(m, p, sink, true);
}

void Zone::mob_swing(MobEntity& m, PlayerEntity& p, ZoneSink& sink, bool counter) {
  if (!m.alive() || p.dead) return;
  // Weakening debuffs (melee_damage_pct below 0) shrink the mob's hits.
  const AttackerView a{m.def->level, m.stats.offense, m.stats.hit, 0.0f,
                       std::max(0.1f, 1.0f + stat_total(m.buffs, BuffStat::MeleeDamagePct) / 100.0f) * (1.0f + m.enrage_pct / 100.0f)};
  const DefenderView d{p.level, p.skill("defense"), p.stats.ac, p.skill("dodge"), p.skill("parry"), counter ? 0 : p.skill("riposte"),
                       std::max(0.0f, 1.0f + p.buff_total(BuffStat::DamageTakenPct) / 100.0f)};
  try_skill_up(p, "defense", sink);
  for (const char* s : {"dodge", "parry", "riposte"})
    if (p.skill(s) > 0) try_skill_up(p, s, sink);
  const SwingOutcome out = resolve_swing(a, d, rules_.combat, rng_);
  mark_combat(p);
  stand(p);
  combat_event(p.pos, m.id, p.id, swing_kind(out.result), static_cast<std::uint32_t>(out.damage), "", sink);
  const EntityId mid = m.id;
  if (out.damage > 0) {
    const int shield = static_cast<int>(std::lround(p.buff_total(BuffStat::DamageShield)));
    damage_player(p, out.damage, m.id, m.def->name, sink);
    MobEntity* still = find_mob(mid);
    if (shield > 0 && still && still->alive() && find(p.id)) reflect_damage_shield(*still, p.id, shield, sink);
    if (!still || !still->alive()) return;
    if (!counter && !still->def->specials.empty() && find(p.id) && !p.dead) {
      mob_on_hit(*still, p, sink);
      if (!find_mob(mid) || !find_mob(mid)->alive()) return;
    }
  }
  // The counter needs a player free to swing: not dead, stunned, casting, mesmerized, or afraid.
  if (out.result == SwingResult::Riposte && !counter && !p.dead && p.stunned_until <= tick_ && !p.cast.casting() &&
      (control_bits(p.buffs) & (kControlMez | kControlFear)) == 0)
    player_swing(p, m, sink, true);
}

void Zone::damage_mob(MobEntity& m, PlayerEntity& attacker, int damage, double bonus_threat, ZoneSink& sink, bool direct,
                      MobEntity* via_pet) {
  if (!m.alive() || damage <= 0) return;
  if (m.is_pet()) return;  // players and their pets never hurt pets
  on_mob_damaged(m, direct, sink);  // damage breaks mez (and sometimes root)
  // Loot and XP rights count only the HP actually removed, so an overkill finishing blow
  // cannot outweigh the player who did the real work.
  const int credited = std::min(damage, m.hp);
  m.hp -= damage;
  m.changed_tick = tick_;
  auto it = std::find_if(m.damage_by.begin(), m.damage_by.end(), [&](const auto& e) { return e.first == attacker.char_id; });
  if (it != m.damage_by.end()) it->second += credited;
  else m.damage_by.emplace_back(attacker.char_id, credited);
  if (via_pet) {
    mark_combat(attacker);
    const float pet_threat = via_pet->pet_def ? via_pet->pet_def->threat_mult : 1.0f;
    engage_id(m, via_pet->id, (static_cast<double>(damage) + bonus_threat) * static_cast<double>(pet_threat));
    if (via_pet->pet_target == 0) via_pet->pet_target = m.id;
  } else {
    engage(m, attacker, static_cast<double>(damage) + bonus_threat, sink);
  }
  if (m.hp <= 0) kill_mob(m, &attacker, sink);
}

void Zone::damage_player(PlayerEntity& p, int damage, EntityId source, const std::string& source_name, ZoneSink& sink) {
  if (p.dead || damage <= 0) return;
  p.hp -= damage;
  p.changed_tick = tick_;
  // Damage breaks mez on players too (only the mez line; the rest of that spell stays).
  bool woke = false;
  for (auto& b : p.buffs) woke |= strip_control(b, kControlMez);
  if (woke) {
    std::erase_if(p.buffs, [](const ActiveBuff& b) { return !has_lasting_part(b); });
    p.buffs_dirty = true;
    refresh_stats(p);
  }
  if (p.hp <= 0) {
    kill_player(p, source, source_name, sink);
    return;
  }
  // A hit while casting needs a concentration check (channeling improves it).
  if (p.cast.casting()) {
    try_skill_up(p, "channeling", sink);
    if (rng_.chance(concentration_chance(p.skill("channeling"), damage, p.stats.max_hp, rules_.spells)))
      combat_text(p, "You keep your concentration.", sink);
    else
      interrupt_cast(p, CastResult::Interrupted, "Your spell is interrupted.", sink);
  }
}

void Zone::kill_mob(MobEntity& m, PlayerEntity* killer, ZoneSink& sink) {
  if (m.is_pet() && !m.charmed) return kill_pet(m, sink);
  if (m.is_pet()) {
    // A charmed mob that dies stays dead like any other (its charmer gets no experience for it).
    if (PlayerEntity* owner = find(m.owner); owner && owner->pet == m.id) {
      owner->pet = 0;
      send_pet_status(*owner, sink, true);
    }
    m.owner = 0;
    m.owner_char = 0;
    m.charmed = false;
    m.pet_target = 0;
    killer = nullptr;
    m.damage_by.clear();
  }
  for (auto& [oid, o] : mobs_) {
    o.threat.remove(m.id);
    if (o.is_pet() && o.pet_target == m.id) o.pet_target = 0;
  }
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
  std::vector<std::int64_t> party = loot_party(m, winner ? winner->char_id : 0);
  m.damage_by.clear();
  m.stunned_until = 0;
  m.buffs.clear();
  m.buffs_dirty = false;
  set_mob_state(m, AiState::Dead);
  grid_.remove(m.id);
  if (m.spawn_index < groups_.size()) --groups_[m.spawn_index].alive;
  for (auto& [id, p] : players_) {
    if (p.target != m.id) continue;
    p.target = 0;
    p.auto_attack = false;
    send_target_info(p, sink);
  }
  // Faction consequences for everyone who shares the kill.
  if (const FactionDef* fd = content_.find_faction(m.def->faction); fd && !fd->hits.empty())
    for (std::int64_t cid : party)
      if (PlayerEntity* pp = find_by_char(cid)) faction_hits(*pp, *fd, sink);
  // Quest credit (and quest item drops) for everyone who shares the kill.
  for (std::int64_t cid : party)
    if (PlayerEntity* pp = find_by_char(cid)) quest_on_kill(*pp, *m.def, sink);
  CorpseEntity& corpse = make_mob_corpse(m, winner ? winner->char_id : 0);
  corpse.party = std::move(party);
  start_rolls(corpse, sink);
  if (winner) {
    const std::uint64_t base = kill_xp(winner->level, m.def->level, rules_.xp, rules_.con);
    const auto shares = split_group_xp(base, {winner->level}, rules_.xp);  // group hook: one member today
    if (shares.front() > 0) award_xp(*winner, shares.front(), sink);
    else combat_text(*winner, "You gain no experience from such an easy foe.", sink);
  }
}

void Zone::kill_player(PlayerEntity& p, EntityId killer, const std::string& killer_name, ZoneSink& sink) {
  if (p.looting != 0) close_loot(p, sink, "");
  trade_cancel(p.id, "The trade ends: " + p.name + " died.", sink);
  interrupt_cast(p, CastResult::Interrupted, "", sink);
  stop_song(p, "", sink);
  p.feigned = p.hidden = p.sneaking = false;
  if (p.pet != 0)
    if (MobEntity* pet = find_mob(p.pet)) release_pet(*pet, false, "Your pet fades away as you die.", sink);
  p.pet = 0;
  p.dead = true;
  p.hp = 0;
  p.auto_attack = false;
  p.sitting = false;
  p.stunned_until = 0;
  p.target = 0;
  p.buffs.clear();  // death strips every effect
  p.buffs_dirty = true;
  refresh_stats(p);
  p.changed_tick = tick_;
  drop_from_threat(p.id);
  combat_event(p.pos, killer, p.id, CombatKind::Death, 0, "", sink);

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
  // A fresh player corpse looks like its owner (race model + class appearance). Corpses restored from
  // the database after a restart keep the generic humanoid model (the corpses table has no appearance).
  c.model = p.race;
  c.class_id = p.class_id;
  c.gender = p.gender;
  c.level = p.level;
  c.pos = p.pos;
  c.yaw = p.yaw;
  c.coin = p.coin;
  bool dissolved = false;
  // Worn gear, the pack and its bags go onto the corpse (each remembering its slot); the bank stays.
  p.inv.for_each_mut([&](SlotCode code, ItemStackState& s) {
    if (in_bank(code)) return;
    // Summoned items do not outlive their owner's death. Only stacks positively known to be summoned
    // dissolve; anything else (even an item whose definition has since left the content) stays on the corpse.
    const ItemDef* def = content_.find_item(s.item_id);
    if (def && def->summoned) dissolved = true;
    else c.items.push_back({s, code, 0, 0});
    s = {};
  });
  if (dissolved) combat_text(p, "Your summoned items dissolve.", sink);
  c.xp_lost = loss;
  c.expires_tick = tick_ + ticks(rules_.death.player_corpse_seconds);
  p.coin = 0;
  const EntityId cid = c.id;
  grid_.insert(cid, c.pos.x, c.pos.z);
  auto& corpse = corpses_.emplace(cid, std::move(c)).first->second;

  send(sink, p, proto::PlayerDied{killer_name, static_cast<std::uint32_t>(std::min<std::uint64_t>(loss, 0xFFFFFFFFu)), cid});
  gear_changed(p, sink);
  p.save_dirty = false;  // the corpse write saves the stripped character
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
  p.base_stats = derive_stats(*p.race_def, *p.class_def, p.level);
  refresh_stats(p);
  p.endurance = std::min(p.endurance, p.stats.max_endurance);
  unlock_skills(p);
  // Losing a level can lock gems; their spells stay in the book.
  for (std::size_t g = static_cast<std::size_t>(std::max(0, gem_count(p.level, rules_.spells))); g < kMaxGems; ++g) p.gems[g].clear();
  p.changed_tick = tick_;
  if (announce) {
    if (level > old) {
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You have gained a level! Welcome to level " + std::to_string(level) + "!"});
      for (const AbilityDef* a : content_.abilities_for(p.class_id))
        if (a->level > old && a->level <= level)
          send(sink, p, proto::ChatMessage{kChatSystem, "", "You have learned " + a->name + (a->passive() ? " (passive)." : "!")});
      if (gem_count(level, rules_.spells) > gem_count(old, rules_.spells) && !content_.spells_for(p.class_id).empty())
        send(sink, p, proto::ChatMessage{kChatSystem, "", "You can now memorize " + std::to_string(gem_count(level, rules_.spells)) + " spells."});
    } else {
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You have lost a level. You are now level " + std::to_string(level) + "."});
    }
  }
  send_skills(p, sink);
  send_gems(p, sink);
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
  step(p.hp, p.stats.max_hp, p.hp_acc, std::max(0.0f, hp_regen(p.level, ic, p.sitting, r) + p.buff_total(BuffStat::HpRegen)));
  step(p.endurance, p.stats.max_endurance, p.end_acc, endurance_regen(p.level, ic, p.sitting, r));
  if (p.stats.max_mana > 0) {
    // Meditating (sitting with the meditate skill) speeds mana regeneration, and practice improves it.
    float mana = mana_regen(p.level, ic, p.sitting, r);
    if (p.sitting) {
      const SkillCap* cap = p.class_def->skill("meditate");
      mana *= meditate_multiplier(p.skill("meditate"), cap ? skill_cap(*cap, p.level) : 0, rules_.spells);
    }
    step(p.mana, p.stats.max_mana, p.mana_acc, std::max(0.0f, mana + p.buff_total(BuffStat::ManaRegen)));
  }
}

}  // namespace ef::game
