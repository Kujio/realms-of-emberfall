// Zone class mechanics: Bard songs, sneak/hide/invisibility, feign death, threat drops (evade),
// tracking, pick lock, summoned items, and illusions. The math lives in class_rules.cpp.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

}  // namespace

// ---------------------------------------------------------------- looks and flags

const std::string* Zone::illusion_of(const PlayerEntity& p) const {
  const std::string* look = nullptr;
  for (const auto& b : p.buffs)  // the newest illusion wins
    if (!b.illusion.empty()) look = &b.illusion;  // prune_lines clears an illusion when it ends
  return look;
}

std::uint8_t Zone::player_flags2(const PlayerEntity& p) const {
  std::uint8_t f = 0;
  if (p.feigned) f |= kFlag2Feigned;
  if (p.hidden || p.buff_total(BuffStat::Invisible) > 0.0f) f |= kFlag2Hidden;
  if (p.sneaking) f |= kFlag2Sneaking;
  if (p.song.active()) f |= kFlag2Singing;
  return f;
}

std::uint8_t Zone::mob_flags2(const MobEntity& m) const {
  std::uint8_t f = 0;
  if (m.is_pet()) f |= kFlag2Pet;
  if (m.charmed) f |= kFlag2Charmed;
  return f;
}

// ---------------------------------------------------------------- songs

void Zone::start_song(PlayerEntity& p, const AbilityDef& a, int gem, EntityId target, ZoneSink& sink) {
  const bool switching = p.song.active();
  p.song = SongState{&a, gem, target, tick_};
  p.changed_tick = tick_;
  send(sink, p, proto::SongUpdate{a.id, a.name, ms_of(ticks(rules_.songs.pulse_seconds), dt_), target});
  combat_text(p, std::string(switching ? "You switch to " : "You begin to sing ") + a.name + ".", sink);
  song_tick(p, sink);  // the first pulse lands at once
}

void Zone::stop_song(PlayerEntity& p, const std::string& message, ZoneSink& sink) {
  if (!p.song.active()) return;
  p.song = {};
  p.changed_tick = tick_;
  send(sink, p, proto::SongUpdate{"", "", 0, 0});
  if (!message.empty()) combat_text(p, message, sink);
}

void Zone::song_tick(PlayerEntity& p, ZoneSink& sink) {
  if (!p.song.active()) return;
  const AbilityDef& a = *p.song.ability;
  const std::uint8_t ctl = control_bits(p.buffs);
  if (p.dead || p.stunned_until > tick_ || (ctl & (kControlMez | kControlFear)) != 0 || p.feigned)
    return stop_song(p, "Your song is interrupted.", sink);
  const int g = p.song.gem;
  if (g >= 0 && (g >= gem_count(p.level, rules_.spells) || g >= static_cast<int>(kMaxGems) || p.gems[static_cast<std::size_t>(g)] != a.id))
    return stop_song(p, "Your song fades: it is no longer memorized.", sink);
  if (tick_ < p.song.next_pulse) return;
  p.song.next_pulse = tick_ + std::max<std::uint32_t>(1, ticks(rules_.songs.pulse_seconds));
  EntityId tid = 0;
  std::string why;
  if (!resolve_target(p, a, p.song.target, tid, why, true)) return stop_song(p, why, sink);
  if (!a.beneficial()) mark_combat(p);
  if (!a.skill.empty()) try_skill_up(p, a.skill, sink);
  combat_event(p.pos, p.id, tid, CombatKind::SongPulse, 0, a.name, sink, a.resist);
  resolve_ability(p, a, tid, sink);
}

// ---------------------------------------------------------------- stealth

void Zone::break_stealth(PlayerEntity& p, bool offensive, ZoneSink& sink) {
  bool changed = false;
  if (p.hidden) {
    p.hidden = false;
    changed = true;
    combat_text(p, "You are no longer hidden.", sink);
  }
  if (offensive && p.sneaking) {
    p.sneaking = false;
    changed = true;
    combat_text(p, "You stop sneaking.", sink);
  }
  if (offensive && p.buff_total(BuffStat::Invisible) > 0.0f) {
    std::erase_if(p.buffs, [](const ActiveBuff& b) {
      return std::any_of(b.mods.begin(), b.mods.end(), [](const StatMod& m) { return m.stat == BuffStat::Invisible; });
    });
    p.buffs_dirty = true;
    refresh_stats(p);
    changed = true;
    combat_text(p, "You fade back into view.", sink);
  }
  if (changed) p.changed_tick = tick_;
}

