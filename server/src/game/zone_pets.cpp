// Zone pets: summoning, charm and its breaks, the pet AI (follow, guard, attack, defend), pet
// commands, pet melee, damage to pets, buffs and heals on pets, and the owner's pet window.
// The math lives in class_rules.cpp and rules.cpp; this file applies it to entities.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

std::uint8_t pct(int hp, int max_hp) {
  if (max_hp <= 0 || hp <= 0) return 0;
  return static_cast<std::uint8_t>(std::clamp((hp * 100 + max_hp - 1) / max_hp, 1, 100));
}

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

std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001B3ULL; }

}  // namespace

MobEntity* Zone::hostile_mob(EntityId id) {
  MobEntity* m = id != 0 ? find_mob(id) : nullptr;
  return m && m->alive() && !m->is_pet() ? m : nullptr;
}

// ---------------------------------------------------------------- summoning and charm

void Zone::summon_pet(PlayerEntity& p, const AbilityDef& a, const EffectDef& e, ZoneSink& sink) {
  if (p.pet != 0 && find_mob(p.pet)) {
    combat_text(p, "You already have a pet.", sink);
    return;
  }
  const PetDef* def = content_.find_pet(e.ref);
  if (!def) return;
  const int level = std::clamp(effect_amount(e, p.level), 1, rules_.max_level);
  MobEntity* m = spawn_pet_for(p, *def, level);
  if (!m) return;
  const EntityId id = m->id;
  combat_event(m->pos, p.id, id, CombatKind::Summon, static_cast<std::uint32_t>(level), a.name, sink, a.resist);
  combat_text(p, "Your " + def->name + " answers your call.", sink);
  send_pet_status(p, sink, true);
}

bool Zone::make_charmed(PlayerEntity& p, MobEntity& m, ZoneSink& sink) {
  if ((p.pet != 0 && find_mob(p.pet)) || m.is_pet() || !m.alive()) {
    for (auto& b : m.buffs) strip_control(b, kControlCharm);
    std::erase_if(m.buffs, [](const ActiveBuff& b) { return !has_lasting_part(b); });
    m.buffs_dirty = true;
    return false;
  }
  // The mob forgets its fight: nobody is on its hate list, and nobody is fighting it.
  m.threat.clear();
  m.damage_by.clear();
  for (auto& [id, o] : mobs_) {
    o.threat.remove(m.id);
    if (o.is_pet() && o.pet_target == m.id) o.pet_target = 0;
  }
  m.owner = p.id;
  m.owner_char = p.char_id;
  m.charmed = true;
  m.pet_def = nullptr;
  m.pet_mode = PetMode::Follow;
  m.pet_target = 0;
  m.guard_pos = m.pos;
  m.passive_until = 0;
  m.fled = false;
  m.forgot = false;
  m.next_charm_check = tick_ + std::max<std::uint32_t>(1, ticks(rules_.pets.charm_check_seconds));
  set_mob_state(m, AiState::Idle);
  p.pet = m.id;
  respawn_spot_watchers(m.id, sink);  // watchers learn its new owner
  send(sink, p, proto::ChatMessage{kChatSystem, "", m.def->name + " is now under your control."});
  send_pet_status(p, sink, true);
  return true;
}

void Zone::break_charm(MobEntity& m, bool hostile, ZoneSink& sink) {
  if (!m.charmed) return;
  PlayerEntity* owner = find(m.owner);
  for (auto& b : m.buffs) strip_control(b, kControlCharm);
  std::erase_if(m.buffs, [](const ActiveBuff& b) { return !has_lasting_part(b); });
  m.buffs_dirty = true;
  m.owner = 0;
  m.owner_char = 0;
  m.charmed = false;
  m.pet_target = 0;
  m.passive_until = 0;
  m.threat.clear();
  for (auto& [id, o] : mobs_) o.threat.remove(m.id);
  if (owner) {
    if (owner->pet == m.id) owner->pet = 0;
    combat_event(m.pos, owner->id, m.id, CombatKind::CharmBreak, 0, "", sink);
    send(sink, *owner, proto::ChatMessage{kChatSystem, "", "Your charm over " + m.def->name + " has broken!"});
    send_pet_status(*owner, sink, true);
  }
  if (hostile && owner && !owner->dead) {
    engage(m, *owner, 10.0, sink);  // it remembers who enslaved it
  } else {
    set_mob_state(m, AiState::Leash);
  }
  respawn_spot_watchers(m.id, sink);
}

