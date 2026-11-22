// Zone-level spell engine: casting, interrupts, ticking effects, stacking, crowd control, healer
// aggro, resurrection, scrolls and trainers, saved state, and a Cleric/Wizard balance check.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zone_support.hpp"

using namespace zt;

namespace {

template <class F>
int run_until(Zone& z, RecordingSink& sink, std::uint32_t& t, int limit, F done) {
  for (int i = 0; i < limit; ++i) {
    if (done()) return i;
    z.tick(++t, sink);
  }
  return limit;
}

void run(Zone& z, RecordingSink& sink, std::uint32_t& t, int n) {
  for (int i = 0; i < n; ++i) z.tick(++t, sink);
}

int events(const RecordingSink& s, EntityId to, CombatKind kind) {
  int n = 0;
  for (const auto& e : s.all<proto::CombatEvent>(to))
    if (e.kind == static_cast<std::uint8_t>(kind)) ++n;
  return n;
}

// A sturdy, nearly harmless target that never resists more than the floor.
MobDef dummy(int level, float hp_mult = 50.0f) {
  MobDef m = mob("road_brigand");
  m.id = "test_dummy";
  m.name = "a practice dummy";
  m.level = level;
  m.hp_mult = hp_mult;
  m.damage_mult = 0.01f;
  m.aggro_radius = 0;
  m.assist = false;
  m.flee_hp_pct = 0;
  m.wander_radius = 0;
  m.loot = "";
  m.resists = {0, 0, 0, 0, 0, 0};
  return m;
}

// A synthetic detrimental spell with one lasting effect (for force_effect).
AbilityDef lasting(const char* id, EffectType type, float duration, float value = 0.0f, BuffStat stat = BuffStat::Ac, float base = 0.0f,
                   float tick = 0.0f) {
  AbilityDef a;
  a.id = id;
  a.name = id;
  a.stack_group = id;
  a.spell = true;
  a.target = TargetType::Enemy;
  EffectDef e;
  e.type = type;
  e.duration = duration;
  e.value = value;
  e.stat = type == EffectType::Slow ? BuffStat::AttackSpeedPct : type == EffectType::Snare ? BuffStat::MoveSpeedPct : stat;
  e.base = base;
  e.tick = tick;
  a.effects.push_back(e);
  return a;
}

// Waits out the GCD, casts from a gem, and waits for the cast to end. Returns the CastEnd result,
// 0 for an instant spell that succeeded, or -1 if the cast was refused.
int cast_and_wait(Zone& z, RecordingSink& s, std::uint32_t& t, EntityId p, int gem, EntityId target) {
  auto ready = [&] {
    const PlayerEntity& me = *z.find(p);
    if (me.cast.casting() || z.now() < me.gcd_until) return false;
    if (gem < 0 || gem >= static_cast<int>(kMaxGems)) return true;
    for (const auto& [id, at] : me.cooldowns)
      if (id == me.gems[static_cast<std::size_t>(gem)] && z.now() < at) return false;
    return true;
  };
  run_until(z, s, t, 20 * 60, ready);
  const auto ends = s.all<proto::CastEnd>(p).size();
  const auto oks = s.all<proto::AbilityResult>(p).size();
  z.cast_spell(p, gem, target, s);
  if (!z.find(p)->cast.casting()) {
    const auto r = s.all<proto::AbilityResult>(p);
    return r.size() > oks && r.back().ok ? 0 : -1;
  }
  run_until(z, s, t, 20 * 30, [&] { return !z.find(p) || !z.find(p)->cast.casting(); });
  const auto all = s.all<proto::CastEnd>(p);
  return all.size() > ends ? all.back().result : -1;
}

bool has_buff(const std::vector<ActiveBuff>& b, std::string_view id) {
  return std::any_of(b.begin(), b.end(), [&](const ActiveBuff& x) { return x.ability_id == id; });
}

constexpr int kDone = static_cast<int>(CastResult::Completed);
constexpr int kInterrupted = static_cast<int>(CastResult::Interrupted);

}  // namespace

TEST_CASE("a wizard casts a fire nuke: cast bar, mana at completion, recast, GCD", "[spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 21);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Pyra", 10), "wizard"), {"ember_dart", "flame_lash"}));
  const auto m = zone.spawn_mob(dummy(10), {20, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  const int mana0 = zone.find(p)->mana;
  const int hp0 = zone.find_mob(m)->hp;

  zone.cast_spell(p, 1, 0, sink);
  REQUIRE(zone.find(p)->cast.casting());
  auto start = sink.last<proto::CastStart>(p);
  REQUIRE(start);
  CHECK(start->spell_id == "flame_lash");
  CHECK(start->cast_ms == 1950);  // rank 2: 1.75 s + 0.2 s
  CHECK(start->school == static_cast<std::uint8_t>(ResistType::Fire));
  CHECK(zone.find(p)->mana == mana0);  // paid when the spell lands
  // Another cast while casting is refused.
  zone.cast_spell(p, 0, m, sink);
  const auto busy = sink.last<proto::AbilityResult>(p);
  REQUIRE(busy);
  CHECK(busy->message == "You are already casting a spell.");
  run_until(zone, sink, t, 100, [&] { return !zone.find(p)->cast.casting(); });
  auto end = sink.last<proto::CastEnd>(p);
  REQUIRE(end);
  CHECK(end->result == kDone);
  CHECK(zone.find(p)->mana == mana0 - spell("flame_lash").cost);
  CHECK(events(sink, p, CombatKind::SpellHit) + events(sink, p, CombatKind::Resist) == 1);
  if (events(sink, p, CombatKind::SpellHit) == 1) CHECK(zone.find_mob(m)->hp < hp0);
  auto ok = sink.last<proto::AbilityResult>(p);
  CHECK(ok->ok);
  CHECK(ok->gcd_ms == 1500);
  // The GCD blocks the next spell for a moment.
  zone.cast_spell(p, 0, m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message == "You must wait a moment.");
  // Out of mana.
  zone.find(p)->mana = 0;
  run(zone, sink, t, 40);
  zone.find(p)->mana = 0;
  zone.cast_spell(p, 0, m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message.find("Insufficient mana") != std::string::npos);
  // An empty or locked gem.
  zone.cast_spell(p, 2, m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message == "You have no spell memorized there.");
  zone.cast_spell(p, 7, m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message == "You have no spell memorized there.");
}

TEST_CASE("moving interrupts a cast and keeps the mana; stopping cancels", "[spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 22);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Mover", 12), "wizard"), {"arcane_spark"}));
  const auto m = zone.spawn_mob(dummy(12), {20, 0, 0});
  zone.tick(++t, sink);
  const int mana0 = zone.find(p)->mana;
  zone.cast_spell(p, 0, m, sink);
  REQUIRE(zone.find(p)->cast.casting());
  run(zone, sink, t, 10);
  zone.find(p)->pos.x += 2.0f;
  zone.tick(++t, sink);
  CHECK_FALSE(zone.find(p)->cast.casting());
  CHECK(sink.last<proto::CastEnd>(p)->result == kInterrupted);
  CHECK(zone.find(p)->mana == mana0);
  zone.cast_spell(p, 0, m, sink);
  REQUIRE(zone.find(p)->cast.casting());
  zone.stop_cast(p, sink);
  CHECK_FALSE(zone.find(p)->cast.casting());
  CHECK(sink.last<proto::CastEnd>(p)->result == static_cast<int>(CastResult::Cancelled));
}

