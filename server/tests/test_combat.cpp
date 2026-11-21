// Zone-level combat: real content, a recording sink, deterministic RNG.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "zone_support.hpp"

using namespace zt;

namespace {

// Runs ticks until `done()` or the limit; returns ticks used.
template <class F>
int run_until(Zone& z, RecordingSink& sink, std::uint32_t& t, int limit, F done) {
  for (int i = 0; i < limit; ++i) {
    if (done()) return i;
    z.tick(++t, sink);
  }
  return limit;
}

bool mob_gone(Zone& z, EntityId m) {
  const MobEntity* mob = z.find_mob(m);
  return !mob || !mob->alive();
}

EntityId first_corpse(Zone& z) {
  EntityId id = 0;
  z.for_each_corpse([&](const CorpseEntity& c) {
    if (id == 0) id = c.id;
  });
  return id;
}

int events(const RecordingSink& s, EntityId to, CombatKind kind) {
  int n = 0;
  for (const auto& e : s.all<proto::CombatEvent>(to))
    if (e.kind == static_cast<std::uint8_t>(kind)) ++n;
  return n;
}

// A deadly mob that never aggroes on its own (so a respawned player can walk back safely).
const MobDef& brute() {
  static const MobDef d = [] {
    MobDef m = mob("thornback_boar");
    m.id = "test_brute";
    m.name = "a test brute";
    m.level = 30;
    m.loot = "";
    return m;
  }();
  return d;
}

}  // namespace

TEST_CASE("a warrior kills a mob, gains XP, and loots the corpse", "[combat]") {
  Zone zone(test_zone(), content(), kDt, 11);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(at(0, 0, "Hero", 2));
  const auto m = zone.spawn_mob(mob("ember_rat"), {2, 0, 0});
  zone.tick(++t, sink);
  REQUIRE(sink.spawned(p, m));
  const auto xp_before = zone.find(p)->xp;

  zone.set_target(p, m, sink);
  auto info = sink.last<proto::TargetInfo>(p);
  REQUIRE(info);
  CHECK(info->entity_id == m);
  CHECK(info->con == static_cast<std::uint8_t>(Con::Blue));
  zone.set_auto_attack(p, true, sink);
  CHECK(zone.find(p)->auto_attack);

  run_until(zone, sink, t, 20 * 120, [&] { return mob_gone(zone, m); });
  REQUIRE(mob_gone(zone, m));
  const PlayerEntity& hero = *zone.find(p);
  REQUIRE_FALSE(hero.dead);
  CHECK(hero.xp > xp_before);
  CHECK(sink.chat_count(p, "You gain experience") == 1);
  CHECK(events(sink, p, CombatKind::Death) == 1);
  CHECK(events(sink, p, CombatKind::Hit) + events(sink, p, CombatKind::Crit) > 0);
  CHECK_FALSE(hero.auto_attack);  // the target died
  CHECK(hero.target == 0);
  CHECK(sink.last<proto::TargetInfo>(p)->entity_id == 0);
  CHECK(hero.skill("offense") >= 1);

  // The corpse holds the roll; only the killer can open it.
  const EntityId corpse = first_corpse(zone);
  REQUIRE(corpse != 0);
  zone.tick(++t, sink);
  CHECK(sink.spawned(p, corpse));
  const CorpseEntity* c = zone.find_corpse(corpse);
  REQUIRE(c);
  CHECK(c->loot_char_id == hero.char_id);
  const std::uint64_t coin = c->coin;
  const std::size_t stacks = c->items.size();
  zone.loot_open(p, corpse, sink);
  auto window = sink.last<proto::LootWindow>(p);
  REQUIRE(window);
  CHECK(window->corpse_id == corpse);
  CHECK(window->items.size() == stacks);
  CHECK(zone.find(p)->coin == coin);  // coin is auto-looted on open
  zone.loot_take(p, corpse, kLootAll, sink);
  int carried = 0;
  for (const auto& s : zone.find(p)->inventory) carried += s.count;
  CHECK(carried >= static_cast<int>(stacks));
  // An emptied mob corpse vanishes when the window closes (taking everything closes it).
  CHECK(zone.find_corpse(corpse) == nullptr);
  CHECK(sink.last<proto::LootClosed>(p));
  CHECK(sink.loots_persisted >= (coin > 0 ? 1 : 0) + (stacks > 0 ? 1 : 0));
}