void Zone::remove_pet_entity(MobEntity& pet, ZoneSink& sink) {
  for (auto& [id, o] : mobs_) {
    o.threat.remove(pet.id);
    if (o.is_pet() && o.pet_target == pet.id) o.pet_target = 0;
  }
  for (auto& [id, pl] : players_)
    if (pl.target == pet.id) {
      pl.target = 0;
      send_target_info(pl, sink);
    }
  pet.owner = 0;
  pet.owner_char = 0;
  pet.pet_target = 0;
  pet.threat.clear();
  pet.buffs.clear();
  pet.hp = 0;
  set_mob_state(pet, AiState::Dead);  // hand-placed: process_respawns erases it, clients see it despawn
  grid_.remove(pet.id);
}

void Zone::release_pet(MobEntity& m, bool hostile, const char* message, ZoneSink& sink) {
  PlayerEntity* owner = find(m.owner);
  if (owner && message) combat_text(*owner, message, sink);
  if (m.charmed) {
    break_charm(m, hostile, sink);
    return;
  }
  if (owner && owner->pet == m.id) {
    owner->pet = 0;
    send_pet_status(*owner, sink, true);
  }
  remove_pet_entity(m, sink);
}

void Zone::kill_pet(MobEntity& pet, ZoneSink& sink) {
  PlayerEntity* owner = find(pet.owner);
  combat_event(pet.pos, 0, pet.id, CombatKind::PetDeath, 0, "", sink);
  if (owner) {
    if (owner->pet == pet.id) owner->pet = 0;
    combat_text(*owner, "Your " + (pet.pet_def ? pet.pet_def->name : std::string("pet")) + " has been destroyed.", sink);
    send_pet_status(*owner, sink, true);
  }
  remove_pet_entity(pet, sink);
}

// ---------------------------------------------------------------- the pet AI

