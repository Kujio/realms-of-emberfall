// Phase 7, zone side of groups: who earns a kill, shared XP and loot, group spells, group-aware threat,
// and the acceptance run: a group of six clears a dungeon.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>

#include "zone_support.hpp"

using namespace zt;

namespace {

template <class F>
int run(Zone& z, RecordingSink& sink, std::uint32_t& t, int limit, F done) {
  int n = 0;
  while (n < limit && !done()) {
    z.tick(++t, sink);
    ++n;
  }
  return n;
}

void group_all(Zone& zone, GroupRegistry& reg, const std::vector<EntityId>& who) {
  const PlayerEntity* lead = zone.find(who.front());
  for (std::size_t i = 1; i < who.size(); ++i) {
    const PlayerEntity* p = zone.find(who[i]);
    REQUIRE(reg.invite(lead->char_id, lead->name, p->char_id, 1e9).ok());
    REQUIRE(reg.accept(p->char_id, p->name).ok());
  }
  zone.set_groups(&reg);
}

MobDef weak(const char* id, int level) {
  MobDef m = mob(id);
  m.level = level;
  m.aggro_radius = 0;
  m.damage_mult = 0.05f;
  return m;
}

void fight(Zone& zone, RecordingSink& sink, std::uint32_t& t, const std::vector<EntityId>& who, EntityId m) {
  zone.tick(++t, sink);
  for (EntityId p : who) {
    zone.set_target(p, m, sink);
    zone.set_auto_attack(p, true, sink);
  }
  run(zone, sink, t, 20 * 300, [&] {
    const MobEntity* x = zone.find_mob(m);
    return !x || !x->alive();
  });
}

}  // namespace

TEST_CASE("ungrouped players: the top damager alone earns the kill", "[social][group][zone]") {
  Zone zone(test_zone(), content(), kDt, 71);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto a = zone.add_player(at(0, 0, "Hitter", 10));
  const auto b = zone.add_player(at(1, 0, "Helper", 10));
  const auto m = zone.spawn_mob(weak("road_brigand", 9), {2, 0, 0});
  const auto a0 = zone.find(a)->xp;
  const auto b0 = zone.find(b)->xp;
  // The helper lands a few blows, then stops; the hitter finishes the job.
  zone.tick(++t, sink);
  zone.set_target(b, m, sink);
  zone.set_auto_attack(b, true, sink);
  run(zone, sink, t, 20 * 4, [] { return false; });
  zone.set_auto_attack(b, false, sink);
  REQUIRE(zone.find_mob(m)->alive());
  fight(zone, sink, t, {a}, m);
  REQUIRE((!zone.find_mob(m) || !zone.find_mob(m)->alive()));
  CHECK(zone.find(a)->xp > a0);
  CHECK(zone.find(b)->xp == b0);  // no group, no share
  bool found = false;
  zone.for_each_corpse([&](const CorpseEntity& c) {
    if (c.player_corpse) return;
    found = true;
    CHECK(c.party == std::vector<std::int64_t>{zone.find(a)->char_id});
  });
  CHECK(found);
}

TEST_CASE("a group's combined damage beats a stronger soloer; members far away share nothing", "[social][group][zone]") {
  Zone zone(test_zone(), content(), kDt, 72);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto solo = zone.add_player(at(0, 0, "Loner", 13));
  const auto g1 = zone.add_player(at(1, 0, "Pairone", 12));
  const auto g2 = zone.add_player(at(1, 1, "Pairtwo", 12));
  const auto far = zone.add_player(at(400, 400, "Faraway", 12));
  GroupRegistry reg;
  group_all(zone, reg, {g1, g2, far});
  const MobDef foe = weak("road_brigand", 11);
  const auto m = zone.spawn_mob(foe, {2, 0, 0});
  const auto s0 = zone.find(solo)->xp;
  const auto g10 = zone.find(g1)->xp;
  const auto g20 = zone.find(g2)->xp;
  const auto f0 = zone.find(far)->xp;
  // The loner is a level higher, but the two members together do more.
  fight(zone, sink, t, {solo, g1, g2}, m);
  const auto xp1 = zone.find(g1)->xp - g10;
  const auto xp2 = zone.find(g2)->xp - g20;
  INFO("solo +" << zone.find(solo)->xp - s0 << " g1 +" << xp1 << " g2 +" << xp2);
  CHECK(zone.find(solo)->xp == s0);
  CHECK(xp1 > 0);
  CHECK(xp1 == xp2);  // same level, same share
  CHECK(zone.find(far)->xp == f0);
  // Two sharers: each gets (1 + bonus) / 2 of the solo value.
  const auto base = kill_xp(12, 11, content().rules.xp, content().rules.con, toughness_xp_mult(static_cast<double>(foe.hp_mult), content().rules.xp));
  CHECK(xp1 == static_cast<std::uint64_t>(std::floor(static_cast<double>(base) * (1.0 + content().rules.xp.group_bonus_per_member) / 2.0)));
  zone.for_each_corpse([&](const CorpseEntity& c) {
    if (!c.player_corpse) CHECK(c.party.size() == 2);  // the faraway member is not in the loot party
  });
}

