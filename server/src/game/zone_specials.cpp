// Mob special abilities and boss mechanics (Phase 6): procs and casts through the spell engine,
// area attacks that punish a crowd, enrage below a health threshold, enrage timers, rampage,
// summoning a fleeing target, and add phases. Pure triggers live in mob_specials.hpp.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

void Zone::mob_say(const MobEntity& m, const std::string& text, ZoneSink& sink) {
  if (text.empty()) return;
  broadcast(m.pos, proto::ChatMessage{kChatSay, m.def->name, text}, sink);
}

void Zone::mob_cast(MobEntity& m, const AbilityDef& a, PlayerEntity& p, ZoneSink& sink) {
  if (p.dead || !m.alive()) return;
  // Resisted like a player's spell, against the player's own resist and buffs.
  const auto rt = static_cast<std::size_t>(a.resist);
  const int resist = (rt < p.stats.resists.size() ? p.stats.resists[rt] : 0) + resist_bonus(p.buffs, a.resist);
  const bool deals = a.has(EffectType::Damage);
  const float chance = resist_chance(a.resist, resist, m.def->level, p.level, rules_.spells);
  const ResistRoll roll = roll_resist(chance, deals, rules_.spells, rng_);
  if (roll.resisted) {
    combat_event(p.pos, m.id, p.id, CombatKind::Resist, 0, a.name, sink, a.resist);
    return;
  }
  mark_combat(p);
  const EntityId pid = p.id;
  for (const auto& e : a.effects) {
    if (e.type != EffectType::Damage) continue;
    const int dmg = std::max(1, static_cast<int>(std::lround(static_cast<float>(effect_amount(e, m.def->level)) * roll.fraction)));
    combat_event(p.pos, m.id, p.id, CombatKind::SpellHit, static_cast<std::uint32_t>(dmg), a.name, sink, a.resist);
    damage_player(p, dmg, m.id, m.def->name, sink);
    if (!find(pid) || p.dead) return;
  }
  ActiveBuff probe = build_buff(a, m.def->level, 0, 0, "", tick_, 1.0f / dt_, rules_.spells);
  if (!has_lasting_part(probe)) return;
  const StackAction act = force_effect(p.id, a, m.def->level, sink, m.id, m.def->name);
  if (act == StackAction::Add || act == StackAction::Replace) {
    CombatKind kind = CombatKind::Debuff;
    if (a.has(EffectType::Root)) kind = CombatKind::Root;
    else if (a.has(EffectType::Fear)) kind = CombatKind::Fear;
    else if (a.has(EffectType::Snare)) kind = CombatKind::Snare;
    else if (a.has(EffectType::Stun)) kind = CombatKind::Stun;
    const std::uint32_t ms = static_cast<std::uint32_t>(std::lround(a.lasting_seconds() * 1000.0f));
    combat_event(p.pos, m.id, p.id, kind, ms, a.name, sink, a.resist);
  }
}

void Zone::dismiss_adds(MobEntity& m, ZoneSink& sink) {
  for (EntityId add : m.adds)
    if (MobEntity* a = find_mob(add); a && a->alive() && a->add_of == m.id) remove_pet_entity(*a, sink);  // no corpse, off every hate list
  m.adds.clear();
}

void Zone::reset_specials(MobEntity& m, ZoneSink& sink) {
  m.special_ready.assign(m.def->specials.size(), 0);
  m.adds_fired.assign(m.def->specials.size(), 0);
  m.fight_start = 0;
  m.enrage_pct = 0.0f;
  m.enraged = false;
  m.timer_enraged = false;
  // A boss that resets takes its summoned helpers with it.
  dismiss_adds(m, sink);
}