void Zone::pet_think(MobEntity& m, ZoneSink& sink) {
  PlayerEntity* owner = find(m.owner);
  if (!owner || owner->dead || owner->pet != m.id) {
    release_pet(m, false, nullptr, sink);
    return;
  }
  const std::uint8_t ctl = control_bits(m.buffs);
  if (m.charmed) {
    if ((ctl & kControlCharm) == 0) {  // the charm ran its course
      break_charm(m, true, sink);
      return;
    }
    if (tick_ >= m.next_charm_check) {
      m.next_charm_check = tick_ + std::max<std::uint32_t>(1, ticks(rules_.pets.charm_check_seconds));
      if (rng_.chance(charm_break_chance(owner->level, m.def->level, owner->stats.stats.cha, rules_.pets))) {
        break_charm(m, true, sink);
        return;
      }
    }
  }
  if (m.stunned_until > tick_ || (ctl & kControlMez) != 0) return;
  if ((ctl & kControlFear) != 0) {
    // A pet in fear (say, a charmed mob that was feared first) runs from the fear's caster, or its
    // own owner, and obeys nothing until the fear ends.
    Vec3 from = owner->pos;
    for (const auto& b : m.buffs)
      if ((b.control() & kControlFear) != 0)
        if (const PlayerEntity* c = find(b.caster)) from = c->pos;
    const float dx = m.pos.x - from.x;
    const float dz = m.pos.z - from.z;
    const float d = std::max(0.001f, std::sqrt(dx * dx + dz * dz));
    mob_move_toward(m, {m.pos.x + dx / d * 5.0f, m.pos.y, m.pos.z + dz / d * 5.0f}, m.def->run_speed * 0.6f, 0.0f);
    return;
  }
  const auto& pr = rules_.pets;
  const float owner_d = horizontal_distance(m.pos, owner->pos);
  if (owner_d > pr.leash_distance) {
    // Too far from its owner: drop the fight and come back, guard spot or not.
    m.pet_target = 0;
    m.pet_mode = PetMode::Follow;
  }
  // A mob walking home (leashing) is no longer a fight: forget it.
  auto fightable = [&](EntityId id) -> MobEntity* {
    MobEntity* x = hostile_mob(id);
    return x && x->state != AiState::Leash ? x : nullptr;
  };
  MobEntity* t = fightable(m.pet_target);
  if (!t) m.pet_target = 0;
  if (!t && tick_ >= m.passive_until && owner_d <= pr.leash_distance) {
    // Defend: whatever is hitting the pet, else the nearest mob fighting its owner.
    for (;;) {
      const EntityId top = m.threat.top();
      if (top == 0) break;
      t = fightable(top);
      if (t) break;
      m.threat.remove(top);
    }
    if (!t) {
      float best = pr.assist_radius;
      for (auto& [id, o] : mobs_) {
        if (!o.alive() || o.is_pet() || o.state == AiState::Leash) continue;
        if (!o.threat.contains(owner->id) && !o.threat.contains(m.id)) continue;
        const float d = horizontal_distance(o.pos, owner->pos);
        if (d <= best) {
          best = d;
          t = &o;
        }
      }
    }
    if (t) m.pet_target = t->id;
  }
  if (t) {
    const float r = std::max(reach(m), reach(*t));
    mob_move_toward(m, t->pos, m.def->run_speed, r * 0.7f);
    if (tick_ >= m.swing_ready_tick && horizontal_distance(m.pos, t->pos) <= r) {
      m.swing_ready_tick = tick_ + std::max<std::uint32_t>(1, ticks(m.def->attack_delay * attack_delay_multiplier(m.buffs)));
      pet_swing(m, *t, sink);
    }
    return;
  }
  // Nothing to fight: follow the owner or hold the guard spot, recovering like a calm mob.
  const bool guarding = m.pet_mode == PetMode::Guard;
  const Vec3 dest = guarding ? m.guard_pos : owner->pos;
  const float stop = guarding ? 0.3f : pr.follow_distance;
  const float d = horizontal_distance(m.pos, dest);
  if (d > stop + 0.5f) mob_move_toward(m, dest, m.def->run_speed * (d > 15.0f ? 1.5f : 1.0f), stop);
  if (m.hp < m.stats.max_hp && m.threat.empty()) {
    m.hp_acc += 0.01f * static_cast<float>(m.stats.max_hp) * dt_;
    if (m.hp_acc >= 1.0f) {
      const int whole = static_cast<int>(m.hp_acc);
      m.hp = std::min(m.stats.max_hp, m.hp + whole);
      m.hp_acc -= static_cast<float>(whole);
      m.changed_tick = tick_;
    }
  }
}

// ---------------------------------------------------------------- pet melee

void Zone::pet_swing(MobEntity& pet, MobEntity& target, ZoneSink& sink) {
  if (!pet.alive() || !target.alive() || target.is_pet()) return;
  PlayerEntity* owner = find(pet.owner);
  if (!owner) return;
  mark_combat(*owner);
  if (target.state == AiState::Leash) {
    combat_event(target.pos, pet.id, target.id, CombatKind::Evade, 0, "", sink);
    pet.pet_target = 0;
    return;
  }
  const AttackerView a{pet.def->level, pet.stats.offense, pet.stats.hit, 0.0f,
                       std::max(0.1f, 1.0f + stat_total(pet.buffs, BuffStat::MeleeDamagePct) / 100.0f)};
  const int avoid = target.stats.avoid_skill;
  const DefenderView d{target.def->level, target.stats.defense, mob_ac(target), avoid, avoid, 0, 1.0f};
  const SwingOutcome out = resolve_swing(a, d, rules_.combat, rng_);
  combat_event(target.pos, pet.id, target.id, swing_kind(out.result), static_cast<std::uint32_t>(out.damage), "", sink);
  const float threat_mult = pet.pet_def ? pet.pet_def->threat_mult : 1.0f;
  if (out.damage > 0) damage_mob(target, *owner, out.damage, 0.0, sink, true, &pet);
  else engage_id(target, pet.id, static_cast<double>(threat_mult));
}

