#include "balance_sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include "game/groups.hpp"
#include "game/rules.hpp"
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

// A headless zone and its clock.
struct Arena {
  Arena(const Content& c, std::uint64_t seed) : content(c), zone(arena(), c, kDt, seed) {}
  const Content& content;
  Zone zone;
  NullSink sink;
  std::uint32_t t = 0;
  void tick() { zone.tick(++t, sink); }
  PlayerEntity& player(EntityId id) { return *zone.find(id); }
  void wait_ready(EntityId id, int limit) {
    for (int i = 0; i < limit && (player(id).cast.casting() || zone.now() < player(id).gcd_until); ++i) tick();
  }
};

// One autopiloted character: its kit and its Bard twist.
struct Pilot {
  EntityId id = 0;
  std::string class_id;
  Kit kit;
  bool healer = false;  // watches the whole party's health, not only its own
  std::size_t song_idx = 0;
  std::uint32_t song_switch = 0;
};

ZonePlayerInit sim_player(const Content& c, const ClassDef& cd, int level, const Kit& kit, std::int64_t char_id, bool gear) {
  ZonePlayerInit init;
  init.name = "Sim" + std::to_string(char_id);
  init.race = "highlander";
  init.class_id = cd.id;
  init.char_id = char_id;
  init.level = level;
  init.bind_zone = "sim_arena";
  for (const AbilityDef* a : c.spells_for(cd.id))
    if (a->level <= level) init.spellbook.push_back(a->id);
  init.gems = kit.gems;
  for (const auto& [sid, cap] : cd.skills) init.skills.push_back({sid, skill_cap(cap, level)});
  if (gear)
    if (const RaceDef* race = c.find_race(init.race))
      for (const auto& [slot, id] : recommended_kit(c, *race, cd, level)) {
        const ItemDef* it = c.find_item(id);
        init.items.emplace_back(slot, ItemStackState{id, it && it->kind == "ammo" ? std::min(it->stack, 50) : 1, 0});
      }
  return init;
}

Pilot make_pilot(Arena& a, const ClassDef& cd, int level, std::int64_t char_id, bool gear) {
  Pilot pl;
  pl.class_id = cd.id;
  pl.kit = build_kit(a.content, cd.id, level);
  pl.healer = cd.archetype == "healer" && pl.kit.heal >= 0;
  pl.id = a.zone.add_player(sim_player(a.content, cd, level, pl.kit, char_id, gear));
  return pl;
}

// Before a pull: summon the pet, then cast every pre-fight buff that is missing or about to fade.
void prepare(Arena& a, const Pilot& pl) {
  const Kit& kit = pl.kit;
  if (a.player(pl.id).dead) return;
  if (kit.pet >= 0 && (a.player(pl.id).pet == 0 || !a.zone.find_mob(a.player(pl.id).pet))) {
    a.wait_ready(pl.id, 200);
    a.zone.cast_spell(pl.id, kit.pet, pl.id, a.sink);
    a.wait_ready(pl.id, 400);
  }
  for (int g : kit.buffs) {
    const AbilityDef* ab = a.content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
    if (!ab) continue;
    a.wait_ready(pl.id, 200);
    const EntityId target = ab->target == TargetType::Pet ? a.player(pl.id).pet : pl.id;
    if (target == 0) continue;
    const MobEntity* pet = ab->target == TargetType::Pet ? a.zone.find_mob(target) : nullptr;
    const auto& have = pet ? pet->buffs : a.player(pl.id).buffs;
    if (std::any_of(have.begin(), have.end(), [&](const ActiveBuff& b) { return b.stack_group == ab->stack_group && b.expires_tick > a.zone.now() + 1200; }))
      continue;
    a.zone.cast_spell(pl.id, g, target, a.sink);
    a.wait_ready(pl.id, 400);
  }
}

float health_frac(const PlayerEntity& p) { return static_cast<float>(p.hp) / static_cast<float>(std::max(1, p.stats.max_hp)); }