TEST_CASE("a hit while casting needs a concentration check", "[spells][zone]") {
  // A one-second DoT on the caster guarantees a hit during a four-second cast.
  const AbilityDef pain = [] {
    AbilityDef a = lasting("test_pain", EffectType::Dot, 20, 0, BuffStat::Ac, 2, 1);
    return a;
  }();
  for (const bool keep : {false, true}) {
    Content c = content();
    c.rules.spells.min_concentration = c.rules.spells.max_concentration = keep ? 1.0f : 0.0f;
    Zone zone(test_zone(), c, kDt, 23);
    RecordingSink sink;
    std::uint32_t t = 0;
    const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Focus", 12), "wizard"), {"arcane_spark"}));
    const auto m = zone.spawn_mob(dummy(12), {20, 0, 0});
    zone.tick(++t, sink);
    REQUIRE(zone.force_effect(p, pain, 10, sink) == StackAction::Add);
    const int result = cast_and_wait(zone, sink, t, p, 0, m);
    CHECK(events(sink, p, CombatKind::DotTick) >= 1);
    CHECK(result == (keep ? kDone : kInterrupted));
    CHECK(sink.chat_count(p, "You keep your concentration") == (keep ? events(sink, p, CombatKind::DotTick) : 0));
  }
}

TEST_CASE("a DoT ticks for its whole duration, shows on the target, then fades", "[spells][zone][dot]") {
  Zone zone(test_zone(), content(), kDt, 24);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Brander", 10), "wizard"), {"smoldering_brand"}));
  const auto m = zone.spawn_mob(dummy(10), {30, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  // Root the dummy so it stays put (and cannot interrupt anything).
  zone.force_effect(m, lasting("test_root", EffectType::Root, 60), 60, sink);
  int result = -1;
  for (int tries = 0; tries < 6 && events(sink, p, CombatKind::Debuff) == 0; ++tries) result = cast_and_wait(zone, sink, t, p, 0, m);
  REQUIRE(result == kDone);
  REQUIRE(events(sink, p, CombatKind::Debuff) == 1);
  REQUIRE(has_buff(zone.find_mob(m)->buffs, "smoldering_brand"));
  zone.tick(++t, sink);
  auto list = sink.last<proto::BuffList>(p);
  REQUIRE(list);
  CHECK(list->entity_id == m);
  CHECK(std::any_of(list->buffs.begin(), list->buffs.end(), [](const auto& b) { return b.spell_id == "smoldering_brand" && !b.beneficial; }));
  const int hp_before = zone.find_mob(m)->hp;
  const int ticks_before = events(sink, p, CombatKind::DotTick);
  sink.clear();  // only lists sent after this point count below
  run(zone, sink, t, 20 * 19);
  const int ticks = events(sink, p, CombatKind::DotTick);
  (void)ticks_before;
  CHECK(ticks == 6);  // 18 s / 3 s
  const int per = effect_amount(spell("smoldering_brand").effects[0], 10);
  CHECK(hp_before - zone.find_mob(m)->hp == per * ticks);
  CHECK_FALSE(has_buff(zone.find_mob(m)->buffs, "smoldering_brand"));
  CHECK(events(sink, p, CombatKind::BuffFade) >= 1);
  // The target's own list (not a self list) must have been resent without the DoT.
  std::optional<proto::BuffList> after;
  for (const auto& l : sink.all<proto::BuffList>(p))
    if (l.entity_id == m) after = l;
  REQUIRE(after);
  CHECK(std::none_of(after->buffs.begin(), after->buffs.end(), [](const auto& b) { return b.spell_id == "smoldering_brand"; }));
}

TEST_CASE("a HoT heals every tick; buffs change stats and revert when they fade", "[spells][zone][hot]") {
  Zone zone(test_zone(), content(), kDt, 25);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Mender", 15), "cleric"), {"soothing_glow", "sacred_armor", "blessed_ward"}));
  zone.tick(++t, sink);
  PlayerEntity& me = *zone.find(p);
  me.hp = 10;
  REQUIRE(cast_and_wait(zone, sink, t, p, 0, p) == kDone);
  CHECK(events(sink, p, CombatKind::Buff) == 1);
  run(zone, sink, t, 20 * 25);
  CHECK(events(sink, p, CombatKind::HotTick) == 8);  // 24 s / 3 s
  CHECK(zone.find(p)->hp > 10);
  CHECK_FALSE(has_buff(zone.find(p)->buffs, "soothing_glow"));

  const int ac0 = zone.find(p)->stats.ac;
  const int hp0 = zone.find(p)->stats.max_hp;
  REQUIRE(cast_and_wait(zone, sink, t, p, 1, p) == kDone);
  CHECK(zone.find(p)->stats.ac == ac0 + 9);
  CHECK(zone.find(p)->stats.max_hp == hp0 + 35);
  // A lower rank of the same line cannot overwrite it.
  REQUIRE(cast_and_wait(zone, sink, t, p, 2, p) == kDone);
  CHECK(sink.chat_count(p, "would not take hold") == 1);
  CHECK(has_buff(zone.find(p)->buffs, "sacred_armor"));
  CHECK_FALSE(has_buff(zone.find(p)->buffs, "blessed_ward"));
  auto self = sink.last<proto::BuffList>(p);
  REQUIRE(self);
  CHECK(self->entity_id == p);
  // A short buff reverts the stats when it fades.
  AbilityDef brief = lasting("test_brief", EffectType::Buff, 2, 50, BuffStat::Ac);
  brief.target = TargetType::Self;
  REQUIRE(zone.force_effect(p, brief, 10, sink) == StackAction::Add);
  CHECK(zone.find(p)->stats.ac == ac0 + 9 + 50);
  run(zone, sink, t, 20 * 3);
  CHECK(zone.find(p)->stats.ac == ac0 + 9);
}

TEST_CASE("mez holds a mob until damage breaks it", "[spells][zone][cc]") {
  Zone zone(test_zone(), content(), kDt, 26);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Charmer", 10), "wizard"), {"ember_dart"}));
  MobDef angry = dummy(10);
  angry.aggro_radius = 30;
  const auto m = zone.spawn_mob(angry, {20, 0, 0});
  REQUIRE(zone.force_effect(m, lasting("test_mez", EffectType::Mez, 30), 30, sink) == StackAction::Add);
  run(zone, sink, t, 40);
  // A mesmerized mob does not even notice the player in its aggro radius.
  CHECK(zone.find_mob(m)->state == AiState::Idle);
  CHECK(zone.find_mob(m)->pos.x == 20.0f);
  auto has_mob = [&] {
    const auto& es = sink.last_snapshot[p].entities;
    return std::any_of(es.begin(), es.end(), [&](const auto& e) { return e.entity_id == m; });
  };
  run_until(zone, sink, t, 25, has_mob);  // the next heartbeat carries it
  auto snap = sink.last_snapshot[p];
  const auto it = std::find_if(snap.entities.begin(), snap.entities.end(), [&](const auto& e) { return e.entity_id == m; });
  REQUIRE(it != snap.entities.end());
  CHECK((it->flags & kFlagMezzed) != 0);
  int tries = 0;
  while (events(sink, p, CombatKind::MezBreak) == 0 && tries++ < 6) cast_and_wait(zone, sink, t, p, 0, m);
  CHECK(events(sink, p, CombatKind::MezBreak) == 1);
  CHECK_FALSE(has_buff(zone.find_mob(m)->buffs, "test_mez"));
  run(zone, sink, t, 40);
  CHECK(zone.find_mob(m)->pos.x < 20.0f);  // awake and coming for the caster
}

TEST_CASE("root, snare, slow, and fear change how a mob moves and fights", "[spells][zone][cc]") {
  // Each section: a warrior engages a dummy (it chases or swings), then an effect lands.
  auto engaged = [](Zone& zone, RecordingSink& sink, std::uint32_t& t, float distance) {
    const auto p = zone.add_player(at(0, 0, "Bait", 60));
    const auto m = zone.spawn_mob(dummy(1, 500.0f), {distance, 0, 0});
    zone.tick(++t, sink);
    zone.set_target(p, m, sink);
    zone.find_mob(m)->threat.add(p, 10.0);  // the mob now wants the warrior
    zone.tick(++t, sink);
    return std::pair{p, m};
  };
  SECTION("root: the mob stays put but keeps its target") {
    Zone zone(test_zone(), content(), kDt, 27);
    RecordingSink sink;
    std::uint32_t t = 0;
    const auto [p, m] = engaged(zone, sink, t, 15);
    zone.force_effect(m, lasting("test_root", EffectType::Root, 20), 30, sink);
    const float x = zone.find_mob(m)->pos.x;
    run(zone, sink, t, 20 * 5);
    CHECK(zone.find_mob(m)->pos.x == x);
    CHECK(zone.find_mob(m)->state == AiState::Chase);
    run(zone, sink, t, 20 * 16);
    CHECK(zone.find_mob(m)->pos.x < x);  // root expired: it closes in
  }
  SECTION("snare: the mob moves at a fraction of its speed") {
    Zone zone(test_zone(), content(), kDt, 28);
    RecordingSink sink;
    std::uint32_t t = 0;
    const auto [p, m] = engaged(zone, sink, t, 40);
    float x = zone.find_mob(m)->pos.x;
    run(zone, sink, t, 20);
    const float free_step = x - zone.find_mob(m)->pos.x;
    zone.force_effect(m, lasting("test_snare", EffectType::Snare, 20, 50), 30, sink);
    x = zone.find_mob(m)->pos.x;
    run(zone, sink, t, 20);
    const float snared_step = x - zone.find_mob(m)->pos.x;
    CHECK(free_step > 4.0f);
    CHECK(snared_step == Catch::Approx(free_step * 0.5f).margin(0.2f));
  }
  SECTION("slow: longer delays between swings") {
    auto swings = [&](bool slowed) {
      Zone zone(test_zone(), content(), kDt, 29);
      RecordingSink sink;
      std::uint32_t t = 0;
      const auto [p, m] = engaged(zone, sink, t, 2);
      if (slowed) zone.force_effect(m, lasting("test_slow", EffectType::Slow, 120, 50), 30, sink);
      sink.clear();
      run(zone, sink, t, 20 * 60);
      int n = 0;
      for (const auto& e : sink.all<proto::CombatEvent>(p))
        if (e.source_id == m && e.kind <= static_cast<std::uint8_t>(CombatKind::Crit)) ++n;
      return n;
    };
    const int normal = swings(false);
    const int slowed = swings(true);
    CHECK(normal >= 18);
    CHECK(slowed == Catch::Approx(normal / 2.0).margin(3));
  }
  SECTION("fear: the mob runs from its caster and stops attacking") {
    Zone zone(test_zone(), content(), kDt, 30);
    RecordingSink sink;
    std::uint32_t t = 0;
    const auto [p, m] = engaged(zone, sink, t, 2);
    run(zone, sink, t, 40);
    zone.force_effect(m, lasting("test_fear", EffectType::Fear, 6), 30, sink);
    sink.clear();
    run(zone, sink, t, 20 * 4);
    CHECK(horizontal_distance(zone.find_mob(m)->pos, zone.find(p)->pos) > 10.0f);
    int swings = 0;
    for (const auto& e : sink.all<proto::CombatEvent>(p))
      if (e.source_id == m && e.kind <= static_cast<std::uint8_t>(CombatKind::Crit)) ++swings;
    CHECK(swings == 0);
  }
}

TEST_CASE("debuffs lower a mob's defenses; a cure strips harmful effects from a friend", "[spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 31);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto w = zone.add_player(with_spells(as_class(at(0, 0, "Unraveler", 20), "wizard"), {"arcane_frailty"}));
  const auto c = zone.add_player(with_spells(as_class(at(3, 0, "Purifier", 15), "cleric"), {"purifying_touch"}));
  const auto m = zone.spawn_mob(dummy(20), {20, 0, 0});
  zone.tick(++t, sink);
  for (int i = 0; i < 6 && !has_buff(zone.find_mob(m)->buffs, "arcane_frailty"); ++i) cast_and_wait(zone, sink, t, w, 0, m);
  REQUIRE(has_buff(zone.find_mob(m)->buffs, "arcane_frailty"));
  CHECK(stat_total(zone.find_mob(m)->buffs, BuffStat::ResistAll) == -20.0f);
  CHECK(stat_total(zone.find_mob(m)->buffs, BuffStat::Ac) == -15.0f);

  REQUIRE(zone.force_effect(w, lasting("test_snare", EffectType::Snare, 60, 40), 20, sink) == StackAction::Add);
  CHECK(move_speed_multiplier(zone.find(w)->buffs) < 1.0f);
  REQUIRE(cast_and_wait(zone, sink, t, c, 0, w) == kDone);
  CHECK_FALSE(has_buff(zone.find(w)->buffs, "test_snare"));
  CHECK(events(sink, c, CombatKind::Dispel) == 1);
}

TEST_CASE("healing and buffing someone in a fight angers only what they are fighting", "[spells][zone][threat]") {
  Zone zone(test_zone(), content(), kDt, 32);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto tank = zone.add_player(at(0, 0, "Tank", 30));
  const auto healer = zone.add_player(with_spells(as_class(at(10, 0, "Healer", 30), "cleric"), {"greater_restoration", "hallowed_bulwark"}));
  const auto far_healer = zone.add_player(with_spells(as_class(at(-58, 0, "Distant", 30), "cleric"), {"greater_restoration"}));
  const auto fight = zone.spawn_mob(dummy(30), {3, 0, 0});
  const auto idle = zone.spawn_mob(dummy(30), {8, 0, 8});
  zone.tick(++t, sink);
  zone.use_ability(tank, "iron_challenge", fight, sink);
  zone.tick(++t, sink);
  REQUIRE(zone.find_mob(fight)->threat.contains(tank));

  // Healing a full-health tank heals nothing and angers no one.
  REQUIRE(cast_and_wait(zone, sink, t, healer, 0, tank) == kDone);
  CHECK_FALSE(zone.find_mob(fight)->threat.contains(healer));
  zone.find(tank)->hp = 50;
  REQUIRE(cast_and_wait(zone, sink, t, healer, 0, tank) == kDone);
  CHECK(zone.find_mob(fight)->threat.contains(healer));
  CHECK_FALSE(zone.find_mob(idle)->threat.contains(healer));
  CHECK(zone.find_mob(idle)->state == AiState::Idle);
  const double before = zone.find_mob(fight)->threat.threat_of(healer);
  REQUIRE(cast_and_wait(zone, sink, t, healer, 1, tank) == kDone);  // a buff on someone in the fight
  CHECK(zone.find_mob(fight)->threat.threat_of(healer) > before);
  // A healer beyond heal_aggro_radius goes unnoticed (the heal still lands).
  zone.find(tank)->hp = 50;
  REQUIRE(cast_and_wait(zone, sink, t, far_healer, 0, tank) == kDone);
  CHECK(zone.find(tank)->hp > 50);
  CHECK_FALSE(zone.find_mob(fight)->threat.contains(far_healer));
}

TEST_CASE("resurrection: the owner accepts, returns to the corpse, and regains XP once", "[spells][zone][rez]") {
  Zone zone(test_zone(), content(), kDt, 33);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto victim = zone.add_player(at(5, 5, "Fallen", 12));
  const auto cleric = zone.add_player(with_spells(as_class(at(0, 0, "Saint", 20), "cleric"), {"rite_of_return"}));
  zone.tick(++t, sink);
  const auto xp_before = zone.find(victim)->xp;
  zone.force_effect(victim, lasting("test_doom", EffectType::Dot, 3, 0, BuffStat::Ac, 100000, 1), 1, sink);
  run_until(zone, sink, t, 100, [&] { return zone.find(victim)->dead; });
  REQUIRE(zone.find(victim)->dead);
  const auto xp_dead = zone.find(victim)->xp;
  const auto lost = xp_before - xp_dead;
  REQUIRE(lost > 0);
  const auto died = sink.last<proto::PlayerDied>(victim);
  REQUIRE(died);
  const EntityId corpse = died->corpse_id;
  zone.tick(++t, sink);

  // A warrior's corpse, not the caster's own, and the owner is here: the offer goes out.
  REQUIRE(cast_and_wait(zone, sink, t, cleric, 0, corpse) == kDone);
  auto offer = sink.last<proto::RezOffer>(victim);
  REQUIRE(offer);
  CHECK(offer->caster == "Saint");
  CHECK(offer->pct == 50);
  zone.rez_response(victim, offer->offer_id + 1, true, sink);  // a reply to some other offer does nothing
  CHECK(zone.find(victim)->dead);
  zone.rez_response(victim, offer->offer_id, true, sink);
  const PlayerEntity& v = *zone.find(victim);
  CHECK_FALSE(v.dead);
  CHECK(v.hp > 0);
  CHECK(v.xp == xp_dead + lost / 2);
  CHECK(horizontal_distance(v.pos, zone.find_corpse(corpse)->pos) < 0.01f);
  CHECK(sink.chat_count(victim, "You have been resurrected by Saint") == 1);
  CHECK(zone.find_corpse(corpse)->rezzed);
  CHECK(zone.find_corpse(corpse)->xp_lost == 0);
  // The corpse keeps its loot for the owner, and cannot be resurrected again.
  run(zone, sink, t, 40);
  CHECK(cast_and_wait(zone, sink, t, cleric, 0, corpse) == -1);
  CHECK(sink.last<proto::AbilityResult>(cleric)->message == "That corpse has already been resurrected.");
  // A stale answer does nothing.
  zone.rez_response(victim, offer->offer_id, true, sink);
  CHECK(zone.find(victim)->xp == xp_dead + lost / 2);
}

TEST_CASE("a declined or expired resurrection restores nothing", "[spells][zone][rez]") {
  Zone zone(test_zone(), content(), kDt, 34);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto victim = zone.add_player(at(5, 5, "Stubborn", 12));
  const auto cleric = zone.add_player(with_spells(as_class(at(0, 0, "Patient", 20), "cleric"), {"rite_of_return"}));
  zone.tick(++t, sink);
  zone.force_effect(victim, lasting("test_doom", EffectType::Dot, 3, 0, BuffStat::Ac, 100000, 1), 1, sink);
  run_until(zone, sink, t, 100, [&] { return zone.find(victim)->dead; });
  const auto died = sink.last<proto::PlayerDied>(victim);
  REQUIRE(died);
  const EntityId corpse = died->corpse_id;
  const auto xp_dead = zone.find(victim)->xp;
  REQUIRE(cast_and_wait(zone, sink, t, cleric, 0, corpse) == kDone);
  const auto first = sink.last<proto::RezOffer>(victim);
  REQUIRE(first);
  zone.rez_response(victim, first->offer_id, false, sink);
  CHECK(zone.find(victim)->dead);
  CHECK(sink.chat_count(cleric, "declines your resurrection") == 1);
  REQUIRE(cast_and_wait(zone, sink, t, cleric, 0, corpse) == kDone);
  const auto second = sink.last<proto::RezOffer>(victim);
  REQUIRE(second);
  REQUIRE(second->offer_id != first->offer_id);  // a new offer, which then expires
  run(zone, sink, t, 20 * 61);
  zone.rez_response(victim, second->offer_id, true, sink);
  CHECK(zone.find(victim)->dead);
  CHECK(zone.find(victim)->xp == xp_dead);
  // A respawned owner can still be resurrected: they are pulled back to the corpse.
  zone.respawn(victim, sink);
  REQUIRE(cast_and_wait(zone, sink, t, cleric, 0, corpse) == kDone);
  zone.rez_response(victim, sink.last<proto::RezOffer>(victim)->offer_id, true, sink);
  CHECK(horizontal_distance(zone.find(victim)->pos, zone.find_corpse(corpse)->pos) < 0.01f);
  CHECK(zone.find(victim)->xp > xp_dead);
}

TEST_CASE("trainers sell scrolls; scribing learns them; memorizing takes a moment", "[spells][zone][trainer]") {
  ZoneDef zd = test_zone();
  zd.id = "proving_flats";  // the trainers live there
  Zone zone(zd, content(), kDt, 35);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit init = as_class(at(0, 4, "Apprentice", 5), "wizard");
  init.coin = 100;
  const auto p = zone.add_player(init);
  zone.tick(++t, sink);
  REQUIRE(zone.npc_count() >= 2);
  EntityId wiz_trainer = 0;
  EntityId clr_trainer = 0;
  zone.for_each_npc([&](const NpcEntity& n) {
    if (n.def->id == "archmage_torvan") wiz_trainer = n.id;
    if (n.def->id == "sister_maelis") clr_trainer = n.id;
  });
  REQUIRE(sink.spawned(p, wiz_trainer));

  // New casters start with their starter spells scribed and memorized.
  CHECK(zone.find(p)->knows_spell("ember_dart"));
  CHECK(zone.find(p)->gems[0] == "ember_dart");
  CHECK(zone.find(p)->gems[1] == "ward_of_glass");

  zone.trainer_open(p, clr_trainer, sink);
  CHECK(sink.chat_count(p, "I have nothing to teach a Wizard") == 1);
  zone.trainer_open(p, wiz_trainer, sink);
  auto w = sink.last<proto::TrainerWindow>(p);
  REQUIRE(w);
  CHECK(w->entries.size() == content().spells_for("wizard").size());
  CHECK(std::is_sorted(w->entries.begin(), w->entries.end(), [](const auto& a, const auto& b) { return a.level < b.level; }));
  const auto dart = std::find_if(w->entries.begin(), w->entries.end(), [](const auto& e) { return e.spell_id == "ember_dart"; });
  REQUIRE(dart != w->entries.end());
  CHECK(dart->known);

  const ItemDef* needle = content().scroll_for("frost_needle");
  const ItemDef* lash = content().scroll_for("flame_lash");
  REQUIRE(needle);
  REQUIRE(lash);
  zone.trainer_buy(p, wiz_trainer, lash->id, sink);
  CHECK(sink.chat_count(p, "You cannot afford") == 1);
  zone.trainer_buy(p, wiz_trainer, content().scroll_for("minor_mending")->id, sink);
  CHECK(sink.chat_count(p, "does not sell that to you") == 1);
  zone.trainer_buy(p, wiz_trainer, needle->id, sink);
  CHECK(zone.find(p)->coin == 100 - static_cast<std::uint64_t>(needle->value));
  const int slot = slot_of(*zone.find(p), needle->id);
  REQUIRE(slot != kNoSlot);

  zone.scribe_spell(p, slot, sink);
  CHECK(zone.find(p)->knows_spell("frost_needle"));
  CHECK(zone.find(p)->inv.at(static_cast<SlotCode>(slot))->empty());
  auto book = sink.last<proto::SpellBook>(p);
  REQUIRE(book);
  CHECK(std::any_of(book->spells.begin(), book->spells.end(), [](const auto& s) { return s.id == "frost_needle"; }));
  zone.scribe_spell(p, slot, sink);
  CHECK(sink.chat_count(p, "There is nothing there to scribe") == 1);

  zone.memorize_spell(p, 3, "frost_needle", sink);  // gem 4 opens at level 9
  CHECK(sink.chat_count(p, "You cannot use that spell gem yet") == 1);
  zone.memorize_spell(p, 2, "frost_needle", sink);
  CHECK(zone.find(p)->gems[2] == "frost_needle");
  auto bar = sink.last<proto::GemBar>(p);
  REQUIRE(bar);
  CHECK(bar->unlocked == 3);
  CHECK(bar->gems[2].spell_id == "frost_needle");
  CHECK(bar->gems[2].ready_ms > 0);
  const auto m = zone.spawn_mob(dummy(5), {20, 0, 0});
  zone.tick(++t, sink);
  zone.cast_spell(p, 2, m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message == "You are still memorizing Frost Needle.");
  run(zone, sink, t, 50);
  // Moving a memorized spell to another gem empties the old one.
  zone.memorize_spell(p, 0, "frost_needle", sink);
  CHECK(zone.find(p)->gems[0] == "frost_needle");
  CHECK(zone.find(p)->gems[2].empty());
  CHECK(sink.saves >= 4);
  run(zone, sink, t, 50);
  zone.cast_spell(p, 0, m, sink);
  CHECK(zone.find(p)->cast.casting());
  run_until(zone, sink, t, 200, [&] { return !zone.find(p)->cast.casting(); });
  zone.memorize_spell(p, 1, "frost_needle", sink);
  CHECK(sink.chat_count(p, "You cannot memorize spells in combat") == 1);

  // Scrolls of other classes and spells above your level cannot be scribed.
  ItemStackState clr{content().scroll_for("minor_mending")->id, 1};
  *zone.find(p)->inv.at(general_code(5)) = clr;
  zone.scribe_spell(p, general_code(5), sink);
  CHECK(sink.chat_count(p, "Only a Cleric can scribe") == 1);
  *zone.find(p)->inv.at(general_code(5)) = {lash->id, 1};
  zone.scribe_spell(p, general_code(5), sink);
  CHECK(sink.chat_count(p, "You must be level 9 to scribe") == 1);
  // Out of range of the trainer.
  zone.find(p)->pos = {200, 0, 200};
  zone.trainer_buy(p, wiz_trainer, needle->id, sink);
  CHECK(sink.chat_count(p, "too far away") == 1);
}

TEST_CASE("targeting rules: body types, friendly fallbacks, range, and line of sight", "[spells][zone]") {
  ZoneDef zd = test_zone();
  zd.obstacles.push_back({0, 15, 2});
  Zone zone(zd, content(), kDt, 36);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto c = zone.add_player(with_spells(as_class(at(0, 0, "Priest", 20), "cleric"), {"banish_dead", "greater_restoration", "halting_word"}));
  const auto rat = zone.spawn_mob(dummy(20), {10, 0, 0});
  MobDef bones = dummy(20);
  bones.body = "undead";
  const auto skel = zone.spawn_mob(bones, {-10, 0, 0});
  zone.force_effect(skel, lasting("test_root", EffectType::Root, 120), 60, sink);  // it must not interrupt the heal below
  const auto hidden = zone.spawn_mob(bones, {0, 0, 30});
  const auto far = zone.spawn_mob(bones, {0, 0, -45});
  zone.tick(++t, sink);
  CHECK(cast_and_wait(zone, sink, t, c, 0, rat) == -1);
  CHECK(sink.last<proto::AbilityResult>(c)->message == "Banish Dead only affects the undead.");
  CHECK(cast_and_wait(zone, sink, t, c, 0, skel) == kDone);
  CHECK(cast_and_wait(zone, sink, t, c, 0, hidden) == -1);
  CHECK(sink.last<proto::AbilityResult>(c)->message == "You cannot see your target.");
  CHECK(cast_and_wait(zone, sink, t, c, 2, far) == -1);  // halting word: 40 m
  CHECK(sink.last<proto::AbilityResult>(c)->message == "Your target is out of range.");
  CHECK(cast_and_wait(zone, sink, t, c, 0, c) == -1);
  CHECK(sink.last<proto::AbilityResult>(c)->message == "You can't cast Banish Dead on that.");
  // A heal with a mob targeted lands on the caster instead.
  zone.set_target(c, rat, sink);
  zone.find(c)->hp = 20;
  CHECK(cast_and_wait(zone, sink, t, c, 1, 0) == kDone);
  CHECK(zone.find(c)->hp > 20);
}

TEST_CASE("crowd control respects level caps and immunities", "[spells][zone][cc]") {
  Zone zone(test_zone(), content(), kDt, 37);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto c = zone.add_player(with_spells(as_class(at(0, 0, "Halter", 20), "cleric"), {"halting_word"}));
  const auto w = zone.add_player(with_spells(as_class(at(1, 0, "Binder", 20), "wizard"), {"binding_frost"}));
  const auto big = zone.spawn_mob(dummy(31), {10, 0, 0});
  MobDef wall = dummy(15);
  wall.stun_immune = true;
  const auto immune = zone.spawn_mob(wall, {-10, 0, 0});
  const auto small = zone.spawn_mob(dummy(15), {0, 0, 10});
  zone.tick(++t, sink);
  auto landed_one = [&](EntityId caster, EntityId target, CombatKind good, CombatKind bad) {
    for (int i = 0; i < 8; ++i) {
      sink.clear();
      cast_and_wait(zone, sink, t, caster, 0, target);
      if (events(sink, caster, good) > 0) return good;
      if (events(sink, caster, bad) > 0) return bad;
    }
    return CombatKind::Resist;
  };
  CHECK(landed_one(c, small, CombatKind::Stun, CombatKind::StunFail) == CombatKind::Stun);
  CHECK(landed_one(c, big, CombatKind::Stun, CombatKind::StunFail) == CombatKind::StunFail);     // cap 25
  CHECK(landed_one(c, immune, CombatKind::Stun, CombatKind::StunFail) == CombatKind::StunFail);  // stun immune
  CHECK(landed_one(w, big, CombatKind::Root, CombatKind::Immune) == CombatKind::Immune);         // cap 30
  CHECK(landed_one(w, small, CombatKind::Root, CombatKind::Immune) == CombatKind::Root);
}

TEST_CASE("evacuate shakes pursuit, gate goes home, gather mana refills", "[spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 38);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit init = with_spells(as_class(at(100, 100, "Escapist", 25), "wizard"), {"wind_retreat", "homeward_step", "gather_aether", "ember_dart"});
  init.bind_pos = {-50, 0, -50};
  const auto p = zone.add_player(init);
  const auto m = zone.spawn_mob(dummy(25), {130, 0, 100});
  zone.tick(++t, sink);
  zone.force_effect(m, lasting("test_root", EffectType::Root, 120), 30, sink);
  for (int i = 0; i < 6 && !zone.find_mob(m)->threat.contains(p); ++i) cast_and_wait(zone, sink, t, p, 3, m);
  REQUIRE(zone.find_mob(m)->threat.contains(p));
  REQUIRE(cast_and_wait(zone, sink, t, p, 0, 0) == kDone);
  CHECK(horizontal_distance(zone.find(p)->pos, {0, 0, 0}) < 0.01f);
  CHECK_FALSE(zone.find_mob(m)->threat.contains(p));
  CHECK(events(sink, p, CombatKind::Teleport) == 1);
  REQUIRE(cast_and_wait(zone, sink, t, p, 1, 0) == kDone);
  CHECK(horizontal_distance(zone.find(p)->pos, {-50, 0, -50}) < 0.01f);
  zone.find(p)->mana = 0;
  REQUIRE(cast_and_wait(zone, sink, t, p, 2, 0) == kDone);
  CHECK(zone.find(p)->mana >= zone.find(p)->stats.max_mana * 6 / 10 - 1);
  // A city in another zone is out of reach until zone travel exists.
  ZonePlayerInit far = with_spells(as_class(at(0, 0, "Traveler", 25), "wizard"), {"emberhold_translocation"});
  const auto q = zone.add_player(far);  // the test zone is not Emberhold's zone
  zone.tick(++t, sink);
  CHECK(cast_and_wait(zone, sink, t, q, 0, 0) == -1);
  CHECK(sink.last<proto::AbilityResult>(q)->message.find("beyond this zone") != std::string::npos);
}

TEST_CASE("buffs, recast timers, spellbook and gems survive a relog", "[spells][zone][persist]") {
  Zone zone(test_zone(), content(), kDt, 39);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Keeper", 30), "wizard"), {"mantle_of_runes", "gather_aether"}));
  zone.tick(++t, sink);
  REQUIRE(cast_and_wait(zone, sink, t, p, 0, 0) == kDone);
  zone.find(p)->mana = 0;
  REQUIRE(cast_and_wait(zone, sink, t, p, 1, 0) == kDone);
  const auto buffs = zone.saved_buffs(*zone.find(p));
  const auto timers = zone.saved_timers(*zone.find(p));
  REQUIRE(buffs.size() == 1);
  CHECK(buffs[0].ability_id == "mantle_of_runes");
  CHECK(buffs[0].seconds_left > 1790.0f);
  CHECK(std::any_of(timers.begin(), timers.end(), [](const SavedTimer& s) { return s.ability_id == "gather_aether" && s.seconds_left > 1190.0f; }));
  const int ac = zone.find(p)->stats.ac;
  ZonePlayerInit again = as_class(at(0, 0, "Keeper", 30), "wizard");
  again.spellbook = zone.find(p)->spellbook;
  again.gems.assign(zone.find(p)->gems.begin(), zone.find(p)->gems.end());
  again.buffs = buffs;
  again.timers = timers;
  zone.remove_player(p, sink);
  const auto q = zone.add_player(again);
  CHECK(zone.find(q)->stats.ac == ac);
  CHECK(has_buff(zone.find(q)->buffs, "mantle_of_runes"));
  CHECK(zone.find(q)->gems[1] == "gather_aether");
  zone.tick(++t, sink);
  zone.cast_spell(q, 1, 0, sink);
  CHECK(sink.last<proto::AbilityResult>(q)->message == "Gather Aether is not ready yet.");
  // Warriors have no spellbook at all.
  const auto war = zone.add_player(at(0, 0, "Brute", 10));
  CHECK(zone.find(war)->spellbook.empty());
}

TEST_CASE("area spells hit every foe around the target, up to the cap", "[spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 40);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Blaster", 36), "wizard"), {"cinder_ring", "hailstorm"}));
  std::vector<EntityId> ring;
  for (int i = 0; i < 4; ++i) ring.push_back(zone.spawn_mob(dummy(36), {std::cos(static_cast<float>(i) * 1.5f) * 6, 0, std::sin(static_cast<float>(i) * 1.5f) * 6}));
  const auto outside = zone.spawn_mob(dummy(36), {25, 0, 0});
  zone.tick(++t, sink);
  for (EntityId id : ring) zone.force_effect(id, lasting("test_root", EffectType::Root, 60), 60, sink);
  REQUIRE(cast_and_wait(zone, sink, t, p, 0, 0) == kDone);
  CHECK(events(sink, p, CombatKind::SpellHit) + events(sink, p, CombatKind::Resist) == 4);
  CHECK(zone.find_mob(outside)->threat.empty());
  sink.clear();
  REQUIRE(cast_and_wait(zone, sink, t, p, 1, outside) == kDone);  // around the target, 10 m: only it
  CHECK(events(sink, p, CombatKind::SpellHit) + events(sink, p, CombatKind::Resist) == 1);
}