void Zone::mob_specials(MobEntity& m, EntityId target, ZoneSink& sink) {
  const auto& specials = m.def->specials;
  if (specials.empty()) return;
  if (m.special_ready.size() != specials.size()) {
    m.special_ready.assign(specials.size(), 0);
    m.adds_fired.assign(specials.size(), 0);
  }
  if (m.fight_start == 0) {
    m.fight_start = tick_;
    // Timed abilities wait a first interval before they fire.
    for (std::size_t i = 0; i < specials.size(); ++i)
      if (specials[i].every > 0.0f) m.special_ready[i] = tick_ + ticks(specials[i].every);
  }
  PlayerEntity* tp = find(target);
  const float hp_frac = static_cast<float>(m.hp) / static_cast<float>(std::max(1, m.stats.max_hp));
  const float fight_s = static_cast<float>(tick_ - m.fight_start) * dt_;
  for (std::size_t i = 0; i < specials.size(); ++i) {
    const MobSpecial& s = specials[i];
    switch (s.type) {
      case MobSpecialType::Enrage:
        if (!m.enraged && hp_frac <= s.below) {
          m.enraged = true;
          m.enrage_pct += s.value;
          mob_say(m, s.message.empty() ? "becomes enraged!" : s.message, sink);
        }
        break;
      case MobSpecialType::EnrageTimer:
        if (!m.timer_enraged && fight_s >= s.seconds) {
          m.timer_enraged = true;
          m.enrage_pct += s.value;
          mob_say(m, s.message.empty() ? "grows impatient and strikes with terrible fury!" : s.message, sink);
        }
        break;
      case MobSpecialType::Adds: {
        // Each threshold fires once per fight.
        while (m.adds_fired[i] < static_cast<int>(s.at.size()) && hp_frac <= s.at[static_cast<std::size_t>(m.adds_fired[i])]) {
          ++m.adds_fired[i];
          const MobDef* add = content_.find_mob(s.mob);
          if (!add) break;
          mob_say(m, s.message.empty() ? "calls for aid!" : s.message, sink);
          for (int k = 0; k < s.count; ++k) {
            const float a = static_cast<float>(k) * 2.0f * kPi / static_cast<float>(std::max(1, s.count)) + static_cast<float>(m.adds_fired[i]);
            const EntityId id = spawn_mob(*add, {m.pos.x + std::cos(a) * 4.0f, m.pos.y, m.pos.z + std::sin(a) * 4.0f});
            if (MobEntity* am = find_mob(id)) {
              am->home = m.home;
              am->add_of = m.id;
              m.adds.push_back(id);
              if (target != 0) engage_id(*am, target, 1.0);
              set_mob_state(*am, AiState::Chase);
            }
          }
        }
        break;
      }
      case MobSpecialType::Cast:
      case MobSpecialType::Aoe:
      case MobSpecialType::Summon: {
        if (tick_ < m.special_ready[i]) break;
        // A blast around the mob needs no player target (a pet may be holding it); everything else does.
        const bool around_self = s.type == MobSpecialType::Aoe && s.on_self;
        if (!around_self && (!tp || tp->dead)) break;
        const float d = tp ? horizontal_distance(m.pos, tp->pos) : 0.0f;
        if (s.type == MobSpecialType::Summon) {
          if (d <= s.range) break;  // close enough: no need
          m.special_ready[i] = tick_ + ticks(s.every);
          mob_say(m, s.message.empty() ? "You will not escape me!" : s.message, sink);
          const Vec3 f = forward_of(m.yaw);
          relocate(*tp, {m.pos.x + f.x * 2.0f, 0.0f, m.pos.z + f.z * 2.0f}, tp->yaw, sink, "summoned");
          break;
        }
        const AbilityDef* a = content_.find_mob_spell(s.effect);
        if (!a) break;
        if (s.type == MobSpecialType::Cast) {
          if (s.range > 0.0f && d > s.range) break;
          if (!collision_.segment_clear(m.pos, tp->pos)) break;
          m.special_ready[i] = tick_ + ticks(s.every);
          mob_say(m, s.message, sink);
          mob_cast(m, *a, *tp, sink);
          break;
        }
        // Area: everyone within `range` of the target (or of the mob).
        m.special_ready[i] = tick_ + ticks(s.every);
        const Vec3 centre = around_self ? m.pos : tp->pos;
        mob_say(m, s.message, sink);
        scratch_dead_.clear();
        grid_.query(centre.x, centre.z, s.range, scratch_dead_);
        const std::vector<EntityId> hit(scratch_dead_.begin(), scratch_dead_.end());
        for (EntityId id : hit)
          if (PlayerEntity* p = find(id); p && !p->dead && horizontal_distance(p->pos, centre) <= s.range) {
            if (!find_mob(m.id) || !m.alive()) return;
            mob_cast(m, *a, *p, sink);
          }
        break;
      }
      case MobSpecialType::Support: {
        if (tick_ < m.special_ready[i]) break;
        const AbilityDef* a = content_.find_mob_spell(s.effect);
        if (!a) break;
        MobEntity* ally = support_target(m, *a, s);
        if (!ally) {
          m.special_ready[i] = tick_ + ticks(1.0f);  // nobody needs it yet: look again in a second
          break;
        }
        m.special_ready[i] = tick_ + ticks(s.every);
        mob_say(m, s.message, sink);
        mob_support(m, *a, *ally, sink);
        break;
      }
      case MobSpecialType::Proc:
      case MobSpecialType::Rampage:
        break;  // on a landed hit (mob_swing)
    }
  }
}