void Zone::stand_from_feign(PlayerEntity& p, ZoneSink& /*sink*/) {
  if (!p.feigned) return;
  p.feigned = false;
  p.changed_tick = tick_;
}

void Zone::feign_death(PlayerEntity& p, const AbilityDef& a, const EffectDef& e, ZoneSink& sink) {
  stop_song(p, "", sink);
  interrupt_cast(p, CastResult::Cancelled, "", sink);
  p.auto_attack = false;
  p.sitting = false;
  // The hardest foe that is after the player decides how convincing it has to be.
  int gap = 0;
  std::string fooled_by;
  for (const auto& [id, m] : mobs_)
    if (m.alive() && !m.is_pet() && m.threat.contains(p.id)) {
      if (m.def->level - p.level >= gap) fooled_by = m.def->name;
      gap = std::max(gap, m.def->level - p.level);
    }
  const float chance = e.value > 0.0f ? std::clamp(e.value / 100.0f, 0.0f, 1.0f)
                                      : feign_chance(p.skill(a.skill.empty() ? "feign_death" : a.skill), gap, rules_.stealth);
  if (!rng_.chance(chance)) {
    combat_event(p.pos, p.id, p.id, CombatKind::FeignFail, 0, a.name, sink);
    combat_text(p, "You fall to the ground, but " + (fooled_by.empty() ? std::string("nobody") : fooled_by) + " is not fooled.", sink);
    return;
  }
  p.feigned = true;
  p.changed_tick = tick_;
  for (auto& [id, m] : mobs_) {
    if (!m.threat.contains(p.id)) continue;
    m.threat.remove(p.id);
    if (m.threat.empty() && !m.is_pet()) m.forgot = true;  // it wanders home as it is
  }
  combat_event(p.pos, p.id, p.id, CombatKind::Feign, 0, a.name, sink);
  combat_text(p, "You fall to the ground and lie still.", sink);
}

void Zone::try_hide(PlayerEntity& p, const AbilityDef& a, ZoneSink& sink) {
  for (const auto& [id, m] : mobs_)
    if (m.alive() && !m.is_pet() && m.threat.contains(p.id)) {
      combat_event(p.pos, p.id, p.id, CombatKind::HideFail, 0, a.name, sink);
      combat_text(p, "You cannot hide while enemies are watching you.", sink);
      return;
    }
  if (!rng_.chance(hide_chance(p.skill(a.skill.empty() ? "hide" : a.skill), rules_.stealth))) {
    combat_event(p.pos, p.id, p.id, CombatKind::HideFail, 0, a.name, sink);
    combat_text(p, "You failed to hide yourself.", sink);
    return;
  }
  p.hidden = true;
  p.auto_attack = false;
  p.changed_tick = tick_;
  combat_event(p.pos, p.id, p.id, CombatKind::Hide, 0, a.name, sink);
  combat_text(p, "You have hidden yourself from view.", sink);
}

void Zone::toggle_sneak(PlayerEntity& p, ZoneSink& sink) {
  p.sneaking = !p.sneaking;
  p.changed_tick = tick_;
  p.stats_hash = 0;  // the client needs the new run speed now
  combat_text(p, p.sneaking ? "You begin to sneak." : "You stop sneaking.", sink);
}

void Zone::threat_drop(PlayerEntity& p, const AbilityDef& a, const EffectDef& e, MobEntity* target, ZoneSink& sink) {
  auto drop = [&](MobEntity& m) {
    if (!m.alive() || m.is_pet() || !m.threat.contains(p.id)) return;
    const double before = m.threat.threat_of(p.id);
    const double after = threat_after_drop(before, e.value);
    if (after <= 0.0) m.threat.remove(p.id);
    else m.threat.add(p.id, after - before);
    if (m.threat.empty()) m.forgot = true;
    combat_event(m.pos, p.id, m.id, CombatKind::ThreatDrop, static_cast<std::uint32_t>(std::lround(e.value)), a.name, sink, a.resist);
  };
  if (target) return drop(*target);
  const float radius = e.radius > 0.0f ? e.radius : a.radius;
  std::vector<EntityId> near;
  if (radius > 0.0f) grid_.query(p.pos.x, p.pos.z, radius, near);
  else
    for (const auto& [id, m] : mobs_) near.push_back(id);
  for (EntityId id : near)
    if (MobEntity* m = find_mob(id)) drop(*m);
}