// The acceptance check for Phase 3: with their own spells, a Cleric and a Wizard each beat an
// even-level mob across the content range (skills trained, no gear). A simple script plays them.
TEST_CASE("a Cleric and a Wizard can solo even-level mobs from 1 to 60 with their spells", "[spells][balance]") {
  struct Plan {
    const char* cls;
    std::vector<std::string> lines;  // stack groups, in gem priority order
  };
  const Plan wizard{"wizard", {"wizard_fire", "wizard_ward", "wizard_cold", "wizard_root", "wizard_arcane", "gather_aether"}};
  const Plan cleric{"cleric", {"cleric_heal", "cleric_ward", "cleric_smite", "cleric_hot", "cleric_undead", "cleric_stun"}};
  for (const Plan* plan : {&wizard, &cleric}) {
    for (int level : {1, 5, 10, 15, 20, 30, 40, 50, 60}) {
      // The best rank of each line known at this level, as many as there are gems.
      std::vector<std::string> gems;
      for (const auto& line : plan->lines) {
        const AbilityDef* best = nullptr;
        for (const AbilityDef* a : content().spells_for(plan->cls))
          if ((a->stack_group == line || a->id == line) && a->level <= level && (!best || a->rank > best->rank)) best = a;
        if (best && static_cast<int>(gems.size()) < gem_count(level, content().rules.spells)) gems.push_back(best->id);
      }
      auto gem_of = [&](const char* line) {
        for (std::size_t g = 0; g < gems.size(); ++g) {
          const AbilityDef& a = spell(gems[g].c_str());
          if (a.stack_group == line || a.id == line) return static_cast<int>(g);
        }
        return -1;
      };
      int wins = 0;
      const int fights = 12;
      for (int f = 0; f < fights; ++f) {
        Zone zone(test_zone(), content(), kDt, 1000 + static_cast<std::uint64_t>(level * 31 + f));
        RecordingSink sink;
        std::uint32_t t = 0;
        ZonePlayerInit init = geared(with_spells(as_class(at(0, 0, "Sim", level), plan->cls), gems));
        const ClassDef* cl = content().find_class(plan->cls);
        for (const auto& [sid, cap] : cl->skills) init.skills.push_back({sid, skill_cap(cap, level)});
        const auto p = zone.add_player(init);
        MobDef foe = mob("road_brigand");
        foe.id = "sim_foe";
        foe.level = level;
        foe.hp_mult = 1.0f;
        foe.damage_mult = 1.0f;
        foe.aggro_radius = 0;
        foe.assist = false;
        foe.flee_hp_pct = 0;
        foe.loot = "";
        const auto m = zone.spawn_mob(foe, {25, 0, 0});
        zone.tick(++t, sink);
        zone.set_target(p, m, sink);
        // Pre-buff, then pull: the mob comes to the caster.
        if (const int g = gem_of(plan->cls == std::string("wizard") ? "wizard_ward" : "cleric_ward"); g >= 0) cast_and_wait(zone, sink, t, p, g, p);
        zone.find_mob(m)->threat.add(p, 1.0);
        bool attacking = false;
        for (int i = 0; i < 20 * 240; ++i) {
          const PlayerEntity* me = zone.find(p);
          const MobEntity* foe_now = zone.find_mob(m);
          if (!me || me->dead || !foe_now || !foe_now->alive()) break;
          if (!me->cast.casting() && zone.now() >= me->gcd_until) {
            const float hp = static_cast<float>(me->hp) / static_cast<float>(me->stats.max_hp);
            int g = -1;
            EntityId target = m;
            if (plan->cls == std::string("cleric")) {
              if (hp < 0.5f && gem_of("cleric_heal") >= 0) g = gem_of("cleric_heal"), target = p;
              else if (gem_of("cleric_smite") >= 0 && me->mana > me->stats.max_mana / 3) g = gem_of("cleric_smite");
            } else {
              if (me->mana < spell("ember_dart").cost && gem_of("gather_aether") >= 0) g = gem_of("gather_aether"), target = p;
              else g = gem_of("wizard_fire");
            }
            if (g >= 0) {
              const AbilityDef& a = spell(gems[static_cast<std::size_t>(g)].c_str());
              if (me->mana >= a.cost && zone.now() >= me->gem_ready[static_cast<std::size_t>(g)]) zone.cast_spell(p, g, target, sink);
            }
          }
          if (!attacking && horizontal_distance(zone.find(p)->pos, zone.find_mob(m)->pos) < 5.0f) {
            zone.set_auto_attack(p, true, sink);
            attacking = true;
          }
          zone.tick(++t, sink);
        }
        const MobEntity* end = zone.find_mob(m);
        if (zone.find(p) && !zone.find(p)->dead && (!end || !end->alive())) ++wins;
      }
      UNSCOPED_INFO(plan->cls << " at level " << level << " won " << wins << "/" << fights);
      std::printf("balance: %s level %d won %d/%d\n", plan->cls, level, wins, fights);
      CHECK(wins >= fights * 3 / 4);
    }
  }
}

