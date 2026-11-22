#include "balance_sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include "game/zone.hpp"

namespace ef::sim {

using namespace ef::game;

namespace {

// Discards everything the zone sends: the autopilot reads state straight from the zone.
struct NullSink final : ZoneSink {
  void zone_send(const PlayerEntity&, std::uint8_t, std::span<const std::uint8_t>) override {}
  void zone_correction(const PlayerEntity&, const char*) override {}
  void zone_violation(const PlayerEntity&, const char*) override {}
};

constexpr float kDt = 0.05f;
constexpr double kDtD = 0.05;

ZoneDef arena() {
  ZoneDef z;
  z.id = "sim_arena";
  z.name = "Simulation Arena";
  z.min_x = -500;
  z.min_z = -500;
  z.max_x = 500;
  z.max_z = 500;
  z.interest_radius = 80;
  z.say_radius = 30;
  return z;
}

bool is_buff_stat_useful(BuffStat s) { return s != BuffStat::Invisible && s != BuffStat::MoveSpeedPct; }

// The kit the autopilot plays: memorized gems with their roles, and the abilities it presses.
struct Kit {
  std::vector<std::string> gems;
  int pet = -1, heal = -1, nuke = -1, slow = -1, tap = -1;
  std::vector<int> dots;
  std::vector<int> buffs;  // cast before each fight
  std::vector<int> songs;  // Bards: the twist, in order
  std::vector<const AbilityDef*> strikes, offense_discs, defense_discs, heal_abilities;
  bool melee = false;
};

Kit build_kit(const Content& c, const std::string& cls, int level) {
  Kit k;
  const ClassDef* cd = c.find_class(cls);
  k.melee = cd && (cd->archetype == "tank" || cd->archetype == "hybrid" || cd->archetype == "melee");
  // The best rank of every spell line known at this level.
  std::map<std::string, const AbilityDef*> best;
  for (const AbilityDef* a : c.spells_for(cls)) {
    if (a->level > level) continue;
    auto& slot = best[a->stack_group];
    if (!slot || a->rank > slot->rank || (a->rank == slot->rank && a->level > slot->level)) slot = a;
  }
  const AbilityDef *pet = nullptr, *heal = nullptr, *nuke = nullptr, *slow = nullptr, *tap = nullptr;
  std::vector<const AbilityDef*> dots, buffs, songs_dot, songs_haste, songs_regen;
  auto later = [](const AbilityDef* a, const AbilityDef* b) { return !b || a->level > b->level; };
  for (const auto& [group, a] : best) {
    if (a->song) {
      if (a->target == TargetType::Enemy && (a->has(EffectType::Dot) || a->has(EffectType::Damage))) songs_dot.push_back(a);
      else if (a->beneficial() && std::any_of(a->effects.begin(), a->effects.end(), [](const EffectDef& e) { return e.stat == BuffStat::AttackSpeedPct; }))
        songs_haste.push_back(a);
      else if (a->beneficial() && std::any_of(a->effects.begin(), a->effects.end(), [](const EffectDef& e) { return e.stat == BuffStat::HpRegen; }))
        songs_regen.push_back(a);
      continue;
    }
    if (a->has(EffectType::SummonPet)) {
      if (later(a, pet)) pet = a;
    } else if (a->has(EffectType::Heal) && a->target != TargetType::Pet && a->beneficial()) {
      if (later(a, heal)) heal = a;
    } else if (a->target == TargetType::Enemy && a->has(EffectType::Lifetap)) {
      if (later(a, tap)) tap = a;
    } else if (a->target == TargetType::Enemy && a->has(EffectType::Damage) && a->requires_body.empty() && a->cooldown <= 12.0f && a->type == "nuke") {
      if (later(a, nuke)) nuke = a;
    } else if (a->target == TargetType::Enemy && (a->has(EffectType::Dot) || a->has(EffectType::Drain)) && a->requires_body.empty()) {
      dots.push_back(a);
    } else if (a->target == TargetType::Enemy && a->has(EffectType::Slow)) {
      if (later(a, slow)) slow = a;
    } else if (a->beneficial() && a->target != TargetType::Group && a->lasting_seconds() >= 120.0f &&
               std::all_of(a->effects.begin(), a->effects.end(), [](const EffectDef& e) { return e.type == EffectType::Buff && is_buff_stat_useful(e.stat); })) {
      buffs.push_back(a);
    }
  }
  std::sort(dots.begin(), dots.end(), [](const AbilityDef* a, const AbilityDef* b) { return a->level > b->level; });
  std::sort(buffs.begin(), buffs.end(), [](const AbilityDef* a, const AbilityDef* b) { return a->level > b->level; });
  auto by_level = [](std::vector<const AbilityDef*>& v) {
    std::sort(v.begin(), v.end(), [](const AbilityDef* a, const AbilityDef* b) { return a->level > b->level; });
  };
  by_level(songs_dot);
  by_level(songs_haste);
  by_level(songs_regen);

  const int gem_slots = std::min<int>(gem_count(level, c.rules.spells), static_cast<int>(kMaxGems));
  auto add = [&](const AbilityDef* a) -> int {
    if (!a || static_cast<int>(k.gems.size()) >= gem_slots) return -1;
    k.gems.push_back(a->id);
    return static_cast<int>(k.gems.size()) - 1;
  };
  if (!songs_dot.empty() || !songs_haste.empty()) {
    for (auto* list : {&songs_dot, &songs_haste, &songs_regen})
      if (!list->empty())
        if (int g = add(list->front()); g >= 0) k.songs.push_back(g);
  }
  k.pet = add(pet);
  k.heal = add(heal);
  k.nuke = add(nuke);
  if (!dots.empty()) k.dots.push_back(add(dots[0]));
  k.slow = add(slow);
  k.tap = add(tap);
  if (dots.size() > 1) k.dots.push_back(add(dots[1]));
  for (const AbilityDef* b : buffs)
    if (b->target != TargetType::Pet || pet)
      if (int g = add(b); g >= 0) k.buffs.push_back(g);
  std::erase(k.dots, -1);

  for (const AbilityDef* a : c.abilities_for(cls)) {
    if (a->level > level || a->passive() || a->cast_time > 0.0f) continue;
    if (a->target == TargetType::Enemy && a->has(EffectType::Damage) && a->position.empty() && a->cooldown <= 60.0f) k.strikes.push_back(a);
    else if (a->type == "discipline" && a->target == TargetType::Self) {
      const bool defensive = std::any_of(a->effects.begin(), a->effects.end(), [](const EffectDef& e) { return e.stat == BuffStat::DamageTakenPct || e.stat == BuffStat::Ac; });
      const bool threat = std::any_of(a->effects.begin(), a->effects.end(), [](const EffectDef& e) { return e.stat == BuffStat::ThreatPct; });
      if (threat) continue;
      (defensive ? k.defense_discs : k.offense_discs).push_back(a);
    } else if (a->target == TargetType::Self && a->has(EffectType::HealPct)) {
      k.heal_abilities.push_back(a);
    }
  }
  return k;
}

std::uint32_t cooldown_of(const PlayerEntity& p, const std::string& id) {
  for (const auto& [cid, at] : p.cooldowns)
    if (cid == id) return at;
  return 0;
}

bool on_target(const MobEntity& m, const AbilityDef& a, EntityId caster) {
  return std::any_of(m.buffs.begin(), m.buffs.end(), [&](const ActiveBuff& b) { return b.stack_group == a.stack_group && b.caster == caster; });
}

}  // namespace

std::vector<std::string> kit_summary(const Content& content, const std::string& class_id, int level) {
  const Kit k = build_kit(content, class_id, level);
  std::vector<std::string> out = k.gems;
  for (const AbilityDef* a : k.strikes) out.push_back(a->id);
  for (const AbilityDef* a : k.offense_discs) out.push_back(a->id);
  return out;
}

FightStats simulate(const Content& content, const std::string& class_id, int level, const SimOptions& opt) {
  FightStats st;
  st.class_id = class_id;
  st.level = level;
  const Kit kit = build_kit(content, class_id, level);
  const ClassDef* cd = content.find_class(class_id);
  if (!cd) return st;
  const MobDef* base = content.find_mob("road_brigand");
  if (!base && !content.mobs.empty()) base = &content.mobs.front();
  if (!base) return st;
  double ttk = 0.0, hp_left = 0.0, mana_pct = 0.0, dpm = 0.0;
  int mana_fights = 0;
  // One character fights every bout in turn, resting a minute between them, so recast timers
  // (disciplines, once-an-hour abilities) carry over the way they do in play.
  Zone zone(arena(), content, kDt, opt.seed * 7919u + static_cast<std::uint64_t>(level) * 131u);
  NullSink sink;
  std::uint32_t t = 0;
  EntityId p = 0;
  {
    ZonePlayerInit init;
    init.name = "Sim";
    init.race = "highlander";
    init.class_id = class_id;
    init.char_id = 1;
    init.level = level;
    init.bind_zone = "sim_arena";
    for (const AbilityDef* a : content.spells_for(class_id))
      if (a->level <= level) init.spellbook.push_back(a->id);
    init.gems = kit.gems;
    for (const auto& [sid, cap] : cd->skills) init.skills.push_back({sid, skill_cap(cap, level)});
    if (opt.gear)
      if (const RaceDef* race = content.find_race(init.race))
        for (const auto& [slot, id] : recommended_kit(content, *race, *cd, level)) {
          const ItemDef* it = content.find_item(id);
          init.items.push_back({slot, ItemStackState{id, it && it->kind == "ammo" ? std::min(it->stack, 50) : 1, 0}});
        }
    p = zone.add_player(init);
  }
  auto me = [&]() -> PlayerEntity& { return *zone.find(p); };
  auto tick = [&] { zone.tick(++t, sink); };
  auto wait_ready = [&](int limit) {
    for (int i = 0; i < limit && (me().cast.casting() || zone.now() < me().gcd_until); ++i) tick();
  };
  for (int f = 0; f < opt.fights; ++f) {
    if (me().dead) zone.respawn(p, sink);
    if (f > 0)  // rest a minute between pulls
      for (int i = 0; i < static_cast<int>(60.0f / kDt); ++i) tick();
    // Rested and buffed before the pull: summon the pet, then every pre-fight buff that is missing.
    if (kit.pet >= 0 && (me().pet == 0 || !zone.find_mob(me().pet))) {
      wait_ready(200);
      zone.cast_spell(p, kit.pet, p, sink);
      wait_ready(400);
    }
    for (int g : kit.buffs) {
      const AbilityDef* a = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
      if (!a) continue;
      wait_ready(200);
      const EntityId target = a->target == TargetType::Pet ? me().pet : p;
      if (target == 0) continue;
      const MobEntity* pet = a->target == TargetType::Pet ? zone.find_mob(target) : nullptr;
      const auto& have = pet ? pet->buffs : me().buffs;
      if (std::any_of(have.begin(), have.end(), [&](const ActiveBuff& b) { return b.stack_group == a->stack_group && b.expires_tick > zone.now() + 1200; }))
        continue;
      zone.cast_spell(p, g, target, sink);
      wait_ready(400);
    }
    me().hp = me().stats.max_hp;
    me().mana = me().stats.max_mana;
    me().endurance = me().stats.max_endurance;

    MobDef foe = *base;
    foe.id = "sim_foe";
    foe.name = "a sparring foe";
    foe.level = level;
    foe.hp_mult = opt.mob_hp_mult;
    foe.damage_mult = opt.mob_damage_mult;
    foe.aggro_radius = 0;
    foe.assist = false;
    foe.flee_hp_pct = 0;
    foe.loot = "";
    foe.wander_radius = 0;
    const EntityId m = zone.spawn_mob(foe, {me().pos.x + 25.0f, 0, me().pos.z});
    tick();
    zone.set_target(p, m, sink);
    if (me().pet != 0) zone.pet_command(p, PetOrder::Attack, m, sink);
    const int mob_hp = zone.find_mob(m)->stats.max_hp;
    const std::uint32_t start = t;
    int mana_prev = me().mana;
    double mana_spent = 0.0;
    std::size_t song_idx = 0;
    std::uint32_t song_switch = 0;
    bool won = false;
    const auto limit = static_cast<int>(opt.fight_seconds / kDt);
    for (int i = 0; i < limit; ++i) {
      PlayerEntity& pl = me();
      if (pl.dead) break;
      MobEntity* mob = zone.find_mob(m);
      if (!mob || !mob->alive()) {
        won = true;
        break;
      }
      const float d = horizontal_distance(pl.pos, mob->pos);
      if (kit.melee && d > 2.5f) {  // run in at normal speed
        const float step = std::min(content.rules.movement.run_speed * kDt, d - 2.0f);
        pl.pos.x += (mob->pos.x - pl.pos.x) / d * step;
        pl.pos.z += (mob->pos.z - pl.pos.z) / d * step;
      }
      if (d <= 6.0f && !pl.auto_attack) zone.set_auto_attack(p, true, sink);
      const float hp_frac = static_cast<float>(pl.hp) / static_cast<float>(std::max(1, pl.stats.max_hp));
      auto spell_ready = [&](int g) {
        if (g < 0) return false;
        const AbilityDef* a = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
        return a && pl.mana >= a->cost && zone.now() >= pl.gem_ready[static_cast<std::size_t>(g)] && zone.now() >= cooldown_of(pl, a->id);
      };
      auto try_cast = [&](int g, EntityId target) {
        if (!spell_ready(g)) return false;
        zone.cast_spell(p, g, target, sink);
        return true;
      };
      // Bards twist: switch songs every pulse.
      if (!kit.songs.empty() && zone.now() >= pl.gcd_until && (!pl.song.active() || zone.now() >= song_switch)) {
        const int g = kit.songs[song_idx % kit.songs.size()];
        const AbilityDef* a = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
        if (a && !(pl.song.active() && pl.song.ability == a)) zone.cast_spell(p, g, a->beneficial() ? p : m, sink);
        ++song_idx;
        song_switch = zone.now() + static_cast<std::uint32_t>(std::lround(content.rules.songs.pulse_seconds / kDt));
      }
      const bool free = !pl.cast.casting() && zone.now() >= pl.gcd_until;
      if (free) {
        bool acted = false;
        if (hp_frac < 0.45f) {
          acted = try_cast(kit.heal, p) || try_cast(kit.tap, m);
          for (const AbilityDef* a : kit.heal_abilities)
            if (!acted && zone.now() >= cooldown_of(pl, a->id)) {
              zone.use_ability(p, a->id, p, sink);
              acted = true;
            }
          for (const AbilityDef* a : kit.defense_discs)
            if (zone.now() >= cooldown_of(pl, a->id) && pl.endurance >= a->cost) zone.use_ability(p, a->id, p, sink);
        }
        if (!acted && kit.slow >= 0) {
          const AbilityDef* a = content.find_spell(kit.gems[static_cast<std::size_t>(kit.slow)]);
          if (a && !on_target(*mob, *a, p)) acted = try_cast(kit.slow, m);
        }
        for (int g : kit.dots) {
          if (acted) break;
          const AbilityDef* a = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
          if (a && !on_target(*mob, *a, p)) acted = try_cast(g, m);
        }
        if (!acted) acted = try_cast(kit.nuke, m);
        if (!acted && hp_frac < 0.8f) acted = try_cast(kit.tap, m);
      }
      // Melee abilities and disciplines whenever they are ready.
      if (!pl.cast.casting()) {
        for (const AbilityDef* a : kit.offense_discs)
          if (zone.now() >= cooldown_of(pl, a->id) && pl.endurance >= a->cost) zone.use_ability(p, a->id, p, sink);
        for (const AbilityDef* a : kit.strikes) {
          if (zone.now() < cooldown_of(pl, a->id) || (a->gcd && zone.now() < pl.gcd_until)) continue;
          if (a->resource == Resource::Endurance && pl.endurance < a->cost) continue;
          if (a->resource == Resource::Mana && pl.mana < a->cost) continue;
          if (d > a->range + 1.0f) continue;
          zone.use_ability(p, a->id, m, sink);
        }
      }
      tick();
      if (const PlayerEntity* now = zone.find(p)) {
        if (now->mana < mana_prev) mana_spent += mana_prev - now->mana;
        mana_prev = now->mana;
      }
    }
    ++st.fights;
    if (won && !me().dead) {
      ++st.wins;
      ttk += static_cast<double>(t - start) * kDtD;
      hp_left += 100.0 * me().hp / std::max(1, me().stats.max_hp);
      if (me().stats.max_mana > 0) {
        mana_pct += 100.0 * mana_spent / me().stats.max_mana;
        if (mana_spent > 0.0) dpm += mob_hp / mana_spent;
        ++mana_fights;
      }
    }
  }
  if (st.wins > 0) {
    st.ttk_seconds = ttk / st.wins;
    st.hp_left_pct = hp_left / st.wins;
  }
  if (mana_fights > 0) {
    st.mana_used_pct = mana_pct / mana_fights;
    st.damage_per_mana = dpm / mana_fights;
  }
  return st;
}

std::vector<Outlier> find_outliers(const std::vector<FightStats>& rows, double ttk_factor, double min_win_rate) {
  std::vector<Outlier> out;
  std::map<int, std::vector<double>> ttks;
  for (const auto& r : rows)
    if (r.wins > 0) ttks[r.level].push_back(r.ttk_seconds);
  std::map<int, double> median;
  for (auto& [lvl, v] : ttks) {
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    median[lvl] = v.size() % 2 == 1 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
  }
  char buf[160];
  for (const auto& r : rows) {
    if (r.win_rate() < min_win_rate) {
      std::snprintf(buf, sizeof buf, "wins only %d of %d fights", r.wins, r.fights);
      out.push_back({r.class_id, r.level, buf});
    }
    if (r.wins == 0 || !median.contains(r.level)) continue;
    const double m = median[r.level];
    if (r.ttk_seconds > m * ttk_factor) {
      std::snprintf(buf, sizeof buf, "kills in %.1f s, %.1fx the median %.1f s", r.ttk_seconds, r.ttk_seconds / m, m);
      out.push_back({r.class_id, r.level, buf});
    } else if (r.ttk_seconds * ttk_factor < m) {
      std::snprintf(buf, sizeof buf, "kills in %.1f s, %.1fx faster than the median %.1f s", r.ttk_seconds, m / r.ttk_seconds, m);
      out.push_back({r.class_id, r.level, buf});
    }
  }
  return out;
}

std::string format_table(const std::vector<FightStats>& rows, const std::vector<int>& levels) {
  std::ostringstream os;
  std::vector<std::string> classes;
  for (const auto& r : rows)
    if (std::find(classes.begin(), classes.end(), r.class_id) == classes.end()) classes.push_back(r.class_id);
  auto find = [&](const std::string& c, int l) -> const FightStats* {
    for (const auto& r : rows)
      if (r.class_id == c && r.level == l) return &r;
    return nullptr;
  };
  char buf[64];
  auto section = [&](const char* title, auto cell) {
    os << title << "\n" << std::string(14, ' ');
    for (int l : levels) {
      std::snprintf(buf, sizeof buf, "%9s", ("L" + std::to_string(l)).c_str());
      os << buf;
    }
    os << "\n";
    for (const auto& c : classes) {
      std::snprintf(buf, sizeof buf, "%-14s", c.c_str());
      os << buf;
      for (int l : levels) {
        const FightStats* r = find(c, l);
        os << (r ? cell(*r) : std::string("        -"));
      }
      os << "\n";
    }
    os << "\n";
  };
  section("Time to kill (s, mean over wins) / win rate (ok = every fight won)", [&](const FightStats& r) {
    const int pct = std::min(99, static_cast<int>(std::lround(r.win_rate() * 100.0)));
    if (r.wins == 0) std::snprintf(buf, sizeof buf, "     -/ 0");
    else if (r.wins == r.fights) std::snprintf(buf, sizeof buf, " %5.1f/ok", r.ttk_seconds);
    else std::snprintf(buf, sizeof buf, " %5.1f/%2d", r.ttk_seconds, pct);
    return std::string(buf);
  });
  section("Health left after a win (%)", [&](const FightStats& r) {
    std::snprintf(buf, sizeof buf, "%9.0f", r.hp_left_pct);
    return std::string(buf);
  });
  section("Mana spent per kill (% of pool) / mob health per mana", [&](const FightStats& r) {
    if (r.damage_per_mana <= 0.0) return std::string("        -");
    std::snprintf(buf, sizeof buf, " %3.0f/%4.1f", r.mana_used_pct, r.damage_per_mana);
    return std::string(buf);
  });
  return os.str();
}

}  // namespace ef::sim