MobEntity* Zone::support_target(const MobEntity& m, const AbilityDef& a, const MobSpecial& s) {
  const bool heals = a.has(EffectType::Heal) || a.has(EffectType::Hot);
  const bool instant = a.has(EffectType::Heal);
  const float below = s.below > 0.0f ? s.below : 1.0f;
  // What it would leave on the ally: one the stacking rules refuse (a stronger effect of its group) is no
  // help, unless the spell also heals at once.
  const ActiveBuff probe = build_buff(a, m.def->level, m.id, 0, m.def->name, tick_, 1.0f / dt_, rules_.spells);
  const bool lasting = has_lasting_part(probe);
  MobEntity* best = nullptr;
  float best_frac = 2.0f;
  scratch_dead_.clear();
  grid_.query(m.pos.x, m.pos.z, s.range, scratch_dead_);
  for (const EntityId id : scratch_dead_) {
    MobEntity* o = find_mob(id);
    // Allies: the caster itself, or a mob of its faction in a fight of its own (not a pet or a charmed mob).
    if (!o || !o->alive() || o->is_pet() || o->def->faction != m.def->faction) continue;
    if (o->id != m.id && o->threat.entries().empty()) continue;
    if (horizontal_distance(o->pos, m.pos) > s.range) continue;
    if (o->id != m.id && !collision_.segment_clear(m.pos, o->pos)) continue;
    // An effect it already carries from this spell is not cast again.
    if (std::any_of(o->buffs.begin(), o->buffs.end(), [&](const ActiveBuff& b) { return b.ability_id == a.id; })) continue;
    if (lasting && !instant) {
      const StackAction fit = decide_stacking(o->buffs, probe, rules_.spells.buff_slots, rules_.spells.debuff_slots).action;
      if (fit != StackAction::Add && fit != StackAction::Replace) continue;
    }
    const float frac = static_cast<float>(o->hp) / static_cast<float>(std::max(1, o->stats.max_hp));
    if (heals && frac >= below) continue;
    // A heal goes to the most hurt; a buff to the first who lacks it (the caster before the rest).
    const float rank = heals ? frac : (o->id == m.id ? -1.0f : 0.0f);
    if (rank < best_frac) {
      best_frac = rank;
      best = o;
    }
  }
  return best;
}

void Zone::mob_support(MobEntity& m, const AbilityDef& a, MobEntity& ally, ZoneSink& sink) {
  const EntityId aid = ally.id;
  for (const auto& e : a.effects) {
    if (e.type != EffectType::Heal) continue;
    const int healed = std::min(ally.stats.max_hp - ally.hp, std::max(1, effect_amount(e, m.def->level)));
    if (healed <= 0) continue;
    ally.hp += healed;
    ally.changed_tick = tick_;
    combat_event(ally.pos, m.id, aid, CombatKind::Heal, static_cast<std::uint32_t>(healed), a.name, sink, a.resist);
  }
  ActiveBuff probe = build_buff(a, m.def->level, 0, 0, "", tick_, 1.0f / dt_, rules_.spells);
  if (!has_lasting_part(probe)) return;
  const StackAction act = force_effect(aid, a, m.def->level, sink, m.id, m.def->name);
  if (act == StackAction::Add || act == StackAction::Replace) {
    const std::uint32_t ms = static_cast<std::uint32_t>(std::lround(a.lasting_seconds() * 1000.0f));
    combat_event(ally.pos, m.id, aid, CombatKind::Buff, ms, a.name, sink, a.resist);
  }
}

void Zone::mob_on_hit(MobEntity& m, PlayerEntity& p, ZoneSink& sink) {
  for (const MobSpecial& s : m.def->specials) {
    if (!m.alive() || p.dead) return;
    if (s.type == MobSpecialType::Proc && rng_.chance(s.chance)) {
      if (const AbilityDef* a = content_.find_mob_spell(s.effect)) {
        mob_say(m, s.message, sink);
        mob_cast(m, *a, p, sink);
      }
    } else if (s.type == MobSpecialType::Rampage && rng_.chance(s.chance)) {
      // Another foe on its hate list, in reach.
      const float r = reach(m);
      for (const auto& entry : m.threat.entries()) {
        if (entry.id == p.id) continue;
        PlayerEntity* other = find(entry.id);
        if (!other || other->dead || horizontal_distance(other->pos, m.pos) > r) continue;
        mob_say(m, s.message.empty() ? "goes on a rampage!" : s.message, sink);
        mob_swing(m, *other, sink, true);
        break;
      }
    }
  }
}

}  // namespace ef::game