TEST_CASE("XP levels you up, unlocks skills and abilities, and is saved", "[combat][xp]") {
  Zone zone(test_zone(), content(), kDt, 2);
  RecordingSink sink;
  const auto p = zone.add_player(at(0, 0, "Grower"));
  zone.tick(1, sink);
  CHECK(zone.find(p)->level == 1);
  CHECK(zone.find(p)->skill("dodge") == 0);
  const int hp1 = zone.find(p)->stats.max_hp;
  zone.grant_xp(p, total_xp_for_level(6, content().rules.xp), sink);
  const PlayerEntity& g = *zone.find(p);
  CHECK(g.level == 6);
  CHECK(g.stats.max_hp > hp1);
  CHECK(g.skill("dodge") == 1);  // unlocked at 6
  CHECK(g.skill("bash") == 1);
  CHECK(sink.chat_count(p, "Welcome to level 6") == 1);
  CHECK(sink.chat_count(p, "You have learned Pommel Crack") == 1);
  CHECK(sink.saves >= 1);
  // XP is capped at max level.
  zone.grant_xp(p, 1ULL << 40, sink);
  CHECK(zone.find(p)->level == 60);
  CHECK(zone.find(p)->xp == total_xp_for_level(60, content().rules.xp));
}

TEST_CASE("aggressive mobs aggro in range and call same-faction friends; grey players are ignored", "[combat][ai]") {
  Zone zone(test_zone(), content(), kDt, 3);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(at(0, 0, "Bait", 5));
  const auto a = zone.spawn_mob(mob("road_brigand"), {10, 0, 0});   // aggro radius 12
  const auto b = zone.spawn_mob(mob("road_brigand"), {22, 0, 0});   // outside aggro, inside assist (15) of a
  const auto c = zone.spawn_mob(mob("mangy_cur"), {-30, 0, 0});     // different faction, far
  run_until(zone, sink, t, 20, [&] { return zone.find_mob(a)->threat.contains(p); });
  CHECK(zone.find_mob(a)->threat.contains(p));
  CHECK(zone.find_mob(b)->threat.contains(p));  // assisted
  CHECK_FALSE(zone.find_mob(c)->threat.contains(p));

  Zone calm(test_zone(), content(), kDt, 3);
  const auto vet = calm.add_player(at(0, 0, "Veteran", 40));
  const auto d = calm.spawn_mob(mob("road_brigand"), {5, 0, 0});
  for (std::uint32_t i = 1; i <= 40; ++i) calm.tick(i, sink);
  CHECK_FALSE(calm.find_mob(d)->threat.contains(vet));
  CHECK(calm.find_mob(d)->state != AiState::Chase);
}

TEST_CASE("a mob kited past its leash evades home, resets, and forgets", "[combat][ai]") {
  Zone zone(test_zone(), content(), kDt, 4);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(at(0, 0, "Kiter", 10));
  const auto m = zone.spawn_mob(mob("road_brigand"), {2, 0, 0});  // leash 80
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  zone.use_ability(p, "rising_kick", m, sink);
  run_until(zone, sink, t, 200, [&] { return zone.find_mob(m)->hp < zone.find_mob(m)->stats.max_hp; });
  REQUIRE(zone.find_mob(m)->threat.contains(p));
  // Run east faster than the brigand can follow.
  std::uint32_t seq = 0;
  bool leashed = false;
  for (int i = 0; i < 20 * 40 && !leashed; ++i) {
    PlayerEntity* k = zone.find(p);
    const float next = std::min(k->pos.x + 0.35f, 500.0f);
    zone.queue_input(p, ++seq, {1, 0, -1.5708f}, {next, 0, 0});
    zone.tick(++t, sink);
    leashed = zone.find_mob(m)->state == AiState::Leash;
  }
  REQUIRE(leashed);
  CHECK(zone.find_mob(m)->threat.empty());
  run_until(zone, sink, t, 20 * 60, [&] { return zone.find_mob(m)->state == AiState::Idle; });
  const MobEntity& home = *zone.find_mob(m);
  CHECK(home.state == AiState::Idle);
  CHECK(home.hp == home.stats.max_hp);
  CHECK(horizontal_distance(home.pos, home.home) < 1.0f);
}