// One tick of decisions for `pl` against mob `m`. `engaged` = may attack (a group waits for its tank
// to take the mob's attention first); healing is always allowed. `party` = everyone it may heal.
void pilot_act(Arena& a, Pilot& pl, EntityId m, const std::vector<EntityId>& party, bool engaged) {
  const Kit& kit = pl.kit;
  const Content& content = a.content;
  Zone& zone = a.zone;
  PlayerEntity& me = a.player(pl.id);
  if (me.dead) return;
  MobEntity* mob = zone.find_mob(m);
  if (!mob || !mob->alive()) return;
  const float d = horizontal_distance(me.pos, mob->pos);
  if (engaged) {
    if (me.target != m) {
      zone.set_target(pl.id, m, a.sink);
      if (me.pet != 0) zone.pet_command(pl.id, PetOrder::Attack, m, a.sink);
    }
    if (kit.melee && d > 2.5f) {  // run in at normal speed
      const float step = std::min(content.rules.movement.run_speed * kDt, d - 2.0f);
      me.pos.x += (mob->pos.x - me.pos.x) / d * step;
      me.pos.z += (mob->pos.z - me.pos.z) / d * step;
    }
    if (d <= 6.0f && !me.auto_attack) zone.set_auto_attack(pl.id, true, a.sink);
  }
  const float self_frac = health_frac(me);
  // Who needs healing: a healer watches everyone, anyone else only itself.
  EntityId hurt = pl.id;
  float hurt_frac = self_frac;
  if (pl.healer)
    for (EntityId id : party)
      if (const PlayerEntity* o = zone.find(id); o && !o->dead && health_frac(*o) < hurt_frac) {
        hurt = id;
        hurt_frac = health_frac(*o);
      }
  auto spell_ready = [&](int g) {
    if (g < 0) return false;
    const AbilityDef* ab = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
    return ab && me.mana >= ab->cost && zone.now() >= me.gem_ready[static_cast<std::size_t>(g)] && zone.now() >= cooldown_of(me, ab->id);
  };
  auto try_cast = [&](int g, EntityId target) {
    if (!spell_ready(g)) return false;
    zone.cast_spell(pl.id, g, target, a.sink);
    return true;
  };
  // Bards twist: switch songs every pulse.
  if (engaged && !kit.songs.empty() && zone.now() >= me.gcd_until && (!me.song.active() || zone.now() >= pl.song_switch)) {
    const int g = kit.songs[pl.song_idx % kit.songs.size()];
    const AbilityDef* ab = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
    if (ab && !(me.song.active() && me.song.ability == ab)) zone.cast_spell(pl.id, g, ab->beneficial() ? pl.id : m, a.sink);
    ++pl.song_idx;
    pl.song_switch = zone.now() + static_cast<std::uint32_t>(std::lround(content.rules.songs.pulse_seconds / kDt));
  }
  const bool free = !me.cast.casting() && zone.now() >= me.gcd_until;
  if (free) {
    bool acted = false;
    const float heal_at = pl.healer ? 0.6f : 0.45f;
    if (hurt_frac < heal_at) {
      const AbilityDef* heal = kit.heal >= 0 ? content.find_spell(kit.gems[static_cast<std::size_t>(kit.heal)]) : nullptr;
      if (heal && (hurt == pl.id || heal->target != TargetType::Self)) acted = try_cast(kit.heal, hurt);
      if (!acted && hurt == pl.id && engaged) acted = try_cast(kit.tap, m);
    }
    if (self_frac < 0.45f) {
      for (const AbilityDef* ab : kit.heal_abilities)
        if (!acted && zone.now() >= cooldown_of(me, ab->id)) {
          zone.use_ability(pl.id, ab->id, pl.id, a.sink);
          acted = true;
        }
      for (const AbilityDef* ab : kit.defense_discs)
        if (zone.now() >= cooldown_of(me, ab->id) && me.endurance >= ab->cost) zone.use_ability(pl.id, ab->id, pl.id, a.sink);
    }
    if (engaged) {
      if (!acted && kit.slow >= 0) {
        const AbilityDef* ab = content.find_spell(kit.gems[static_cast<std::size_t>(kit.slow)]);
        if (ab && !on_target(*mob, *ab, pl.id)) acted = try_cast(kit.slow, m);
      }
      for (int g : kit.dots) {
        if (acted) break;
        const AbilityDef* ab = content.find_spell(kit.gems[static_cast<std::size_t>(g)]);
        if (ab && !on_target(*mob, *ab, pl.id)) acted = try_cast(g, m);
      }
      if (!acted) acted = try_cast(kit.nuke, m);
      if (!acted && self_frac < 0.8f) try_cast(kit.tap, m);
    }
  }
  // Melee abilities and disciplines whenever they are ready.
  if (engaged && !me.cast.casting()) {
    for (const AbilityDef* ab : kit.offense_discs)
      if (zone.now() >= cooldown_of(me, ab->id) && me.endurance >= ab->cost) zone.use_ability(pl.id, ab->id, pl.id, a.sink);
    for (const AbilityDef* ab : kit.strikes) {
      if (zone.now() < cooldown_of(me, ab->id) || (ab->gcd && zone.now() < me.gcd_until)) continue;
      if (ab->resource == Resource::Endurance && me.endurance < ab->cost) continue;
      if (ab->resource == Resource::Mana && me.mana < ab->cost) continue;
      if (d > ab->range + 1.0f) continue;
      zone.use_ability(pl.id, ab->id, m, a.sink);
    }
  }
}