TEST_CASE("group songs and spells reach nearby group members", "[social][group][zone][spells]") {
  Zone zone(test_zone(), content(), kDt, 73);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto bard = zone.add_player(with_spells(as_class(at(0, 0, "Singer", 10), "bard"), {"anthem_of_swiftblades"}));
  const auto mate = zone.add_player(at(4, 0, "Mate", 10));
  const auto stranger = zone.add_player(at(4, 2, "Stranger", 10));
  GroupRegistry reg;
  group_all(zone, reg, {bard, mate});
  zone.tick(++t, sink);
  zone.cast_spell(bard, 0, 0, sink);
  run(zone, sink, t, 20 * 8, [] { return false; });
  auto has = [&](EntityId who) {
    const auto& bs = zone.find(who)->buffs;
    return std::any_of(bs.begin(), bs.end(), [](const ActiveBuff& b) { return b.ability_id == "anthem_of_swiftblades"; });
  };
  CHECK(has(bard));
  CHECK(has(mate));
  CHECK_FALSE(has(stranger));
}

TEST_CASE("group-aware threat: a mob that turns on one member notices the members beside it", "[social][group][zone]") {
  Zone zone(test_zone(), content(), kDt, 74);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto tank = zone.add_player(at(0, 0, "Tank", 10));
  const auto healer = zone.add_player(as_class(at(3, 0, "Healer", 10), "cleric"));
  const auto other = zone.add_player(at(5, 0, "Bystander", 10));
  GroupRegistry reg;
  group_all(zone, reg, {tank, healer});
  const auto m = zone.spawn_mob(weak("road_brigand", 9), {1.5f, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(tank, m, sink);
  zone.set_auto_attack(tank, true, sink);
  run(zone, sink, t, 60, [&] { return zone.find_mob(m)->threat.contains(tank); });
  const MobEntity* x = zone.find_mob(m);
  REQUIRE(x->threat.contains(tank));
  CHECK(x->threat.contains(healer));       // on the list, so the mob turns to them if the tank falls
  CHECK_FALSE(x->threat.contains(other));  // not grouped: not involved
  CHECK(x->threat.top() == tank);          // a token amount: the tank still holds it
}

TEST_CASE("a group of six clears a dungeon, sharing experience and loot", "[social][group][zone][dungeon]") {
  const ZoneDef* def = content().find_zone("rattlebone_warren");
  REQUIRE(def);
  Zone zone(*def, content(), kDt, 75);
  RecordingSink sink;
  std::uint32_t t = 0;
  const int level = 9;  // the boss (11) cons yellow; rats (3) are grey and give nothing
  const char* classes[] = {"warrior", "paladin", "shadow_knight", "rogue", "monk", "ranger"};
  std::vector<EntityId> party;
  for (int i = 0; i < 6; ++i) {
    auto init = geared(as_class(at(def->safe_spawn.x, def->safe_spawn.z, "x", level), classes[i]));
    init.name = std::string("Raider") + static_cast<char>('a' + i);
    init.bind_zone = def->id;
    init.bind_pos = {def->safe_spawn.x, def->safe_spawn.y, def->safe_spawn.z};
    party.push_back(zone.add_player(init));
  }
  GroupRegistry reg;
  group_all(zone, reg, party);
  REQUIRE(reg.group_of(zone.find(party[0])->char_id)->members.size() == 6);
  REQUIRE(reg.set_loot_rule(zone.find(party[0])->char_id, LootRule::NeedGreed).ok());

  std::set<EntityId> to_kill;
  zone.for_each_mob([&](const MobEntity& m) {
    if (m.alive() && !m.is_pet()) to_kill.insert(m.id);
  });
  REQUIRE(to_kill.size() >= 5);
  std::map<EntityId, std::uint64_t> xp_before;
  for (EntityId p : party) xp_before[p] = zone.find(p)->xp;
  int kills = 0;
  int shared_kills = 0;    // kills that fed every member standing at the end of the fight
  int expected_shared = 0;  // kills worth experience at the party's level
  int full_parties = 0;     // kills whose loot all six shared
  std::set<std::uint32_t> rolls_seen;
  int guard = 0;
  while (!to_kill.empty() && guard++ < 200) {
    // The nearest target still standing (adds a boss calls join the list).
    zone.for_each_mob([&](const MobEntity& m) {
      if (m.alive() && !m.is_pet() && m.add_of != 0) to_kill.insert(m.id);
    });
    std::erase_if(to_kill, [&](EntityId id) { const MobEntity* m = zone.find_mob(id); return !m || !m->alive(); });
    if (to_kill.empty()) break;
    const EntityId target = *to_kill.begin();
    const Vec3 at_mob = zone.find_mob(target)->pos;
    for (std::size_t i = 0; i < party.size(); ++i) {
      PlayerEntity* p = zone.find(party[i]);
      if (p->dead) zone.respawn(p->id, sink);  // bound inside: back on their feet at the entrance
      p = zone.find(party[i]);
      p->hp = p->stats.max_hp;  // rested between pulls
      p->pos = {at_mob.x + 1.0f + 0.3f * static_cast<float>(i), at_mob.y, at_mob.z + 0.5f};
    }
    std::map<EntityId, std::uint64_t> before;
    for (EntityId p : party) before[p] = zone.find(p)->xp;
    const int mob_level = zone.find_mob(target)->def->level;
    fight(zone, sink, t, party, target);
    const MobEntity* m = zone.find_mob(target);
    if (m && m->alive()) continue;  // try again next round
    ++kills;
    int gained = 0;
    int standing = 0;
    bool every_survivor_gained = true;
    for (EntityId p : party) {
      const bool got = zone.find(p)->xp > before[p];
      if (got) ++gained;
      if (!zone.find(p)->dead) {
        ++standing;
        every_survivor_gained = every_survivor_gained && got;
      }
    }
    // The loot party is every member standing near the kill.
    EntityId newest = 0;
    zone.for_each_corpse([&](const CorpseEntity& c) {
      if (!c.player_corpse && c.id > newest) newest = c.id;
    });
    if (const CorpseEntity* c = newest ? zone.find_corpse(newest) : nullptr) {
      CHECK(static_cast<int>(c->party.size()) == standing);
      if (static_cast<int>(c->party.size()) == 6) ++full_parties;
    }
    if (kill_xp(level, mob_level, content().rules.xp, content().rules.con) > 0) {
      ++expected_shared;
      if (every_survivor_gained && standing > 0) ++shared_kills;
    } else {
      CHECK(gained == 0);  // grey to the group: nobody gains
    }
    // Answer every loot roll (need for the first member, greed for the rest).
    for (std::size_t i = 0; i < party.size(); ++i)
      for (const auto& r : sink.all<proto::LootRoll>(party[i]))
        if (!rolls_seen.contains(r.roll_id * 8 + static_cast<std::uint32_t>(i))) {
          rolls_seen.insert(r.roll_id * 8 + static_cast<std::uint32_t>(i));
          zone.loot_roll_choice(party[i], r.roll_id, i == 0 && r.can_need ? RollChoice::Need : RollChoice::Greed, sink);
        }
  }
  INFO("kills " << kills << ", shared by all six " << shared_kills << ", rolls " << rolls_seen.size());
  CHECK(to_kill.empty());  // cleared, boss included
  CHECK(kills >= 5);
  CHECK(expected_shared >= 4);
  CHECK(shared_kills == expected_shared);  // every worthwhile kill fed every member still standing
  for (EntityId p : party) CHECK(zone.find(p)->xp > xp_before[p]);
  CHECK(full_parties >= 4);
  // Coin from the kills went to the group.
  std::uint64_t coin = 0;
  for (EntityId p : party) coin += zone.find(p)->coin;
  CHECK(coin > 0);
}