TEST_CASE("taunt pulls a mob onto the taunter", "[combat][threat]") {
  Zone zone(test_zone(), content(), kDt, 5);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto dps = zone.add_player(at(0, 0, "Striker", 12));
  const auto tank = zone.add_player(at(1, 1, "Tank", 12));
  const auto m = zone.spawn_mob(mob("brigand_lookout"), {2, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(dps, m, sink);
  zone.set_auto_attack(dps, true, sink);
  run_until(zone, sink, t, 200, [&] { return zone.find_mob(m)->threat.top() == dps; });
  REQUIRE(zone.find_mob(m)->threat.top() == dps);
  zone.set_target(tank, m, sink);
  bool taunted = false;
  for (int attempt = 0; attempt < 20 && !taunted; ++attempt) {
    zone.use_ability(tank, "iron_challenge", m, sink);
    taunted = zone.find_mob(m)->threat.top() == tank;
    run_until(zone, sink, t, 20 * 7, [] { return false; });  // wait out the cooldown
    if (mob_gone(zone, m)) break;
  }
  CHECK(taunted);
  CHECK(events(sink, tank, CombatKind::Taunt) >= 1);
}

TEST_CASE("abilities enforce level, range, cooldown, GCD, cost, and ownership", "[combat]") {
  Zone zone(test_zone(), content(), kDt, 6);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(at(0, 0, "Learner"));
  const auto far = zone.spawn_mob(mob("ember_rat"), {30, 0, 0});
  const auto near = zone.spawn_mob(mob("dusk_beetle"), {2, 0, 0});
  zone.tick(++t, sink);
  auto result = [&] { return *sink.last<proto::AbilityResult>(p); };

  zone.use_ability(p, "pommel_crack", near, sink);
  CHECK_FALSE(result().ok);
  CHECK(result().message.find("level 6") != std::string::npos);
  zone.use_ability(p, "no_such_thing", near, sink);
  CHECK(result().message.find("do not know") != std::string::npos);
  zone.use_ability(p, "sidestep", 0, sink);  // a passive (and too low level, but passive wins)
  CHECK_FALSE(result().ok);
  zone.use_ability(p, "rising_kick", 0, sink);
  CHECK(result().message.find("need a target") != std::string::npos);
  zone.use_ability(p, "rising_kick", far, sink);
  CHECK(result().message.find("out of range") != std::string::npos);

  zone.use_ability(p, "rising_kick", near, sink);
  REQUIRE(result().ok);
  CHECK(result().cooldown_ms == 6000);
  CHECK(result().gcd_ms == 1500);
  const int end_after = zone.find(p)->endurance;
  zone.use_ability(p, "rising_kick", near, sink);
  CHECK(result().message.find("not ready") != std::string::npos);
  zone.use_ability(p, "iron_challenge", near, sink);
  CHECK(result().message.find("wait a moment") != std::string::npos);  // global cooldown
  CHECK(zone.find(p)->endurance == end_after);  // failures cost nothing

  run_until(zone, sink, t, 20 * 7, [] { return false; });
  zone.find(p)->endurance = 0;
  zone.use_ability(p, "rising_kick", near, sink);
  CHECK(result().message.find("fatigued") != std::string::npos);
}

TEST_CASE("Pommel Crack stuns lower mobs but never stun-immune or much higher ones", "[combat]") {
  Zone zone(test_zone(), content(), kDt, 7);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(at(0, 0, "Basher", 10));
  const auto low = zone.spawn_mob(mob("grave_rattler"), {2, 0, 0});
  const auto immune = zone.spawn_mob(mob("moss_golem"), {-2, 0, 0});
  zone.tick(++t, sink);
  bool stunned_low = false;
  for (int i = 0; i < 15 && !stunned_low; ++i) {
    zone.find(p)->endurance = 100;
    zone.use_ability(p, "pommel_crack", low, sink);
    stunned_low = !mob_gone(zone, low) && zone.find_mob(low)->stunned_until > zone.now();
    run_until(zone, sink, t, 20 * 13, [] { return false; });
    zone.find(p)->hp = zone.find(p)->stats.max_hp;
    if (mob_gone(zone, low)) break;
  }
  CHECK(stunned_low);
  int tries = 0;
  for (; tries < 10 && !mob_gone(zone, immune); ++tries) {
    zone.find(p)->endurance = 100;
    zone.find(p)->hp = zone.find(p)->stats.max_hp;
    zone.use_ability(p, "pommel_crack", immune, sink);
    CHECK(zone.find_mob(immune)->stunned_until <= zone.now());
    run_until(zone, sink, t, 20 * 13, [] { return false; });
  }
  CHECK(events(sink, p, CombatKind::StunFail) >= 1);
}

TEST_CASE("death: XP loss, corpse holds coin and pack, respawn at bind, owner recovers it", "[combat][death]") {
  Zone zone(test_zone(), content(), kDt, 8);
  RecordingSink sink;
  std::uint32_t t = 0;
  auto init = at(100, 100, "Unlucky", 8);
  init.xp = total_xp_for_level(8, content().rules.xp) + 50;
  init.coin = 500;
  init.items.push_back({0, ItemStackState{"rat_whisker", 3}});
  init.items.push_back({4, ItemStackState{"wolf_fang", 1}});
  const auto p = zone.add_player(init);
  const auto m = zone.spawn_mob(brute(), {102, 0, 100});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  zone.set_auto_attack(p, true, sink);
  int saves_before_death = 0;
  run_until(zone, sink, t, 20 * 120, [&] {
    if (!zone.find(p)->dead) saves_before_death = sink.saves;
    return zone.find(p)->dead;
  });
  PlayerEntity& u = *zone.find(p);
  // Losing a level to the penalty must not save on its own: the corpse write saves everything at once.
  CHECK(sink.saves == saves_before_death);
  REQUIRE(u.dead);
  CHECK(u.hp == 0);
  CHECK(u.coin == 0);
  for (const auto& s : u.inventory) CHECK(s.empty());
  const auto died = sink.last<proto::PlayerDied>(p);
  REQUIRE(died);
  CHECK(died->killer == "a test brute");
  const std::uint64_t expected_loss = death_xp_loss(init.xp, 8, content().rules.death, content().rules.xp);
  CHECK(died->xp_lost == expected_loss);
  CHECK(u.xp == init.xp - expected_loss);
  CHECK(u.level == 7);  // the penalty can cost a level
  CHECK(sink.deaths_persisted == 1);
  CHECK(zone.find_mob(m)->threat.empty());  // the killer forgets the dead

  const EntityId corpse_id = died->corpse_id;
  const CorpseEntity* corpse = zone.find_corpse(corpse_id);
  REQUIRE(corpse);
  CHECK(corpse->player_corpse);
  CHECK(corpse->coin == 500);
  CHECK(corpse->items.size() == 2);
  CHECK(corpse->xp_lost == expected_loss);

  // Dead players cannot act or move.
  zone.use_ability(p, "rising_kick", m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message.find("dead") != std::string::npos);
  const Vec3 where = u.pos;
  zone.queue_input(p, 1, {1, 0, 0}, {where.x + 0.35f, 0, where.z});
  zone.tick(++t, sink);
  CHECK(zone.find(p)->pos.x == where.x);

  zone.respawn(p, sink);
  PlayerEntity& back = *zone.find(p);
  CHECK_FALSE(back.dead);
  CHECK(back.hp == back.stats.max_hp);
  CHECK(horizontal_distance(back.pos, {0, 0, 0}) < 0.01f);  // the bind point
  REQUIRE_FALSE(sink.corrections.empty());
  CHECK(sink.corrections.back().second == "respawn");
  zone.tick(++t, sink);
  zone.loot_open(p, corpse_id, sink);
  CHECK(sink.last<proto::LootClosed>(p)->message.find("too far") != std::string::npos);

  // Walk back (teleported here for the test; movement rules have their own tests).
  back.pos = {corpse->pos.x - 3, 0, corpse->pos.z};
  zone.tick(++t, sink);
  zone.loot_open(p, corpse_id, sink);
  REQUIRE(sink.last<proto::LootWindow>(p));
  CHECK(zone.find(p)->coin == 500);
  zone.loot_take(p, corpse_id, kLootAll, sink);
  int whiskers = 0, fangs = 0;
  for (const auto& s : zone.find(p)->inventory) {
    if (s.item_id == "rat_whisker") whiskers += s.count;
    if (s.item_id == "wolf_fang") fangs += s.count;
  }
  CHECK(whiskers == 3);
  CHECK(fangs == 1);
  // An emptied player corpse stays until it decays (a future resurrection target).
  zone.loot_close(p, corpse_id, sink);
  REQUIRE(zone.find_corpse(corpse_id));
  CHECK(zone.find_corpse(corpse_id)->empty());
  const std::uint32_t life = zone.ticks(content().rules.death.player_corpse_seconds);
  for (std::uint32_t i = 0; i < life + 2 && zone.find_corpse(corpse_id); ++i) zone.tick(++t, sink);
  CHECK(zone.find_corpse(corpse_id) == nullptr);
  CHECK(sink.corpses_removed >= 1);
}

TEST_CASE("loot rights: only the owner opens a player corpse, only the killer a mob corpse", "[combat][loot]") {
  Zone zone(test_zone(), content(), kDt, 9);
  RecordingSink sink;
  std::uint32_t t = 0;
  CorpseInit ci;
  ci.db_id = 77;
  ci.owner_char_id = 4242;
  ci.owner_name = "Ghost";
  ci.pos = {1, 0, 0};
  ci.coin = 10;
  ci.seconds_left = 120;
  const auto corpse = zone.add_corpse(ci);
  const auto thief = zone.add_player(at(0, 0, "Thief", 3));
  auto owner_init = at(0, 1, "Ghost", 3);
  owner_init.char_id = 4242;
  const auto owner = zone.add_player(owner_init);
  zone.tick(++t, sink);
  zone.loot_open(thief, corpse, sink);
  CHECK(sink.last<proto::LootClosed>(thief)->message.find("someone else") != std::string::npos);
  zone.loot_open(owner, corpse, sink);
  REQUIRE(sink.last<proto::LootWindow>(owner));
  CHECK(zone.find(owner)->coin == 10);
  // Two looters cannot hold the same corpse open.
  auto other = at(0, 2, "Other", 3);
  other.char_id = 4242;  // even a second session of the owner waits its turn
  const auto second = zone.add_player(other);
  zone.tick(++t, sink);
  zone.loot_open(second, corpse, sink);
  CHECK(sink.last<proto::LootClosed>(second)->message.find("already looting") != std::string::npos);

  const auto m = zone.spawn_mob(mob("ember_rat"), {3, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(owner, m, sink);
  zone.set_auto_attack(owner, true, sink);
  run_until(zone, sink, t, 20 * 120, [&] { return mob_gone(zone, m); });
  REQUIRE(mob_gone(zone, m));
  EntityId mob_corpse = 0;
  zone.for_each_corpse([&](const CorpseEntity& c) {
    if (!c.player_corpse) mob_corpse = c.id;
  });
  REQUIRE(mob_corpse != 0);
  zone.tick(++t, sink);
  zone.loot_open(thief, mob_corpse, sink);
  CHECK(sink.last<proto::LootClosed>(thief)->message.find("did not earn") != std::string::npos);
}

TEST_CASE("sitting doubles out-of-combat regen; moving stands you up", "[combat]") {
  Zone zone(test_zone(), content(), kDt, 10);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto a = zone.add_player(at(0, 0, "Rester", 20));
  const auto b = zone.add_player(at(5, 0, "Stander", 20));
  zone.find(a)->hp = 1;
  zone.find(b)->hp = 1;
  zone.set_sitting(a, true, sink);
  for (int i = 0; i < 20 * 10; ++i) zone.tick(++t, sink);
  CHECK(zone.find(a)->hp > zone.find(b)->hp + 5);
  zone.queue_input(a, 1, {0, 1, 0}, {0, 0, 0.35f});
  zone.tick(++t, sink);
  CHECK_FALSE(zone.find(a)->sitting);
}

TEST_CASE("spawn groups populate a zone and respawn after their timer", "[combat][ai]") {
  ZoneDef def = test_zone();
  def.spawns.push_back({"ember_rat", 20, 20, 3, 5});
  Zone zone(def, content(), kDt, 12);
  RecordingSink sink;
  std::uint32_t t = 0;
  CHECK(zone.mob_count() == 3);
  const auto p = zone.add_player(at(20, 20, "Farmer", 3));
  zone.tick(++t, sink);
  EntityId victim = 0;
  zone.for_each_mob([&](const MobEntity& m) {
    if (victim == 0) victim = m.id;
  });
  zone.set_target(p, victim, sink);
  zone.set_auto_attack(p, true, sink);
  run_until(zone, sink, t, 20 * 120, [&] { return mob_gone(zone, victim); });
  REQUIRE(mob_gone(zone, victim));
  int alive = 0;
  zone.for_each_mob([&](const MobEntity& m) { alive += m.alive() ? 1 : 0; });
  CHECK(alive == 2);
  const std::uint32_t wait = zone.ticks(mob("ember_rat").respawn_seconds) + 5;
  for (std::uint32_t i = 0; i < wait; ++i) zone.tick(++t, sink);
  alive = 0;
  zone.for_each_mob([&](const MobEntity& m) { alive += m.alive() ? 1 : 0; });
  CHECK(alive == 3);
  CHECK(zone.find_mob(victim) == nullptr);  // respawns are new entities
}

TEST_CASE("an overkill finishing blow does not steal loot or XP", "[combat][loot]") {
  Zone zone(test_zone(), content(), kDt, 21);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto worker = zone.add_player(at(0, 0, "Worker", 3));
  const auto thief = zone.add_player(at(0, 2, "Sniper", 50));
  const auto m = zone.spawn_mob(mob("ember_rat"), {2, 0, 1});
  zone.tick(++t, sink);
  zone.set_target(worker, m, sink);
  zone.set_auto_attack(worker, true, sink);
  run_until(zone, sink, t, 20 * 60, [&] { return zone.find_mob(m)->hp < zone.find_mob(m)->stats.max_hp; });
  zone.set_auto_attack(worker, false, sink);
  REQUIRE(zone.find_mob(m)->alive());
  zone.find_mob(m)->hp = 1;  // the worker has done nearly everything
  const auto worker_xp = zone.find(worker)->xp;
  for (int i = 0; i < 20 && !mob_gone(zone, m); ++i) {
    zone.find(thief)->endurance = 100;
    zone.use_ability(thief, "rising_kick", m, sink);  // ~60 damage onto 1 HP
    run_until(zone, sink, t, 20 * 7, [&] { return mob_gone(zone, m); });
  }
  REQUIRE(mob_gone(zone, m));
  EntityId corpse = 0;
  zone.for_each_corpse([&](const CorpseEntity& c) { corpse = c.id; });
  REQUIRE(corpse != 0);
  CHECK(zone.find_corpse(corpse)->loot_char_id == zone.find(worker)->char_id);
  CHECK(zone.find(worker)->xp > worker_xp);
}

TEST_CASE("skills locked at the current level count as zero; trained values clamp to the cap", "[combat][skills]") {
  Zone zone(test_zone(), content(), kDt, 22);
  auto low = at(0, 0, "Fallen", 10);
  low.skills.push_back({"double_attack", 80});  // trained at a higher level, then lost levels
  const auto a = zone.add_player(low);
  CHECK(zone.find(a)->skill("double_attack") == 0);  // unlocks at 15
  auto high = at(5, 0, "Risen", 16);
  high.skills.push_back({"double_attack", 200});
  const auto b = zone.add_player(high);
  const int cap = skill_cap(*content().find_class("warrior")->skill("double_attack"), 16);
  CHECK(zone.find(b)->skill("double_attack") == cap);
  CHECK(zone.find(b)->skills.size() > 0);  // the stored training is kept
}

TEST_CASE("hand-placed mobs keep their own copy of a temporary definition", "[combat][ai]") {
  Zone zone(test_zone(), content(), kDt, 23);
  RecordingSink sink;
  EntityId id = 0;
  {
    MobDef tmp = mob("road_brigand");
    tmp.name = "a short-lived brigand";
    id = zone.spawn_mob(tmp, {3, 0, 0});
  }  // tmp is gone; the zone must not point at it
  const auto p = zone.add_player(at(0, 0, "Witness", 5));
  for (std::uint32_t t = 1; t <= 60; ++t) zone.tick(t, sink);
  REQUIRE(zone.find_mob(id));
  CHECK(zone.find_mob(id)->def->name == "a short-lived brigand");
  CHECK(zone.find_mob(id)->threat.contains(p));  // it thinks (aggro) using its own definition
}