// Acceptance: a spell added through JSON alone (with its scroll and a trainer listing) is
// learnable and castable, with no code change. Its mix of effects (damage, a DoT, a slow, and a
// resist debuff in one spell) exists in no shipped spell.
TEST_CASE("a brand-new spell defined only in JSON is bought, scribed, memorized, and cast", "[spells][zone][content]") {
  auto read = [](const char* name) {
    std::ifstream in(std::filesystem::path(EF_CONTENT_DIR) / name);
    return nlohmann::json::parse(in);
  };
  ContentDocs d;
  d.rules = read("rules.json");
  d.races = read("races.json");
  d.classes = read("classes.json");
  d.cities = read("cities.json");
  d.zones = read("zones.json");
  d.skills = read("skills.json");
  d.abilities = read("abilities.json");
  d.items = read("items.json");
  d.loot = read("loot.json");
  d.mobs = read("mobs.json");
  d.spells = read("spells.json");
  d.npcs = read("npcs.json");
  d.pets = read("pets.json");
  d.item_effects = read("item_effects.json");
  d.spells["spells"].push_back(nlohmann::json::parse(R"({
    "id": "frostfire_hex", "name": "Frostfire Hex", "class": "wizard", "level": 3, "type": "dot", "resource": "mana",
    "cost": 12, "cast_time": 1.5, "cooldown": 4, "gcd": true, "range": 50, "target": "enemy", "resist": "magic",
    "effects": [{"type": "damage", "base": 5, "per_level": 1},
                {"type": "dot", "base": 3, "per_level": 0.5, "duration": 9, "tick": 3},
                {"type": "slow", "value": 25, "duration": 9},
                {"type": "debuff", "stat": "resist_fire", "value": -30, "duration": 9}],
    "description": "A test hex of fire and frost."})"));
  d.items["items"].push_back(nlohmann::json::parse(
      R"({"id": "scroll_frostfire_hex", "name": "Spell: Frostfire Hex", "kind": "scroll", "stack": 1, "value": 10, "spell": "frostfire_hex"})"));
  for (auto& n : d.npcs["npcs"])
    if (n["id"] == "archmage_torvan") n["sells"].push_back("scroll_frostfire_hex");
  const Content c = parse_content(d);  // validation passes: it is reachable and well formed
  REQUIRE(c.find_spell("frostfire_hex"));

  ZoneDef zd = test_zone();
  zd.id = "proving_flats";
  Zone zone(zd, c, kDt, 41);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit init = as_class(at(0, 4, "Hexer", 5), "wizard");
  init.coin = 50;
  const auto p = zone.add_player(init);
  zone.tick(++t, sink);
  EntityId trainer = 0;
  zone.for_each_npc([&](const NpcEntity& n) {
    if (n.def->id == "archmage_torvan") trainer = n.id;
  });
  zone.trainer_buy(p, trainer, "scroll_frostfire_hex", sink);
  const int slot = slot_of(*zone.find(p), "scroll_frostfire_hex");
  REQUIRE(slot != kNoSlot);
  zone.scribe_spell(p, slot, sink);
  zone.memorize_spell(p, 2, "frostfire_hex", sink);
  REQUIRE(zone.find(p)->gems[2] == "frostfire_hex");
  run(zone, sink, t, 50);
  MobDef target = mob("road_brigand");
  target.id = "test_target";
  target.level = 5;
  target.hp_mult = 50;
  target.damage_mult = 0.01f;
  target.aggro_radius = 0;
  target.resists = {0, 0, 0, 0, 0, 0};
  const auto m = zone.spawn_mob(target, {20, 0, 0});
  zone.tick(++t, sink);
  for (int i = 0; i < 8 && !has_buff(zone.find_mob(m)->buffs, "frostfire_hex"); ++i) cast_and_wait(zone, sink, t, p, 2, m);
  REQUIRE(has_buff(zone.find_mob(m)->buffs, "frostfire_hex"));
  const auto& hex = zone.find_mob(m)->buffs.back();
  CHECK(hex.harms_over_time());
  CHECK(stat_total(zone.find_mob(m)->buffs, BuffStat::AttackSpeedPct) == -25.0f);
  CHECK(stat_total(zone.find_mob(m)->buffs, BuffStat::ResistFire) == -30.0f);
  CHECK(events(sink, p, CombatKind::SpellHit) >= 1);
  run(zone, sink, t, 20 * 10);
  CHECK(events(sink, p, CombatKind::DotTick) >= 3);
  CHECK_FALSE(has_buff(zone.find_mob(m)->buffs, "frostfire_hex"));
}