void Zone::mob_swing_pet(MobEntity& m, MobEntity& pet, ZoneSink& sink) {
  if (!m.alive() || !pet.alive() || !pet.is_pet()) return;
  const AttackerView a{m.def->level, m.stats.offense, m.stats.hit, 0.0f,
                       std::max(0.1f, 1.0f + stat_total(m.buffs, BuffStat::MeleeDamagePct) / 100.0f)};
  const int avoid = pet.stats.avoid_skill;
  const DefenderView d{pet.def->level, pet.stats.defense, mob_ac(pet), avoid, avoid, 0,
                       std::max(0.0f, 1.0f + stat_total(pet.buffs, BuffStat::DamageTakenPct) / 100.0f)};
  const SwingOutcome out = resolve_swing(a, d, rules_.combat, rng_);
  combat_event(pet.pos, m.id, pet.id, swing_kind(out.result), static_cast<std::uint32_t>(out.damage), "", sink);
  if (PlayerEntity* owner = find(pet.owner)) mark_combat(*owner);
  if (out.damage <= 0) return;
  // The pet's own shield (fire servants) plus any damage-shield buffs on it burn the attacker.
  const int shield = (pet.pet_def ? pet_damage_shield(*pet.pet_def, pet.def->level) : 0) +
                     static_cast<int>(std::lround(stat_total(pet.buffs, BuffStat::DamageShield)));
  const EntityId pid = pet.id;
  if (shield > 0) reflect_damage_shield(m, pid, shield, sink);
  if (MobEntity* still = find_mob(pid); still && still->alive() && still->is_pet()) damage_pet(*still, out.damage, m.id, sink);
}

void Zone::damage_pet(MobEntity& pet, int damage, EntityId source, ZoneSink& sink) {
  if (!pet.alive() || damage <= 0) return;
  pet.hp -= damage;
  pet.changed_tick = tick_;
  on_mob_damaged(pet, true, sink);
  pet.threat.add(source, static_cast<double>(damage));  // a pet remembers who hurt it
  if (PlayerEntity* owner = find(pet.owner)) mark_combat(*owner);
  if (pet.hp > 0) return;
  if (pet.charmed) kill_mob(pet, nullptr, sink);
  else kill_pet(pet, sink);
}

// ---------------------------------------------------------------- heals and buffs on pets

void Zone::apply_to_pet(PlayerEntity& caster, const AbilityDef& a, MobEntity& pet, ZoneSink& sink) {
  if (!pet.alive() || !pet.is_pet()) return;
  int healed = 0;
  for (const auto& e : a.effects) {
    int amount = 0;
    if (e.type == EffectType::Heal) amount = roll_spread(effect_amount(e, caster.level), rules_.spells.damage_spread, rng_);
    else if (e.type == EffectType::HealPct) amount = static_cast<int>(std::lround(static_cast<float>(pet.stats.max_hp) * e.value / 100.0f));
    else continue;
    const int actual = std::max(0, std::min(pet.stats.max_hp - pet.hp, amount));
    pet.hp += actual;
    pet.changed_tick = tick_;
    healed += actual;
    combat_event(pet.pos, caster.id, pet.id, CombatKind::Heal, static_cast<std::uint32_t>(actual), a.name, sink, a.resist);
  }
  ActiveBuff b = build_buff(a, caster.level, caster.id, caster.char_id, caster.name, tick_, 1.0f / dt_, rules_.spells);
  b.illusion.clear();  // pets keep their own look
  if (has_lasting_part(b)) {
    const std::uint32_t dur = ms_of(b.expires_tick - tick_, dt_);
    const StackAction act = place_buff(pet.buffs, std::move(b));
    if (act == StackAction::Add || act == StackAction::Replace) {
      pet.buffs_dirty = true;
      pet.changed_tick = tick_;
      combat_event(pet.pos, caster.id, pet.id, CombatKind::Buff, dur, a.name, sink, a.resist);
    } else if (act == StackAction::Blocked) {
      combat_text(caster, "Your " + a.name + " would not take hold on your pet: a stronger effect is already active.", sink);
    }
  }
  // Healing a pet angers what it is fighting, as healing a player does.
  if (healed > 0) {
    std::vector<EntityId> engaged;
    for (const auto& [id, m] : mobs_)
      if (m.alive() && !m.is_pet() && m.state != AiState::Leash && m.threat.contains(pet.id) &&
          horizontal_distance(m.pos, caster.pos) <= rules_.spells.heal_aggro_radius)
        engaged.push_back(id);
    const double per = heal_threat(healed, engaged.size());
    for (EntityId id : engaged)
      if (MobEntity* m = find_mob(id)) engage(*m, caster, per, sink);
  }
}