// ---------------------------------------------------------------- tracking and locks

void Zone::track(PlayerEntity& p, const EffectDef& e, ZoneSink& sink) {
  const float radius = std::min(2000.0f, e.radius + e.value * static_cast<float>(p.skill("tracking")));
  std::vector<std::pair<float, EntityId>> found;
  for (const auto& [id, m] : mobs_)
    if (m.alive() && !m.is_pet()) {
      const float d = horizontal_distance(p.pos, m.pos);
      if (d <= radius) found.emplace_back(d, id);
    }
  std::sort(found.begin(), found.end());
  auto tell = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (found.empty()) return tell("You sense no creatures within " + std::to_string(static_cast<int>(radius)) + " meters.");
  tell("You track " + std::to_string(found.size()) + " creature" + (found.size() == 1 ? "" : "s") + " within " +
       std::to_string(static_cast<int>(radius)) + " meters:");
  for (std::size_t i = 0; i < found.size() && i < 8; ++i) {
    const MobEntity& m = mobs_.at(found[i].second);
    tell("  " + m.def->name + " (level " + std::to_string(m.def->level) + ", " + con_name(con_color(p.level, m.def->level, rules_.con)) + "), " +
         std::to_string(static_cast<int>(std::lround(found[i].first))) + " m to the " + compass_direction(m.pos.x - p.pos.x, m.pos.z - p.pos.z));
  }
}

void Zone::pick_lock(PlayerEntity& p, const AbilityDef& a, EntityId npc, ZoneSink& sink) {
  const NpcEntity* n = find_npc(npc);
  if (!n || n->def->role != "lockbox") return;
  const int skill = p.skill(a.skill.empty() ? "pick_lock" : a.skill);
  const bool ok = rng_.chance(pick_lock_chance(skill, n->def->lock_level - p.level, rules_.stealth));
  send(sink, p, proto::ChatMessage{kChatSystem, "", ok ? "You pick the lock of " + n->def->name + ". It swings open: empty, for now."
                                                    : "You fail to pick the lock of " + n->def->name + "."});
}

// ---------------------------------------------------------------- summoned items

void Zone::summon_item(PlayerEntity& p, const EffectDef& e, ZoneSink& sink) {
  const ItemDef* it = content_.find_item(e.ref);
  if (!it) return;
  const int count = std::max(1, static_cast<int>(std::lround(e.value)));
  ItemStackState stack{it->id, count, e.duration > 0.0f ? tick_ + std::max<std::uint32_t>(1, ticks(e.duration)) : 0u};
  const bool all = give_item(p, stack);
  const int given = count - stack.count;
  if (given > 0) {
    combat_text(p, (given == 1 ? it->name : std::to_string(given) + " " + it->name) + " appear" + (given == 1 ? "s" : "") + " in your pack.", sink);
    send_inventory(p, sink);
  }
  if (!all) combat_text(p, "Your pack is full; the rest of the conjuring fades.", sink);
}

void Zone::expire_items(PlayerEntity& p, ZoneSink& sink) {
  bool changed = false;
  bool gear = false;
  p.inv.for_each_mut([&](SlotCode code, ItemStackState& s) {
    if (s.expires_tick == 0 || s.expires_tick > tick_) return;
    if (p.cast.casting() && p.cast.item_slot == code) return;  // fades once the cast resolves
    combat_text(p, "Your " + item_name(s.item_id) + " fades away.", sink);
    s = {};
    changed = true;
    if (slot_container(code) == kContEquip) gear = true;
  });
  if (!changed) return;
  if (gear) gear_changed(p, sink);
  mark_inventory(p);
  send_inventory(p, sink);
}

float Zone::item_seconds_left(const ItemStackState& s) const {
  if (s.expires_tick == 0) return 0.0f;
  return s.expires_tick > tick_ ? std::max(0.05f, static_cast<float>(s.expires_tick - tick_) * dt_) : 0.05f;
}

}  // namespace ef::game