TEST_CASE("a heal whose recipient leaves mid-cast fails instead of landing on the caster", "[spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 42);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto healer = zone.add_player(with_spells(as_class(at(0, 0, "Medic", 20), "cleric"), {"greater_restoration"}));
  const auto friend_id = zone.add_player(at(5, 0, "Patient", 20));
  zone.tick(++t, sink);
  zone.find(healer)->hp = 10;
  const int mana = zone.find(healer)->mana;
  zone.cast_spell(healer, 0, friend_id, sink);
  REQUIRE(zone.find(healer)->cast.casting());
  zone.remove_player(friend_id, sink);
  run_until(zone, sink, t, 200, [&] { return !zone.find(healer)->cast.casting(); });
  const auto end = sink.last<proto::CastEnd>(healer);
  REQUIRE(end);
  CHECK(end->result == static_cast<std::uint8_t>(CastResult::Failed));
  CHECK(end->message == "Your target is gone.");
  CHECK(zone.find(healer)->mana == mana);
  CHECK(events(sink, healer, CombatKind::Heal) == 0);  // nothing was healed (regen still ticks)
}

TEST_CASE("an area spell takes only its nearest foes up to the target cap", "[spells][zone]") {
  Content c = content();
  for (auto& sp : c.spells)
    if (sp.id == "cinder_ring") sp.max_targets = 2;
  Zone zone(test_zone(), c, kDt, 43);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Capped", 20), "wizard"), {"cinder_ring"}));
  std::vector<EntityId> mobs;
  for (const float d : {3.0f, 5.0f, 8.0f, 10.0f}) mobs.push_back(zone.spawn_mob(dummy(20), {d, 0, 0}));
  zone.tick(++t, sink);
  for (EntityId id : mobs) zone.force_effect(id, lasting("test_root", EffectType::Root, 60), 60, sink);
  REQUIRE(cast_and_wait(zone, sink, t, p, 0, 0) == kDone);
  CHECK(events(sink, p, CombatKind::SpellHit) + events(sink, p, CombatKind::Resist) == 2);
  CHECK(zone.find_mob(mobs[0])->threat.contains(p));
  CHECK(zone.find_mob(mobs[1])->threat.contains(p));
  CHECK_FALSE(zone.find_mob(mobs[2])->threat.contains(p));
  CHECK_FALSE(zone.find_mob(mobs[3])->threat.contains(p));
}