MobDef sparring_foe(const Content& content, int level, float hp_mult, float damage_mult) {
  const MobDef* base = content.find_mob("road_brigand");
  if (!base && !content.mobs.empty()) base = &content.mobs.front();
  MobDef foe = base ? *base : MobDef{};
  foe.id = "sim_foe";
  foe.name = "a sparring foe";
  foe.level = level;
  foe.hp_mult = hp_mult;
  foe.damage_mult = damage_mult;
  foe.aggro_radius = 0;
  foe.assist = false;
  foe.flee_hp_pct = 0;
  foe.loot = "";
  foe.wander_radius = 0;
  return foe;
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
  const ClassDef* cd = content.find_class(class_id);
  if (!cd || content.mobs.empty()) return st;
  double ttk = 0.0, hp_left = 0.0, mana_pct = 0.0, dpm = 0.0;
  int mana_fights = 0;
  // One character fights every bout in turn, resting a minute between them, so recast timers
  // (disciplines, once-an-hour abilities) carry over the way they do in play.
  Arena a(content, opt.seed * 7919u + static_cast<std::uint64_t>(level) * 131u);
  Pilot pl = make_pilot(a, *cd, level, 1, opt.gear);
  const EntityId p = pl.id;
  const std::vector<EntityId> party{p};
  for (int f = 0; f < opt.fights; ++f) {
    if (a.player(p).dead) a.zone.respawn(p, a.sink);
    if (f > 0)  // rest a minute between pulls
      for (int i = 0; i < static_cast<int>(60.0f / kDt); ++i) a.tick();
    prepare(a, pl);
    PlayerEntity& me = a.player(p);
    me.hp = me.stats.max_hp;
    me.mana = me.stats.max_mana;
    me.endurance = me.stats.max_endurance;

    const EntityId m = a.zone.spawn_mob(sparring_foe(content, level, opt.mob_hp_mult, opt.mob_damage_mult), {me.pos.x + 25.0f, 0, me.pos.z});
    a.tick();
    const int mob_hp = a.zone.find_mob(m)->stats.max_hp;
    const std::uint32_t start = a.t;
    int mana_prev = a.player(p).mana;
    double mana_spent = 0.0;
    bool won = false;
    const auto limit = static_cast<int>(opt.fight_seconds / kDt);
    for (int i = 0; i < limit; ++i) {
      if (a.player(p).dead) break;
      const MobEntity* mob = a.zone.find_mob(m);
      if (!mob || !mob->alive()) {
        won = true;
        break;
      }
      pilot_act(a, pl, m, party, true);
      a.tick();
      if (const PlayerEntity* now = a.zone.find(p)) {
        if (now->mana < mana_prev) mana_spent += mana_prev - now->mana;
        mana_prev = now->mana;
      }
    }
    ++st.fights;
    const PlayerEntity& after = a.player(p);
    if (won && !after.dead) {
      ++st.wins;
      ttk += static_cast<double>(a.t - start) * kDtD;
      hp_left += 100.0 * after.hp / std::max(1, after.stats.max_hp);
      if (after.stats.max_mana > 0) {
        mana_pct += 100.0 * mana_spent / after.stats.max_mana;
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

// ------------------------------------------------------------------ progression (Phase 8)

std::vector<std::string> standard_group(const Content& content, const std::string& class_id) {
  // A tank, a healer, a slower, a nuker and a melee damage dealer, plus the class being measured.
  static const char* const kCore[] = {"warrior", "cleric", "shaman", "wizard", "rogue", "ranger", "enchanter"};
  std::vector<std::string> out{class_id};
  for (const char* c : kCore) {
    if (out.size() >= 6) break;
    if (c != class_id && content.find_class(c)) out.emplace_back(c);
  }
  return out;
}

LevelSample simulate_level(const Content& content, const std::vector<std::string>& party, int level, const ProgressionOptions& opt) {
  LevelSample s;
  s.level = level;
  const bool group = party.size() > 1;
  std::uint64_t seed = opt.seed * 7919u + static_cast<std::uint64_t>(level) * 131u;
  for (const auto& c : party)
    for (char ch : c) seed = seed * 31u + static_cast<unsigned char>(ch);
  Arena a(content, seed);
  std::vector<Pilot> pilots;
  std::vector<EntityId> ids;
  for (std::size_t i = 0; i < party.size(); ++i) {
    const ClassDef* cd = content.find_class(party[i]);
    if (!cd) return s;
    pilots.push_back(make_pilot(a, *cd, level, static_cast<std::int64_t>(i) + 1, opt.gear));
    ids.push_back(pilots.back().id);
  }
  GroupRegistry reg(static_cast<int>(party.size()));
  if (group) {
    const PlayerEntity& lead = a.player(ids[0]);
    for (std::size_t i = 1; i < ids.size(); ++i) {
      const PlayerEntity& p = a.player(ids[i]);
      reg.invite(lead.char_id, lead.name, p.char_id, 1e18);
      reg.accept(p.char_id, p.name);
    }
    a.zone.set_groups(&reg);
  }
  // The tank: a Warrior if there is one, else the first plate wearer, else the measured class.
  std::size_t tank = 0;
  for (std::size_t i = 0; i < party.size(); ++i)
    if (party[i] == "warrior") {
      tank = i;
      break;
    }
  if (party[tank] != "warrior")
    for (std::size_t i = 0; i < party.size(); ++i)
      if (const ClassDef* cd = content.find_class(party[i]); cd && cd->archetype == "tank") {
        tank = i;
        break;
      }
  const int mob_level = std::min(content.rules.max_level + 5, level + (group ? opt.group_level_offset : 0));
  const MobDef foe = sparring_foe(content, mob_level, group ? opt.group_hp_mult : 1.0f, group ? opt.group_damage_mult : 1.0f);
  const auto engage_delay = static_cast<std::uint32_t>(std::lround(opt.assist_seconds / kDt));
  const auto limit = static_cast<std::uint32_t>(opt.fight_seconds / kDt);
  const auto rest_limit = static_cast<std::uint32_t>(opt.rest_cap_seconds / kDt);
  const std::uint64_t base_xp = total_xp_for_level(level, content.rules.xp);
  double seconds = 0.0, gained = 0.0, fight_s = 0.0, rest_s = 0.0;
  for (int f = 0; f < opt.fights; ++f) {
    bool died = false;
    for (EntityId id : ids)
      if (a.player(id).dead) {
        a.zone.respawn(id, a.sink);
        died = true;
      }
    if (died) seconds += static_cast<double>(opt.death_seconds);
    const std::uint32_t t0 = a.t;
    for (const Pilot& pl : pilots) prepare(a, pl);
    // Nobody levels during the sample: everyone starts each pull at the bottom of the level.
    for (EntityId id : ids) a.player(id).xp = base_xp;
    const PlayerEntity& lead = a.player(ids[tank]);
    const EntityId m = a.zone.spawn_mob(foe, {lead.pos.x + 25.0f, 0, lead.pos.z});
    a.tick();
    const std::uint32_t start = a.t;
    bool won = false;
    while (a.t - start < limit) {
      const MobEntity* mob = a.zone.find_mob(m);
      if (!mob || !mob->alive()) {
        won = true;
        break;
      }
      if (std::all_of(ids.begin(), ids.end(), [&](EntityId id) { return a.player(id).dead; })) break;
      const bool others = !group || a.t - start >= engage_delay || a.player(ids[tank]).dead;
      for (std::size_t i = 0; i < pilots.size(); ++i) pilot_act(a, pilots[i], m, ids, i == tank || others);
      a.tick();
    }
    if (!won)  // the foe may have died on the very last tick allowed
      if (const MobEntity* mob = a.zone.find_mob(m); !mob || !mob->alive()) won = true;
    fight_s += static_cast<double>(a.t - start) * kDtD;
    ++s.fights;
    if (won) ++s.wins;
    else
      a.zone.gm_kill_mob(m, a.sink);  // a wipe or a stalemate: the next pull starts fresh
    const PlayerEntity& me = a.player(ids[0]);
    gained += static_cast<double>(me.xp) - static_cast<double>(base_xp);
    if (me.dead) ++s.deaths;
    // Rest: everyone alive sits until health, mana and endurance are nearly full.
    const std::uint32_t rest0 = a.t;
    for (EntityId id : ids)
      if (!a.player(id).dead) a.zone.set_sitting(id, true, a.sink);
    auto rested = [&](const PlayerEntity& p) {
      return p.dead || (p.hp >= p.stats.max_hp * 95 / 100 && p.mana >= p.stats.max_mana * 95 / 100 &&
                        p.endurance >= p.stats.max_endurance * 95 / 100);
    };
    while (a.t - rest0 < rest_limit && !std::all_of(ids.begin(), ids.end(), [&](EntityId id) { return rested(a.player(id)); })) a.tick();
    for (EntityId id : ids)
      if (!a.player(id).dead) a.zone.set_sitting(id, false, a.sink);
    rest_s += static_cast<double>(a.t - rest0) * kDtD;
    seconds += static_cast<double>(a.t - t0) * kDtD + static_cast<double>(opt.pull_seconds);
  }
  if (s.fights > 0) {
    s.fight_seconds = fight_s / s.fights;
    s.rest_seconds = rest_s / s.fights;
  }
  s.xp_per_hour = seconds > 0.0 ? gained * 3600.0 / seconds : 0.0;
  s.seconds_per_level = s.xp_per_hour > 0.0 ? static_cast<double>(xp_to_next(level, content.rules.xp)) * 3600.0 / s.xp_per_hour : kNoProgress;
  return s;
}

Progression simulate_progression(const Content& content, const std::string& class_id, bool group, const ProgressionOptions& opt) {
  Progression out;
  out.class_id = class_id;
  out.group = group;
  const std::vector<std::string> party = group ? standard_group(content, class_id) : std::vector<std::string>{class_id};
  const int top = content.rules.max_level;  // levels 1 .. top-1 each take some time
  const int step = std::max(1, opt.step);
  std::map<int, LevelSample> sampled;
  for (int l = 1; l < top; l += step) sampled[l] = simulate_level(content, party, l, opt);
  if (!sampled.contains(top - 1)) sampled[top - 1] = simulate_level(content, party, top - 1, opt);
  // Levels between samples: log-linear interpolation of the time per level (it grows geometrically).
  for (int l = 1; l < top; ++l) {
    auto hi = sampled.lower_bound(l);
    if (hi->first == l) {
      out.levels.push_back(hi->second);
      continue;
    }
    auto lo = std::prev(hi);
    LevelSample s;
    s.level = l;
    const double f = static_cast<double>(l - lo->first) / static_cast<double>(hi->first - lo->first);
    const double a = std::min(lo->second.seconds_per_level, kNoProgress), b = std::min(hi->second.seconds_per_level, kNoProgress);
    s.seconds_per_level = std::exp(std::log(a) + f * (std::log(b) - std::log(a)));
    s.interpolated = true;
    out.levels.push_back(s);
  }
  for (const auto& s : out.levels) {
    if (!s.interpolated && s.seconds_per_level >= kNoProgress && out.stalled_level == 0) out.stalled_level = s.level;
    if (!s.interpolated) {
      out.fights += s.fights;
      out.wins += s.wins;
      out.deaths += s.deaths;
    }
    const double h = std::min(s.seconds_per_level, kNoProgress) / 3600.0;
    for (std::size_t b = 0; b < kBands.size(); ++b)
      if (s.level >= kBands[b].first && s.level < kBands[b].second) out.band_hours[b] += h;
    out.total_hours += h;
    (s.level < 50 ? out.hours_1_50 : out.hours_50_60) += h;
  }
  return out;
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

namespace {

double median_of(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t mid = v.size() / 2;
  return v.size() % 2 == 1 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

}  // namespace

std::vector<Outlier> progression_outliers(const std::vector<Progression>& rows, double factor, double max_death_rate) {
  std::vector<Outlier> out;
  std::vector<double> solo, group;
  for (const auto& r : rows)
    if (r.stalled_level == 0) (r.group ? group : solo).push_back(r.total_hours);  // stalled totals are not times
  const double med[2] = {median_of(solo), median_of(group)};
  char buf[200];
  for (const auto& r : rows) {
    const double m = med[r.group ? 1 : 0];
    const char* mode = r.group ? "grouped" : "solo";
    if (r.stalled_level != 0) {
      std::snprintf(buf, sizeof buf, "%s: earned no experience at level %d; never reaches the cap", mode, r.stalled_level);
      out.push_back({r.class_id, r.stalled_level, buf});
    } else if (m > 0.0 && r.total_hours > m * factor) {
      std::snprintf(buf, sizeof buf, "%s: %.0f h to the cap, %.2fx the median %.0f h", mode, r.total_hours, r.total_hours / m, m);
      out.push_back({r.class_id, 0, buf});
    } else if (m > 0.0 && r.total_hours * factor < m) {
      std::snprintf(buf, sizeof buf, "%s: %.0f h to the cap, %.2fx faster than the median %.0f h", mode, r.total_hours, m / r.total_hours, m);
      out.push_back({r.class_id, 0, buf});
    }
    if (r.fights > 0 && static_cast<double>(r.deaths) / r.fights > max_death_rate) {
      std::snprintf(buf, sizeof buf, "%s: died in %d of %d pulls", mode, r.deaths, r.fights);
      out.push_back({r.class_id, 0, buf});
    }
  }
  return out;
}

std::string format_progression(const std::vector<Progression>& rows, bool detail) {
  std::ostringstream os;
  char buf[256];
  for (const bool group : {false, true}) {
    std::vector<const Progression*> mine;
    for (const auto& r : rows)
      if (r.group == group) mine.push_back(&r);
    if (mine.empty()) continue;
    os << (group ? "Grouped (the class plus a standard five; yellow-con dungeon-strength mobs)\n" : "Solo (even-con mobs)\n");
    std::snprintf(buf, sizeof buf, "%-14s %7s %7s %7s %7s %8s %10s %7s %9s\n", "hours", "1-15", "15-30", "30-45", "45-60", "1-60", "50-60/1-50",
                  "vs med", "deaths");
    os << buf;
    std::vector<double> totals;
    totals.reserve(mine.size());
    for (const auto* r : mine)
      if (r->stalled_level == 0) totals.push_back(r->total_hours);
    const double med = median_of(totals);
    for (const auto* r : mine) {
      const double ratio = r->hours_1_50 > 0.0 ? r->hours_50_60 / r->hours_1_50 : 0.0;
      if (r->stalled_level != 0) {
        std::snprintf(buf, sizeof buf, "%-14s stalled: no experience at level %d (never reaches the cap)\n", r->class_id.c_str(), r->stalled_level);
        os << buf;
        continue;
      }
      std::snprintf(buf, sizeof buf, "%-14s %7.1f %7.1f %7.1f %7.1f %8.1f %10.2f %6.2fx %4d/%-4d\n", r->class_id.c_str(), r->band_hours[0],
                    r->band_hours[1], r->band_hours[2], r->band_hours[3], r->total_hours, ratio, med > 0.0 ? r->total_hours / med : 0.0, r->deaths,
                    r->fights);
      os << buf;
    }
    const double lo = totals.empty() ? 0.0 : *std::min_element(totals.begin(), totals.end());
    const double hi = totals.empty() ? 0.0 : *std::max_element(totals.begin(), totals.end());
    std::snprintf(buf, sizeof buf, "median %.1f h, fastest %.1f h, slowest %.1f h (spread %.2fx)\n\n", med, lo, hi, lo > 0.0 ? hi / lo : 0.0);
    os << buf;
    if (!detail) continue;
    os << "  per sampled level: minutes to the next level / mean fight s / mean rest s\n";
    for (const auto* r : mine) {
      os << "  " << r->class_id << ":";
      for (const auto& s : r->levels) {
        if (s.interpolated) continue;
        std::snprintf(buf, sizeof buf, " L%d %.0f/%.0f/%.0f", s.level, std::min(s.seconds_per_level, kNoProgress) / 60.0, s.fight_seconds,
                      s.rest_seconds);
        os << buf;
      }
      os << "\n";
    }
    os << "\n";
  }
  return os.str();
}

}  // namespace ef::sim
