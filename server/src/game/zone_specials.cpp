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
  const StackAction act = force_effect(p.id, a, m.def->level, sink);
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
      case MobSpecialType::Proc:
      case MobSpecialType::Rampage:
        break;  // on a landed hit (mob_swing)
    }
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