TEST_CASE("a level-capped line is dropped on its own and the rest of the spell still lands", "[spells][zone][cc]") {
  Content c = content();
  AbilityDef hex = lasting("test_bind_burn", EffectType::Root, 12);
  hex.class_id = "wizard";
  hex.level = 1;
  hex.resource = Resource::Mana;
  hex.range = 50;
  hex.effects[0].max_target_level = 30;
  EffectDef burn;
  burn.type = EffectType::Dot;
  burn.base = 4;
  burn.duration = 12;
  burn.tick = 3;
  hex.effects.push_back(burn);
  c.spells.push_back(hex);
  Zone zone(test_zone(), c, kDt, 44);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit init = as_class(at(0, 0, "Binder", 20), "wizard");
  init.spellbook = {"test_bind_burn"};
  init.gems = {"test_bind_burn"};
  const auto p = zone.add_player(init);
  const auto big = zone.spawn_mob(dummy(31), {15, 0, 0});
  const auto small = zone.spawn_mob(dummy(30), {0, 0, 15});
  zone.tick(++t, sink);
  for (int i = 0; i < 8 && !has_buff(zone.find_mob(big)->buffs, "test_bind_burn"); ++i) cast_and_wait(zone, sink, t, p, 0, big);
  REQUIRE(has_buff(zone.find_mob(big)->buffs, "test_bind_burn"));
  CHECK(control_bits(zone.find_mob(big)->buffs) == 0);  // no root above level 30
  CHECK(zone.find_mob(big)->buffs.back().harms_over_time());  // but the burn landed
  CHECK(events(sink, p, CombatKind::Immune) >= 1);
  for (int i = 0; i < 8 && !has_buff(zone.find_mob(small)->buffs, "test_bind_burn"); ++i) cast_and_wait(zone, sink, t, p, 0, small);
  REQUIRE(has_buff(zone.find_mob(small)->buffs, "test_bind_burn"));
  CHECK((control_bits(zone.find_mob(small)->buffs) & kControlRoot) != 0);  // at the cap, both lines apply
}