// ---------------------------------------------------------------- commands and status

void Zone::pet_command(EntityId player, PetOrder order, EntityId target, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  MobEntity* pet = p.pet != 0 ? find_mob(p.pet) : nullptr;
  if (!pet || !pet->alive() || pet->owner != p.id) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You do not have a pet."});
    return;
  }
  if (p.dead) return;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSay, pet->def->name, text}); };
  switch (order) {
    case PetOrder::Attack: {
      const EntityId tid = target != 0 ? target : p.target;
      MobEntity* t = hostile_mob(tid);
      const bool visible = t && std::binary_search(p.known.begin(), p.known.end(), tid);
      if (!t || !visible) {
        say(tid == 0 ? "Attack what, master?" : "I cannot attack that, master.");
        break;
      }
      if (horizontal_distance(pet->pos, t->pos) > rules_.pets.leash_distance) {
        say("That is too far away, master.");
        break;
      }
      pet->pet_target = t->id;
      pet->passive_until = 0;
      say("Attacking " + t->def->name + ", master.");
      break;
    }
    case PetOrder::Follow:
      pet->pet_mode = PetMode::Follow;
      say("Following you, master.");
      break;
    case PetOrder::Guard:
      pet->pet_mode = PetMode::Guard;
      pet->guard_pos = pet->pos;
      say("Guarding this spot, master.");
      break;
    case PetOrder::BackOff:
      pet->pet_target = 0;
      pet->passive_until = tick_ + ticks(rules_.pets.back_off_seconds);
      say("Backing off, master.");
      break;
    case PetOrder::Dismiss:
      release_pet(*pet, true, "You release your pet.", sink);
      return;
  }
  send_pet_status(p, sink, true);
}

void Zone::send_pet_status(PlayerEntity& p, ZoneSink& sink, bool force) {
  proto::PetStatus s;
  const MobEntity* pet = p.pet != 0 ? find_mob(p.pet) : nullptr;
  if (pet && pet->alive() && pet->owner == p.id) {
    s.pet_id = pet->id;
    s.name = pet->def->name.substr(0, 48);
    s.level = static_cast<std::uint8_t>(std::clamp(pet->def->level, 1, 255));
    s.hp_pct = pct(pet->hp, pet->stats.max_hp);
    s.mode = static_cast<std::uint8_t>(pet->pet_mode);
    s.target_id = pet->pet_target;
    s.charmed = pet->charmed;
    if (pet->charmed) {
      std::uint32_t ends = tick_;
      for (const auto& b : pet->buffs)
        for (const auto& c : b.controls)
          if (c.bit == kControlCharm) ends = std::max(ends, c.expires_tick);
      s.charm_ms = ms_of(ends - tick_, dt_);
    }
  }
  std::uint64_t h = 0xCBF29CE484222325ULL;
  for (std::uint64_t v : {std::uint64_t{s.pet_id}, std::uint64_t{s.level}, std::uint64_t{s.hp_pct}, std::uint64_t{s.mode},
                          std::uint64_t{s.target_id}, std::uint64_t{s.charmed}})
    h = mix(h, v);
  const bool changed = h != p.pet_hash;
  // Nothing to say about no pet beyond the first "none"; otherwise on change or a 1 s heartbeat.
  if (!force && !changed && (s.pet_id == 0 || tick_ - p.pet_sent_tick < kHeartbeatTicks)) return;
  if (!force && changed && tick_ - p.pet_sent_tick < 2) return;
  p.pet_hash = h;
  p.pet_sent_tick = tick_;
  send(sink, p, s);
}

}  // namespace ef::game
