// Phase 6c: dungeons, bosses and their mechanics (adds, enrage timers, area attacks that punish a crowd,
// fear, summoning), mob specials (procs, casts, enrage, rampage), named rares, and the named loot.
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "zone_support.hpp"

using namespace zt;

namespace {

// Keeps the test's players alive and on the boss's hate list while the boss fights.
struct Fight {
  Zone zone;
  RecordingSink sink;
  std::uint32_t t = 0;
  EntityId boss = 0;
  std::vector<EntityId> players;

  // The heroes are the boss's level, so its spells land as often as they would in play.
  Fight(const char* boss_id, std::vector<std::pair<float, float>> spots, std::uint64_t seed = 21) : zone(test_zone(), content(), kDt, seed) {
    for (std::size_t i = 0; i < spots.size(); ++i) {
      auto init = at(spots[i].first, spots[i].second, ("Hero" + std::string(1, static_cast<char>('A' + i))).c_str(), mob(boss_id).level);
      players.push_back(zone.add_player(init));
    }
    boss = zone.spawn_mob(mob(boss_id), {0, 0, 0});
    zone.tick(++t, sink);
    for (EntityId p : players) zone.find_mob(boss)->threat.add(p, 10.0);
  }
  MobEntity& b() { return *zone.find_mob(boss); }
  int T(float seconds) const { return static_cast<int>(zone.ticks(seconds)); }
  void run(int ticks_n) {
    for (int i = 0; i < ticks_n; ++i) {
      for (EntityId p : players)
        if (PlayerEntity* pl = zone.find(p)) {
          pl->hp = pl->stats.max_hp;  // the healers are very good
          pl->dead = false;
        }
      zone.tick(++t, sink);
    }
  }
  int adds() {
    int n = 0;
    zone.for_each_mob([&](const MobEntity& m) { n += (m.add_of == boss && m.alive()) ? 1 : 0; });
    return n;
  }
  int hits_on(EntityId p, const char* ability) {
    int n = 0;
    for (const auto& e : sink.all<proto::CombatEvent>(p))
      if (e.target_id == p && e.ability == ability) ++n;
    return n;
  }
};

const MobSpecial* special(const MobDef& m, MobSpecialType t) {
  for (const auto& s : m.specials)
    if (s.type == t) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("every dungeon: one boss with a mechanic in the boss room, named rares, and the named loot", "[dungeons][content]") {
  const Content& c = content();
  int dungeons = 0;
  std::map<int, int> per_band;
  for (const auto& z : c.zones) {
    if (!z.has_dungeon) continue;
    ++dungeons;
    INFO(z.id);
    for (std::size_t b = 0; b < kLevelBands.size(); ++b)
      if (z.level_min >= kLevelBands[b].first && (z.level_min < kLevelBands[b].second || b + 1 == kLevelBands.size())) {
        ++per_band[static_cast<int>(b)];
        break;
      }
    std::vector<const MobDef*> bosses;
    std::set<std::string> rares;
    for (const auto& sp : z.spawns) {
      const MobDef* m = c.find_mob(sp.mob);
      REQUIRE(m);
      if (m->boss) bosses.push_back(m);
      if (!sp.rare.empty()) rares.insert(sp.rare);
      CHECK_FALSE(CollisionMap({}, {}, z.layout->tiles).overlaps(sp.x, sp.z, 0.5f));
    }
    REQUIRE(bosses.size() == 1);
    const MobDef& boss = *bosses.front();
    CHECK(boss.named);
    CHECK(boss.level <= z.level_max);
    CHECK(boss.level >= z.level_min);
    const bool mechanic = special(boss, MobSpecialType::Adds) || special(boss, MobSpecialType::EnrageTimer) ||
                          special(boss, MobSpecialType::Aoe) || special(boss, MobSpecialType::Summon);
    CHECK(mechanic);
    CHECK_FALSE(rares.empty());
    // The boss carries the dungeon's named items.
    const LootTable* lt = c.find_loot(boss.loot);
    REQUIRE(lt);
    int named = 0;
    for (const auto& d : lt->drops)
      if (const ItemDef* it = c.find_item(d.item); it && it->source == "dungeon:" + z.id) ++named;
    CHECK(named >= 6);
  }
  CHECK(dungeons >= 8);
  CHECK(per_band[0] >= 2);
  CHECK(per_band[1] >= 2);
  CHECK(per_band[2] >= 2);
  CHECK(per_band[3] >= 2);
  // No named item is left without a source of drops.
  std::set<std::string> dropped;
  for (const auto& t : c.loot_tables)
    for (const auto& d : t.drops) dropped.insert(d.item);
  for (const auto& it : c.items)
    if (it.source.rfind("dungeon:", 0) == 0) CHECK(dropped.contains(it.id));
}

TEST_CASE("boss adds: helpers join at each health threshold and leave when the boss resets", "[dungeons][specials]") {
  Fight f("the_rattle_king", {{2, 0}});
  f.run(10);
  REQUIRE(f.b().state == AiState::Aggro);
  CHECK(f.adds() == 0);
  f.b().hp = f.b().stats.max_hp * 60 / 100;
  f.run(2);
  CHECK(f.adds() == 2);
  CHECK(f.sink.chat_count(f.players[0], "Rise, my subjects") == 1);
  f.run(5);
  CHECK(f.adds() == 2);  // each threshold once
  f.b().hp = f.b().stats.max_hp * 30 / 100;
  f.run(2);
  CHECK(f.adds() == 4);
  // The adds fight too.
  int engaged = 0;
  f.zone.for_each_mob([&](const MobEntity& m) { engaged += (m.add_of == f.boss && m.threat.contains(f.players[0])) ? 1 : 0; });
  CHECK(engaged == 4);
  // Everyone runs: the boss leashes, its adds are gone, and the next fight starts fresh.
  f.zone.find(f.players[0])->pos = {450, 0, 450};
  for (int i = 0; i < 2000 && f.b().state != AiState::Leash; ++i) f.run(1);
  REQUIRE(f.b().state == AiState::Leash);
  f.run(2);
  CHECK(f.adds() == 0);
}

TEST_CASE("enrage timers and low-health enrage raise a mob's damage", "[dungeons][specials]") {
  Fight f("the_cinder_foreman", {{2, 0}});
  const MobSpecial* timer = special(mob("the_cinder_foreman"), MobSpecialType::EnrageTimer);
  REQUIRE(timer);
  f.run(20);
  CHECK(f.b().enrage_pct == 0.0f);
  f.run(f.T(timer->seconds));
  CHECK(f.b().enrage_pct >= timer->value);
  CHECK(f.sink.chat_count(f.players[0], "Back to work") == 1);

  Fight g("the_storm_herald", {{2, 0}});
  g.run(10);
  CHECK(g.b().enrage_pct == 0.0f);
  g.b().hp = g.b().stats.max_hp / 10;
  g.run(2);
  CHECK(g.b().enraged);
  CHECK(g.b().enrage_pct > 0.0f);
}

TEST_CASE("a spread-out area attack hits everyone near the target and no one far away", "[dungeons][specials]") {
  // Mirefang sprays venom around its target: the two players standing together are both hit.
  Fight f("mirefang", {{2, 0}, {3.5f, 1}, {2, 25}});
  const MobSpecial* spray = special(mob("mirefang"), MobSpecialType::Aoe);
  REQUIRE(spray);
  CHECK_FALSE(spray->on_self);
  f.zone.find_mob(f.boss)->threat.add(f.players[0], 1000.0);  // the tank holds it
  const AbilityDef* a = content().find_mob_spell(spray->effect);
  REQUIRE(a);
  f.run(f.T(spray->every * 3.0f) + 10);
  const int tank = f.hits_on(f.players[0], a->name.c_str());
  const int next_to_tank = f.hits_on(f.players[1], a->name.c_str());
  const int far = f.hits_on(f.players[2], a->name.c_str());
  CHECK(tank >= 2);
  CHECK(next_to_tank >= 1);  // landed or resisted, it reached them
  CHECK(far == 0);
}

TEST_CASE("a boss's dread pulse frightens everyone close to it", "[dungeons][specials]") {
  Fight f("the_pale_abbot", {{2, 0}, {30, 0}});
  const MobSpecial* pulse = special(mob("the_pale_abbot"), MobSpecialType::Aoe);
  REQUIRE(pulse);
  CHECK(pulse->on_self);
  f.zone.find_mob(f.boss)->threat.add(f.players[0], 1000.0);
  bool feared = false;
  for (int i = 0; i < f.T(pulse->every * 4.0f) && !feared; ++i) {
    f.run(1);
    feared = (control_bits(f.zone.find(f.players[0])->buffs) & kControlFear) != 0;
  }
  CHECK(feared);  // (a resist can save you once; four pulses never all miss with this seed)
  CHECK((control_bits(f.zone.find(f.players[1])->buffs) & kControlFear) == 0);
}

TEST_CASE("a summoning boss pulls a target that runs too far back to it", "[dungeons][specials]") {
  Fight f("admiral_grell", {{2, 0}});
  const MobSpecial* summon = special(mob("admiral_grell"), MobSpecialType::Summon);
  REQUIRE(summon);
  f.run(10);
  // The target runs, and the boss is rooted so it cannot follow.
  AbilityDef root;
  root.id = root.name = root.stack_group = "test_root";
  root.target = TargetType::Enemy;
  EffectDef held;
  held.type = EffectType::Root;
  held.duration = 600;
  root.effects.push_back(held);
  f.zone.force_effect(f.boss, root, 60, f.sink);
  f.zone.find(f.players[0])->pos = {summon->range + 15.0f, 0, 0};
  bool pulled = false;
  for (int i = 0; i < f.T(summon->every * 2.0f) && !pulled; ++i) {
    f.run(1);
    pulled = horizontal_distance(f.zone.find(f.players[0])->pos, f.b().pos) < 4.0f;
  }
  CHECK(pulled);
  CHECK(f.sink.chat_count(f.players[0], "None leave my ship") >= 1);
}

TEST_CASE("procs cast through the spell engine: a venomous bite poisons", "[dungeons][specials]") {
  Fight f("venomfang_cobra", {{1.5f, 0}});
  const MobSpecial* proc = special(mob("venomfang_cobra"), MobSpecialType::Proc);
  REQUIRE(proc);
  const AbilityDef* venom = content().find_mob_spell(proc->effect);
  REQUIRE(venom);
  bool poisoned = false;
  for (int i = 0; i < 20 * 120 && !poisoned; ++i) {
    f.run(1);
    for (const auto& b : f.zone.find(f.players[0])->buffs) poisoned = poisoned || b.ability_id == venom->id;
  }
  CHECK(poisoned);
  int ticks_seen = 0;
  for (const auto& e : f.sink.all<proto::CombatEvent>(f.players[0]))
    if (e.kind == static_cast<std::uint8_t>(CombatKind::DotTick) && e.ability == venom->name) ++ticks_seen;
  f.run(f.T(6.0f));
  for (const auto& e : f.sink.all<proto::CombatEvent>(f.players[0]))
    if (e.kind == static_cast<std::uint8_t>(CombatKind::DotTick) && e.ability == venom->name) --ticks_seen;
  CHECK(ticks_seen < 0);  // it keeps ticking
}

TEST_CASE("named rares take their trash type's place, one at a time", "[dungeons][spawns]") {
  ZoneDef z = test_zone();
  MobSpawn sp;
  sp.mob = "warren_rat";
  sp.x = 100;
  sp.z = 100;
  sp.count = 4;
  sp.radius = 10;
  sp.rare = "gnawbone";
  sp.rare_chance = 1.0f;
  z.spawns.push_back(sp);
  Zone zone(z, content(), kDt, 5);
  int rare = 0, plain = 0;
  zone.for_each_mob([&](const MobEntity& m) {
    if (m.def->id == "gnawbone") ++rare;
    if (m.def->id == "warren_rat") ++plain;
  });
  CHECK(rare == 1);
  CHECK(plain == 3);
}