TEST_CASE("each lasting line keeps its own duration and tick timer", "[spells][zone][dot]") {
  // One spell: a 3 s root, a 4-damage DoT every 2 s for 8 s, and a 7-damage DoT every 3 s for 9 s.
  AbilityDef mix = lasting("test_mix", EffectType::Root, 3);
  EffectDef fast;
  fast.type = EffectType::Dot;
  fast.base = 4;
  fast.duration = 8;
  fast.tick = 2;
  EffectDef slow;
  slow.type = EffectType::Dot;
  slow.base = 7;
  slow.duration = 9;
  slow.tick = 3;
  mix.effects.push_back(fast);
  mix.effects.push_back(slow);
  Zone zone(test_zone(), content(), kDt, 45);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto watcher = zone.add_player(at(0, 0, "Watcher", 60));
  const auto m = zone.spawn_mob(dummy(10), {10, 0, 0});
  zone.tick(++t, sink);
  zone.find_mob(m)->threat.add(watcher, 1.0);  // in a fight: idle mobs would regenerate
  REQUIRE(zone.force_effect(m, mix, 10, sink) == StackAction::Add);
  const int hp0 = zone.find_mob(m)->hp;
  auto damage_after = [&](float seconds) {
    run(zone, sink, t, static_cast<int>(std::lround(seconds * 20.0f)));
    return hp0 - zone.find_mob(m)->hp;
  };
  CHECK(damage_after(2.0f) == 4);                         // the fast line ticked alone
  CHECK((control_bits(zone.find_mob(m)->buffs) & kControlRoot) != 0);
  CHECK(damage_after(1.0f) == 4 + 7);                     // 3 s: the slow line, and the root ends
  CHECK((control_bits(zone.find_mob(m)->buffs) & kControlRoot) == 0);
  REQUIRE(has_buff(zone.find_mob(m)->buffs, "test_mix"));  // the DoTs still run
  CHECK(damage_after(6.0f) == 4 * 4 + 7 * 3);             // 9 s: four fast ticks and three slow ones
  CHECK_FALSE(has_buff(zone.find_mob(m)->buffs, "test_mix"));
}

TEST_CASE("breaking a root keeps the rest of the spell running", "[spells][zone][cc]") {
  Content c = content();
  c.rules.spells.root_break_chance = 1.0f;  // every direct hit breaks it
  AbilityDef bind = lasting("test_bind", EffectType::Root, 30);
  EffectDef burn;
  burn.type = EffectType::Dot;
  burn.base = 3;
  burn.duration = 30;
  burn.tick = 3;
  bind.effects.push_back(burn);
  Zone zone(test_zone(), c, kDt, 46);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(at(0, 0, "Breaker", 30));
  const auto m = zone.spawn_mob(dummy(5), {3, 0, 0});
  zone.tick(++t, sink);
  REQUIRE(zone.force_effect(m, bind, 30, sink) == StackAction::Add);
  zone.set_target(p, m, sink);
  zone.set_auto_attack(p, true, sink);
  run_until(zone, sink, t, 20 * 30, [&] { return events(sink, p, CombatKind::RootBreak) > 0; });
  REQUIRE(events(sink, p, CombatKind::RootBreak) == 1);
  REQUIRE(has_buff(zone.find_mob(m)->buffs, "test_bind"));
  CHECK((control_bits(zone.find_mob(m)->buffs) & kControlRoot) == 0);
  CHECK(zone.find_mob(m)->buffs.front().harms_over_time());
}

TEST_CASE("a HoT saved just before a tick ticks soon after the relog", "[spells][zone][persist]") {
  Zone zone(test_zone(), content(), kDt, 47);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Glower", 15), "cleric"), {"soothing_glow"}));
  zone.tick(++t, sink);
  zone.find(p)->hp = 5;
  REQUIRE(cast_and_wait(zone, sink, t, p, 0, p) == kDone);
  run(zone, sink, t, 50);  // 2.5 s into the 3 s interval
  const auto saved = zone.saved_buffs(*zone.find(p));
  REQUIRE(saved.size() == 1);
  CHECK(saved[0].next_tick_seconds > 0.0f);
  CHECK(saved[0].next_tick_seconds < 1.0f);
  ZonePlayerInit again = as_class(at(0, 0, "Glower", 15), "cleric");
  again.spellbook = zone.find(p)->spellbook;
  again.buffs = saved;
  again.hp = 5;
  zone.remove_player(p, sink);
  const auto q = zone.add_player(again);
  sink.clear();
  run(zone, sink, t, 20);  // within a second, not a whole new interval
  CHECK(events(sink, q, CombatKind::HotTick) == 1);
}
