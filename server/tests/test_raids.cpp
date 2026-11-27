// Phase 14: raids. The pure registry (forming, joining, leaving, leading, keeping in step with the
// groups, restoring), the experience rule, a raid kill in a zone (credit, shared experience, master loot
// and need before greed), and raids over the wire (commands, RaidState, raid chat, a restart).
// Phase 15: the main assist and target marks, raid lockouts, raid-only dungeons.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <limits>

#include <unistd.h>

#include "server_support.hpp"
#include "zone_support.hpp"

using namespace zt;

namespace {

// Groups of players: each inner list is one group, its first member leading.
GroupRegistry make_groups(const std::vector<std::vector<std::int64_t>>& groups) {
  GroupRegistry reg(6);
  for (const auto& g : groups)
    for (std::size_t i = 1; i < g.size(); ++i) {
      REQUIRE(reg.invite(g[0], "L" + std::to_string(g[0]), g[i], 1e9).ok());
      REQUIRE(reg.accept(g[i], "M" + std::to_string(g[i])).ok());
    }
  return reg;
}

std::uint32_t gid(const GroupRegistry& reg, std::int64_t who) { return reg.group_of(who)->id; }

}  // namespace

// ---------------------------------------------------------------------------- the pure registry

TEST_CASE("raids form from whole groups: only leaders invite and accept", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}, {7, 8}});
  RaidRegistry raids(3);
  // A member who does not lead cannot found a raid, nor be invited (their leader is).
  CHECK(raids.invite(groups, 2, "B", 3, 1e9).error == RaidError::NotGroupLeader);
  CHECK(raids.invite(groups, 1, "A", 4, 1e9).error == RaidError::TargetNotLeader);
  CHECK(raids.invite(groups, 1, "A", 2, 1e9).error == RaidError::SameGroup);
  CHECK(raids.invite(groups, 1, "A", 99, 1e9).error == RaidError::TargetNotGrouped);
  CHECK(raids.invite(groups, 99, "X", 3, 1e9).error == RaidError::NotInGroup);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  CHECK(raids.invite(groups, 5, "E", 3, 1e9).error == RaidError::AlreadyInvited);
  const RaidChange c = raids.accept(groups, 3, 0.0);
  REQUIRE(c.ok());
  const Raid* r = raids.find(c.raid);
  REQUIRE(r);
  CHECK(r->leader == 1);
  CHECK(r->groups == std::vector<std::uint32_t>{gid(groups, 1), gid(groups, 3)});
  CHECK(r->loot == RaidLoot::Master);
  CHECK(raids.raid_of(groups, 4) == r);  // in the raid through their group
  CHECK(RaidRegistry::members(*r, groups) == std::vector<std::int64_t>{1, 2, 3, 4});
  // Only the raid leader invites now; another group's leader in it cannot.
  CHECK(raids.invite(groups, 3, "C", 5, 1e9).error == RaidError::NotRaidLeader);
  CHECK(raids.invite(groups, 1, "A", 3, 1e9).error == RaidError::TargetInRaid);
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(raids.accept(groups, 5, 0.0).ok());
  // Full at three groups.
  CHECK(raids.invite(groups, 1, "A", 7, 1e9).error == RaidError::Full);
  CHECK(raids.accept(groups, 7, 0.0).error == RaidError::NoInvite);
  CHECK(raids.decline(7) == 0);
}

TEST_CASE("an invitation is for one raid and one group", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}, {7, 8}});
  RaidRegistry raids(12);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  REQUIRE(raids.accept(groups, 3, 0.0).ok());
  // Invited to that raid, which then disbands: accepting does not found a new one.
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(raids.disband(groups, 1).ok());
  CHECK(raids.accept(groups, 5, 0.0).error == RaidError::NoInvite);
  CHECK(raids.all().empty());
  // Invited as the leader of one group, now leading another: the invitation does not carry over.
  REQUIRE(raids.invite(groups, 1, "A", 7, 1e9).ok());
  REQUIRE(groups.leave(7).ok());                          // 7 leaves their group (8 is left alone, so it goes)
  REQUIRE(groups.invite(7, "G", 2, 1e9).ok() == false);   // 2 is grouped; 7 founds a new group with someone free
  REQUIRE(groups.invite(7, "G", 99, 1e9).ok());
  REQUIRE(groups.accept(99, "Z").ok());
  REQUIRE(groups.group_of(7)->leader == 7);
  CHECK(raids.accept(groups, 7, 0.0).error == RaidError::NoInvite);
  CHECK(raids.all().empty());
}

TEST_CASE("an invitation is checked again when accepted", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}});
  RaidRegistry raids(12);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  // The inviter hands their group's lead over meanwhile: the invitation is no longer good.
  REQUIRE(groups.make_leader(1, 2).ok());
  CHECK(raids.accept(groups, 3, 0.0).error == RaidError::NoInvite);
  CHECK(raids.all().empty());
  // Declining and expiring.
  REQUIRE(raids.invite(groups, 2, "B", 5, 10.0).ok());
  CHECK(raids.decline(5) == 2);
  REQUIRE(raids.invite(groups, 2, "B", 5, 10.0).ok());
  CHECK(raids.expire_invites(5.0).empty());
  const auto gone = raids.expire_invites(10.0);
  REQUIRE(gone.size() == 1);
  CHECK(gone[0].first == 5);
  CHECK_FALSE(raids.invite_for(5));
  // A character leaving the world drops every invitation to or from them.
  REQUIRE(raids.invite(groups, 2, "B", 5, 1e9).ok());
  raids.drop_invites(2);
  CHECK_FALSE(raids.invite_for(5));
}

TEST_CASE("an invitation lapses at its deadline, and when its invitee or inviter loses the lead", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}});
  RaidRegistry raids(12);
  // Past its deadline it cannot be accepted, though the periodic expiry has not removed it yet.
  REQUIRE(raids.invite(groups, 1, "A", 3, 10.0).ok());
  CHECK(raids.accept(groups, 3, 10.0).error == RaidError::NoInvite);
  CHECK_FALSE(raids.invite_for(3));
  CHECK(raids.all().empty());
  REQUIRE(raids.invite(groups, 1, "A", 3, 10.0).ok());
  REQUIRE(raids.accept(groups, 3, 9.5).ok());
  // The invitee hands their group's lead over and takes it back: the invitation lapsed at the first change.
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(groups.make_leader(5, 6).ok());
  raids.reconcile(groups);
  REQUIRE(groups.make_leader(6, 5).ok());
  raids.reconcile(groups);
  CHECK_FALSE(raids.invite_for(5));
  CHECK(raids.accept(groups, 5, 0.0).error == RaidError::NoInvite);
  // Likewise the raid leader handing the raid's lead away and taking it back.
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(raids.make_leader(groups, 1, 3).ok());
  raids.prune_invites(groups);
  REQUIRE(raids.make_leader(groups, 3, 1).ok());
  raids.prune_invites(groups);
  CHECK_FALSE(raids.invite_for(5));
  // An invitation that still stands survives a change elsewhere.
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(groups.make_leader(3, 4).ok());
  raids.reconcile(groups);
  CHECK(raids.invite_for(5));
  CHECK(raids.accept(groups, 5, 0.0).ok());
}

TEST_CASE("groups leave, are removed, and a raid of one group dissolves", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}});
  RaidRegistry raids(12);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  const std::uint32_t id = raids.accept(groups, 3, 0.0).raid;
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(raids.accept(groups, 5, 0.0).ok());
  // Only a group's leader takes it out.
  CHECK(raids.leave(groups, 4).error == RaidError::NotGroupLeader);
  // The raid leader removes a group (any member names it), but not their own.
  CHECK(raids.kick(groups, 3, gid(groups, 5)).error == RaidError::NotRaidLeader);
  CHECK(raids.kick(groups, 1, gid(groups, 1)).error == RaidError::SameGroup);
  const RaidChange k = raids.kick(groups, 1, gid(groups, 5));
  REQUIRE(k.ok());
  CHECK(std::find(k.groups.begin(), k.groups.end(), gid(groups, 5)) != k.groups.end());  // told they are out
  CHECK_FALSE(raids.raid_of(groups, 6));
  // One more group leaving leaves one: the raid dissolves.
  const RaidChange l = raids.leave(groups, 3);
  REQUIRE(l.ok());
  CHECK(l.disbanded);
  CHECK(l.gone == id);
  CHECK(raids.all().empty());
  CHECK_FALSE(raids.raid_of(groups, 1));
}

TEST_CASE("the raid leader hands over, picks the loot rule and disbands", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}});
  RaidRegistry raids(12);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  REQUIRE(raids.accept(groups, 3, 0.0).ok());
  CHECK(raids.set_loot(groups, 3, RaidLoot::NeedGreed).error == RaidError::NotRaidLeader);
  REQUIRE(raids.set_loot(groups, 1, RaidLoot::NeedGreed).ok());
  CHECK(raids.raid_of(groups, 1)->loot == RaidLoot::NeedGreed);
  CHECK(raids.make_leader(groups, 1, 99).error == RaidError::NotMember);
  REQUIRE(groups.go_offline(4, 0.0).ok());
  CHECK(raids.make_leader(groups, 1, 4).error == RaidError::TargetOffline);
  REQUIRE(raids.make_leader(groups, 1, 2).ok());  // any member, not only a group leader
  CHECK(raids.raid_of(groups, 1)->leader == 2);
  CHECK(raids.disband(groups, 1).error == RaidError::NotRaidLeader);
  const RaidChange d = raids.disband(groups, 2);
  REQUIRE(d.ok());
  CHECK(d.disbanded);
  CHECK(raids.all().empty());
}

TEST_CASE("reconcile keeps a raid in step with its groups", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2, 7}, {3, 4}, {5, 6}});
  RaidRegistry raids(12);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  const std::uint32_t id = raids.accept(groups, 3, 0.0).raid;
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(raids.accept(groups, 5, 0.0).ok());
  CHECK(raids.reconcile(groups).empty());  // nothing changed
  // The leader goes offline: the lead passes to the first member online (a group leader first).
  REQUIRE(groups.go_offline(1, 0.0).ok());
  auto ch = raids.reconcile(groups);
  REQUIRE(ch.size() == 1);
  CHECK(raids.find(id)->leader == 2);  // their group's new leader (groups hand the lead on too)
  // A group disbands: it leaves the raid.
  REQUIRE(groups.leave(6).ok());  // a group of one disbands
  ch = raids.reconcile(groups);
  REQUIRE(ch.size() == 1);
  CHECK(raids.find(id)->groups.size() == 2);
  // The raid leader leaves their group: they are out of the raid, and the lead passes to the first
  // member online (their old group's new leader).
  REQUIRE(groups.leave(2).ok());
  ch = raids.reconcile(groups);
  REQUIRE(ch.size() == 1);
  CHECK(raids.find(id)->leader == 7);
  CHECK_FALSE(raids.raid_of(groups, 2));
  // Everyone offline: the leader stays (a raid kept across a restart keeps its leader).
  for (std::int64_t who : {7, 3, 4}) REQUIRE(groups.go_offline(who, 0.0).ok());
  raids.reconcile(groups);
  const std::int64_t kept = raids.find(id)->leader;
  CHECK(raids.reconcile(groups).empty());
  CHECK(raids.find(id)->leader == kept);
  // The last group but one disbands: the raid dissolves.
  REQUIRE(groups.leave(4).ok());
  ch = raids.reconcile(groups);
  REQUIRE(ch.size() == 1);
  CHECK(ch[0].disbanded);
  CHECK(raids.all().empty());
}

TEST_CASE("restore puts a saved raid back, without groups that are gone", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}});
  RaidRegistry raids(12);
  Raid r;
  r.id = 9;
  r.leader = 3;
  r.loot = RaidLoot::NeedGreed;
  r.groups = {gid(groups, 1), 4242, gid(groups, 3), gid(groups, 1)};
  REQUIRE(raids.restore(r, groups));
  const Raid* back = raids.find(9);
  REQUIRE(back);
  CHECK(back->groups == std::vector<std::uint32_t>{gid(groups, 1), gid(groups, 3)});
  CHECK(back->leader == 3);
  CHECK(back->loot == RaidLoot::NeedGreed);
  // The same id again, a group already in a raid, or fewer than two groups left: refused.
  CHECK_FALSE(raids.restore(r, groups));
  Raid one;
  one.id = 10;
  one.leader = 5;
  one.groups = {gid(groups, 5), gid(groups, 3)};
  CHECK_FALSE(raids.restore(one, groups));
  // A leader no longer in it is replaced by the first group's leader.
  RaidRegistry fresh(12);
  Raid lost = r;
  lost.leader = 77;
  REQUIRE(fresh.restore(lost, groups));
  CHECK(fresh.find(9)->leader == 1);
  // New raids never reuse a restored id.
  REQUIRE(fresh.invite(groups, 5, "E", 1, 1e9).error == RaidError::TargetInRaid);
}

TEST_CASE("a raid's experience bonus counts no more than a full group", "[raid][rules]") {
  XpRules xp;
  xp.group_bonus_per_member = 0.3;
  const std::vector<int> six(6, 50);
  const std::vector<int> eighteen(18, 50);
  const auto g = split_group_xp(1000, six, xp);
  const auto r = split_group_xp(1000, eighteen, xp, 6);
  std::uint64_t gsum = 0;
  std::uint64_t rsum = 0;
  for (auto v : g) gsum += v;
  for (auto v : r) rsum += v;
  CHECK(gsum <= 2500);                       // 1 + 0.3 x 5 (each share rounds down)
  CHECK(gsum >= 2500 - 6);
  CHECK(rsum <= 2500);                       // the same pool, split three times as many ways
  CHECK(rsum >= 2500 - 18);
  CHECK(r[0] == g[0] / 3);
  const auto uncapped = split_group_xp(1000, eighteen, xp);
  CHECK(uncapped[0] > r[0]);  // without the cap a raid would earn far more
}

// ---------------------------------------------------------------------------- a raid kill in a zone

namespace {

struct RaidZone {
  Content c = content();
  Zone zone;
  RecordingSink sink;
  std::uint32_t t = 0;
  GroupRegistry groups{6};
  RaidRegistry raids{12};
  std::vector<EntityId> a;  // group 1 (a[0] leads it and the raid)
  std::vector<EntityId> b;  // group 2 (b[0] leads it)
  explicit RaidZone(const std::vector<std::pair<std::string, int>>& drops, std::uint64_t seed, bool raid_zone = false)
      : c(with_drops(drops)), zone(raid_zone ? raid_dungeon() : test_zone(), c, kDt, seed) {
    a = {zone.add_player(at(0, 0, "Raidlead", 20)), zone.add_player(at(1, 0, "Annis", 20))};
    b = {zone.add_player(at(0, 1, "Bregor", 20)), zone.add_player(as_class(at(1, 1, "Bryn", 20), "wizard"))};
    for (const auto& g : {a, b}) {
      const PlayerEntity* lead = zone.find(g[0]);
      for (std::size_t i = 1; i < g.size(); ++i) {
        const PlayerEntity* p = zone.find(g[i]);
        REQUIRE(groups.invite(lead->char_id, lead->name, p->char_id, 1e9).ok());
        REQUIRE(groups.accept(p->char_id, p->name).ok());
      }
    }
    REQUIRE(raids.invite(groups, cid(a[0]), "Raidlead", cid(b[0]), 1e9).ok());
    REQUIRE(raids.accept(groups, cid(b[0]), 0.0).ok());
    zone.set_groups(&groups);
    zone.set_raids(&raids);
  }
  static ZoneDef raid_dungeon() {
    ZoneDef z = test_zone();
    z.raid = true;
    return z;
  }
  static Content with_drops(const std::vector<std::pair<std::string, int>>& drops) {
    Content cc = content();
    for (auto& t : cc.loot_tables)
      if (t.id == "road_brigand_loot") {
        t.coin_min = t.coin_max = 400;
        t.drops.clear();
        for (const auto& [item, n] : drops) t.drops.push_back({item, 1.0f, n, n});
        t.pools.clear();
        t.max_drops = 8;
      }
    return cc;
  }
  std::int64_t cid(EntityId e) { return zone.find(e)->char_id; }
  // Only Bryn (group 2) fights; everyone stands at the kill. A `boss` is named and a boss (raid lockouts).
  EntityId kill(bool boss = false) {
    MobDef foe = mob("road_brigand");
    foe.level = 20;
    foe.boss = foe.named = boss;
    foe.aggro_radius = 0;
    foe.damage_mult = 0.05f;
    const auto m = zone.spawn_mob(foe, {2, 0, 0});
    zone.tick(++t, sink);
    zone.set_target(b[1], m, sink);
    zone.set_auto_attack(b[1], true, sink);
    zone.find(b[1])->pos = {2, 0, 1};
    for (int i = 0; i < 20 * 300; ++i) {
      const MobEntity* x = zone.find_mob(m);
      if (!x || !x->alive()) break;
      zone.tick(++t, sink);
    }
    EntityId corpse = 0;
    zone.for_each_corpse([&](const CorpseEntity& cc) {
      if (!cc.player_corpse && cc.id > corpse) corpse = cc.id;  // the newest
    });
    return corpse;
  }
  const CorpseEntity* corpse(EntityId id) {
    const CorpseEntity* out = nullptr;
    zone.for_each_corpse([&](const CorpseEntity& cc) {
      if (cc.id == id) out = &cc;
    });
    return out;
  }
};

}  // namespace

TEST_CASE("a raid kill: the whole raid shares the credit, the experience and the coin; master loot", "[raid][zone][loot]") {
  RaidZone rz({{"plate_t1_chest", 1}, {"rat_whisker", 1}}, 141);
  std::map<EntityId, std::uint64_t> xp0;
  for (EntityId e : {rz.a[0], rz.a[1], rz.b[0], rz.b[1]}) xp0[e] = rz.zone.find(e)->xp;
  const EntityId corpse = rz.kill();
  REQUIRE(corpse != 0);
  // Every raider at the kill gains experience, from both groups, though only Bryn fought.
  for (EntityId e : {rz.a[0], rz.a[1], rz.b[0], rz.b[1]}) CHECK(rz.zone.find(e)->xp > xp0[e]);
  // The coin is split four ways at once.
  for (EntityId e : {rz.a[0], rz.a[1], rz.b[0], rz.b[1]}) CHECK(rz.zone.find(e)->coin == 100);
  // Master loot: the raid leader holds every item.
  const CorpseEntity* c = nullptr;
  rz.zone.for_each_corpse([&](const CorpseEntity& cc) {
    if (cc.id == corpse) c = &cc;
  });
  REQUIRE(c);
  CHECK(c->party.size() == 4);
  CHECK(c->master == rz.cid(rz.a[0]));
  CHECK(rz.sink.chat_count(rz.b[1], "Raidlead holds this kill's loot.") == 1);
  // Nobody else may take an item; the master sees the party and hands each one out.
  rz.zone.loot_open(rz.b[1], corpse, rz.sink);
  rz.zone.loot_take(rz.b[1], corpse, kLootAll, rz.sink);
  CHECK(count_of(*rz.zone.find(rz.b[1]), "plate_t1_chest") == 0);
  rz.zone.loot_close(rz.b[1], corpse, rz.sink);
  rz.zone.loot_open(rz.a[0], corpse, rz.sink);
  auto w = rz.sink.last<proto::LootWindow>(rz.a[0]);
  REQUIRE(w);
  CHECK(w->master);
  CHECK(w->party.size() == 4);
  std::uint8_t plate = 255;
  std::uint8_t whisker = 255;
  for (const auto& e : w->items) {
    if (e.item_id == "plate_t1_chest") plate = e.index;
    if (e.item_id == "rat_whisker") whisker = e.index;
  }
  REQUIRE(plate != 255);
  REQUIRE(whisker != 255);
  // Someone who was not at the kill, and someone who is not here, are refused.
  rz.zone.loot_give(rz.a[0], corpse, plate, "Nobody", rz.sink);
  CHECK(rz.sink.chat_count(rz.a[0], "Nobody was not at this kill") == 1);
  // Only the master hands out.
  rz.zone.loot_give(rz.b[0], corpse, plate, "Bregor", rz.sink);
  CHECK(count_of(*rz.zone.find(rz.b[0]), "plate_t1_chest") == 0);
  rz.zone.loot_give(rz.a[0], corpse, plate, "bregor", rz.sink);  // names compare without case
  CHECK(count_of(*rz.zone.find(rz.b[0]), "plate_t1_chest") == 1);
  const std::string plate_name = rz.c.find_item("plate_t1_chest")->name;
  CHECK(rz.sink.chat_count(rz.b[0], "Raidlead gives you " + plate_name) == 1);
  CHECK(rz.sink.chat_count(rz.b[1], "Raidlead gives " + plate_name + " to Bregor") == 1);  // the raid hears where it went
  rz.zone.loot_give(rz.a[0], corpse, plate, "Bryn", rz.sink);      // already gone
  CHECK(rz.sink.chat_count(rz.a[0], "That item is gone.") == 1);
  // The master may keep an item too, and the raid hears that as well.
  rz.zone.loot_take(rz.a[0], corpse, whisker, rz.sink);
  CHECK(count_of(*rz.zone.find(rz.a[0]), "rat_whisker") == 1);
  const std::string whisker_name = rz.c.find_item("rat_whisker")->name;
  CHECK(rz.sink.chat_count(rz.b[1], "Raidlead keeps " + whisker_name) == 1);
  CHECK(rz.sink.chat_count(rz.a[0], "Raidlead keeps") == 0);
}

TEST_CASE("a raid set to need before greed rolls among every raider at the kill", "[raid][zone][loot]") {
  RaidZone rz({{"plate_t1_chest", 1}}, 142);
  REQUIRE(rz.raids.set_loot(rz.groups, rz.cid(rz.a[0]), RaidLoot::NeedGreed).ok());
  const EntityId corpse = rz.kill();
  REQUIRE(corpse != 0);
  for (EntityId e : {rz.a[0], rz.a[1], rz.b[0], rz.b[1]}) {
    auto roll = rz.sink.last<proto::LootRoll>(e);
    REQUIRE(roll);
    rz.zone.loot_roll_choice(e, roll->roll_id, e == rz.a[1] ? RollChoice::Need : RollChoice::Pass, rz.sink);
  }
  CHECK(count_of(*rz.zone.find(rz.a[1]), "plate_t1_chest") == 1);
  CHECK(rz.sink.chat_count(rz.b[1], "Annis wins") == 1);
}

TEST_CASE("master loot passes on when the master looter leaves; a raider far from the kill shares nothing", "[raid][zone][loot]") {
  RaidZone rz({{"plate_t1_chest", 1}}, 144);
  // The raid leader stands well away from the fight: not at the kill, so not in its party, and not its master.
  rz.zone.find(rz.a[0])->pos = {100, 0, 100};  // about 140 m from the kill (share range: 120 m)
  const EntityId corpse = rz.kill();
  REQUIRE(corpse != 0);
  const CorpseEntity* c = nullptr;
  rz.zone.for_each_corpse([&](const CorpseEntity& cc) {
    if (cc.id == corpse) c = &cc;
  });
  REQUIRE(c);
  CHECK_FALSE(c->shared_with(rz.cid(rz.a[0])));
  CHECK(c->party.size() == 3);
  const std::int64_t first_master = c->master;
  CHECK(first_master != rz.cid(rz.a[0]));
  // That master leaves the zone: whoever opens the corpse next among the raiders becomes the master.
  EntityId gone = 0;
  for (EntityId e : {rz.a[1], rz.b[0], rz.b[1]})
    if (rz.cid(e) == first_master) gone = e;
  REQUIRE(gone != 0);
  rz.zone.remove_player(gone, rz.sink);
  // Opening the corpse hands the job on (to the raid leader if at the kill, else the first raider still here).
  const EntityId opener = gone == rz.b[0] ? rz.b[1] : rz.b[0];
  rz.zone.loot_open(opener, corpse, rz.sink);
  rz.zone.loot_close(opener, corpse, rz.sink);
  REQUIRE(c->master != first_master);
  EntityId next = 0;
  for (EntityId e : {rz.a[1], rz.b[0], rz.b[1]})
    if (e != gone && rz.cid(e) == c->master) next = e;
  REQUIRE(next != 0);
  rz.zone.loot_open(next, corpse, rz.sink);
  auto w = rz.sink.last<proto::LootWindow>(next);
  REQUIRE(w);
  CHECK(w->master);
  std::uint8_t plate = 255;
  for (const auto& e : w->items)
    if (e.item_id == "plate_t1_chest") plate = e.index;
  REQUIRE(plate != 255);
  EntityId to = 0;
  for (EntityId e : {rz.a[1], rz.b[0], rz.b[1]})
    if (to == 0 && e != gone && e != next) to = e;
  REQUIRE(to != 0);
  rz.zone.loot_give(next, corpse, plate, rz.zone.find(to)->name, rz.sink);
  CHECK(count_of(*rz.zone.find(to), "plate_t1_chest") == 1);
}

TEST_CASE("a master looter who dies hands the job to a living raider at the kill", "[raid][zone][loot]") {
  RaidZone rz({{"plate_t1_chest", 1}}, 146);
  const EntityId corpse = rz.kill();
  REQUIRE(corpse != 0);
  const CorpseEntity* c = nullptr;
  rz.zone.for_each_corpse([&](const CorpseEntity& cc) {
    if (cc.id == corpse) c = &cc;
  });
  REQUIRE(c);
  REQUIRE(c->master == rz.cid(rz.a[0]));
  // The raid leader, the master looter, dies at the kill: dead, they cannot open the corpse.
  PlayerEntity* lead = rz.zone.find(rz.a[0]);
  lead->dead = true;
  lead->hp = 0;
  rz.zone.loot_open(rz.b[0], corpse, rz.sink);
  rz.zone.loot_close(rz.b[0], corpse, rz.sink);
  REQUIRE(c->master != rz.cid(rz.a[0]));
  REQUIRE(c->master != 0);
  EntityId next = 0;
  for (EntityId e : {rz.a[1], rz.b[0], rz.b[1]})
    if (rz.cid(e) == c->master) next = e;
  REQUIRE(next != 0);
  rz.zone.loot_open(next, corpse, rz.sink);
  auto w = rz.sink.last<proto::LootWindow>(next);
  REQUIRE(w);
  CHECK(w->master);
  REQUIRE(w->items.size() == 1);
  const EntityId to = next == rz.b[1] ? rz.b[0] : rz.b[1];
  rz.zone.loot_give(next, corpse, w->items[0].index, rz.zone.find(to)->name, rz.sink);
  CHECK(count_of(*rz.zone.find(to), "plate_t1_chest") == 1);
}

TEST_CASE("a raid kill with nobody from the raid at it drops no loot", "[raid][zone][loot]") {
  RaidZone rz({{"rat_whisker", 1}}, 147);
  MobDef foe = mob("road_brigand");
  foe.level = 20;
  foe.aggro_radius = 0;
  foe.damage_mult = 0.0f;
  const auto m = rz.zone.spawn_mob(foe, {2, 0, 0});
  rz.zone.tick(++rz.t, rz.sink);
  // Bryn's damage wins it, but every raider has gone far away by the time it dies.
  for (EntityId e : {rz.a[0], rz.a[1], rz.b[0], rz.b[1]}) rz.zone.find(e)->pos = {150, 0, 150};
  rz.zone.find_mob(m)->damage_by.emplace_back(rz.cid(rz.b[1]), 500);
  rz.zone.gm_kill_mob(m, rz.sink);
  int corpses = 0;
  rz.zone.for_each_corpse([&](const CorpseEntity& cc) {
    if (cc.player_corpse) return;
    ++corpses;
    CHECK(cc.party.empty());
    CHECK(cc.items.empty());
    CHECK(cc.coin == 0);
  });
  CHECK(corpses == 1);
}

TEST_CASE("a raid's top damager who wandered off leaves the corpse to the raider who stayed", "[raid][zone][loot]") {
  RaidZone rz({{"rat_whisker", 1}}, 145);
  // Bryn did most of the work earlier and is now far away; Bregor finishes the kill alone, the rest are far off.
  MobDef foe = mob("road_brigand");
  foe.level = 20;
  foe.aggro_radius = 0;
  foe.damage_mult = 0.0f;
  const auto m = rz.zone.spawn_mob(foe, {2, 0, 0});
  rz.zone.tick(++rz.t, rz.sink);
  for (EntityId e : {rz.a[0], rz.a[1]}) rz.zone.find(e)->pos = {-150, 0, -150};
  rz.zone.find(rz.b[1])->pos = {150, 0, 150};
  rz.zone.find_mob(m)->damage_by.emplace_back(rz.cid(rz.b[1]), 100000);  // far more than Bregor will do
  rz.zone.set_target(rz.b[0], m, rz.sink);
  rz.zone.set_auto_attack(rz.b[0], true, rz.sink);
  rz.zone.find(rz.b[0])->pos = {2, 0, -1};
  for (int i = 0; i < 20 * 300; ++i) {
    const MobEntity* x = rz.zone.find_mob(m);
    if (!x || !x->alive()) break;
    rz.zone.tick(++rz.t, rz.sink);
  }
  EntityId corpse = 0;
  rz.zone.for_each_corpse([&](const CorpseEntity& cc) {
    if (!cc.player_corpse) {
      corpse = cc.id;
      CHECK(cc.party == std::vector<std::int64_t>{rz.cid(rz.b[0])});
      CHECK(cc.loot_char_id == rz.cid(rz.b[0]));
    }
  });
  REQUIRE(corpse != 0);
  rz.zone.loot_open(rz.b[0], corpse, rz.sink);
  rz.zone.loot_take(rz.b[0], corpse, kLootAll, rz.sink);
  CHECK(count_of(*rz.zone.find(rz.b[0]), "rat_whisker") == 1);
}

TEST_CASE("raid threat: a mob that turns on one raider notices the raiders beside it", "[raid][zone]") {
  RaidZone rz({}, 143);
  MobDef foe = mob("road_brigand");
  foe.level = 20;
  foe.aggro_radius = 0;
  const auto m = rz.zone.spawn_mob(foe, {2, 0, 0});
  rz.zone.tick(++rz.t, rz.sink);
  rz.zone.set_target(rz.b[1], m, rz.sink);
  rz.zone.set_auto_attack(rz.b[1], true, rz.sink);
  for (int i = 0; i < 40; ++i) rz.zone.tick(++rz.t, rz.sink);
  const MobEntity* x = rz.zone.find_mob(m);
  REQUIRE(x);
  CHECK(x->threat.contains(rz.a[0]));  // the other group's members too
  CHECK(x->threat.contains(rz.b[0]));
}

// ---------------------------------------------------------------------------- raids over the wire

namespace {

bool raid_heard(const wire::Harness& h, const wire::Client& c, const std::string& needle) {
  for (const auto& m : h.received<proto::ChatMessage>(c.peer))
    if ((m.channel == chat_id(ChatChannel::Raid) || m.channel == chat_id(ChatChannel::System)) && m.text.find(needle) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST_CASE("raids over the wire: formed by commands, seen in RaidState, talked in, and kept across a restart", "[raid][server]") {
  using namespace wire;
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_raid_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->groups_loaded());
    Client a1(h), a2(h), b1(h), b2(h), solo(h);
    a1.login_and_enter("ra1", "Ravel");
    a2.login_and_enter("ra2", "Rhona", "cleric");
    b1.login_and_enter("rb1", "Borin");
    b2.login_and_enter("rb2", "Brisa", "wizard");
    solo.login_and_enter("rso", "Sola");
    h.tick(2);
    a1.cmd("invite Rhona");
    a2.cmd("join");
    b1.cmd("invite Brisa");
    b2.cmd("join");
    h.tick(5);
    REQUIRE(h.server->groups().group_count() == 2);
    // Raids are made of groups: someone ungrouped, or a member who does not lead, cannot be invited.
    a1.cmd("raidinvite Sola");
    CHECK(raid_heard(h, a1, "They are not in a group"));
    a1.cmd("raidinvite Brisa");
    CHECK(raid_heard(h, a1, "Invite their group leader"));
    a2.cmd("raidinvite Borin");
    CHECK(raid_heard(h, a2, "Only your group leader can do that"));
    a1.cmd("raidinvite Borin");
    auto inv = h.last<proto::SocialInvite>(b1.peer);
    REQUIRE(inv);
    CHECK(inv->kind == 2);
    CHECK(inv->from_name == "Ravel");
    b1.cmd("raidaccept");
    h.tick(5);
    REQUIRE(h.server->raids().all().size() == 1);
    CHECK(raid_heard(h, a2, "Borin's group has joined the raid."));
    for (Client* c : {&a1, &a2, &b1, &b2}) {
      auto st = h.last<proto::RaidState>(c->peer);
      REQUIRE(st);
      CHECK(st->raid_id != 0);
      CHECK(st->leader == "Ravel");
      CHECK(st->loot_rule == 0);
      REQUIRE(st->members.size() == 4);
      CHECK(st->members[0].group == 1);
      CHECK(st->members[2].group == 2);
      CHECK(st->members[2].name == "Borin");
      CHECK(st->members[2].group_leader);
      for (const auto& m : st->members) {
        CHECK(m.online);
        CHECK(m.entity_id != 0);  // all in the same zone
        CHECK(m.hp_pct > 0);
      }
    }
    // Raid chat reaches both groups, and nobody outside.
    b2.cmd("rsay pull on three");
    for (Client* c : {&a1, &a2, &b1})
      CHECK(h.last<proto::ChatMessage>(c->peer)->text == "pull on three");
    for (const auto& m : h.received<proto::ChatMessage>(solo.peer)) CHECK(m.text != "pull on three");
    solo.cmd("rsay hello");
    CHECK(raid_heard(h, solo, "You are not in a raid."));
    // The loot rule and the lead.
    b1.cmd("raidloot ng");
    CHECK(raid_heard(h, b1, "Only the raid leader can do that."));
    a1.cmd("raidloot ng");
    h.tick(2);
    CHECK(h.last<proto::RaidState>(b2.peer)->loot_rule == 1);
    a1.cmd("raidleader Brisa");
    h.tick(2);
    CHECK(h.last<proto::RaidState>(a2.peer)->leader == "Brisa");
    b2.cmd("raidleader Ravel");
    h.tick(2);
    // Joining a group in the raid joins the raid.
    a1.cmd("invite Sola");
    solo.cmd("join");
    h.tick(5);
    auto st = h.last<proto::RaidState>(solo.peer);
    REQUIRE(st);
    CHECK(st->members.size() == 5);
    // The raid leader logs out: the lead passes to someone online and comes back to nobody by itself.
    a1.send(proto::Logout{});
    h.tick(10);
    CHECK(h.last<proto::RaidState>(b1.peer)->leader != "Ravel");
    for (Client* c : {&a2, &b1, &b2, &solo}) c->send(proto::Logout{});
    h.tick(10);
    CHECK(h.server->raids().all().size() == 1);  // everyone offline, the raid waits with its groups
  }
  {
    // A restart: the raid is back with its two groups, and someone logging in sees it.
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick(3);
    REQUIRE(h.server->groups_loaded());
    REQUIRE(h.server->raids().all().size() == 1);
    CHECK(h.server->raids().all().begin()->second.groups.size() == 2);
    CHECK(h.server->raids().all().begin()->second.loot == RaidLoot::NeedGreed);
    Client b1(h);
    b1.login("rb1", "password123", false);
    auto list = h.last<proto::CharList>(b1.peer);
    REQUIRE(list);
    b1.send(proto::EnterWorld{list->characters.front().id});
    h.tick(5);
    auto st = h.last<proto::RaidState>(b1.peer);
    REQUIRE(st);
    CHECK(st->raid_id != 0);
    CHECK(st->members.size() == 5);
    CHECK(st->leader == "Borin");  // the only one online leads
    // Disbanding forgets it for good.
    b1.cmd("raiddisband");
    h.tick(5);
    CHECK(h.server->raids().all().empty());
    CHECK(h.last<proto::RaidState>(b1.peer)->raid_id == 0);
  }
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick(3);
    CHECK(h.server->raids().all().empty());
  }
  std::filesystem::remove(path);
}

TEST_CASE("a raid deletion that fails just before shutdown is tried again before the database stops", "[raid][server][db]") {
  using namespace wire;
  Harness h(200, true);
  h.tick(3);
  REQUIRE(h.server->groups_loaded());
  Client a1(h), a2(h), b1(h), b2(h);
  a1.login_and_enter("sa1", "Sevra");
  a2.login_and_enter("sa2", "Sigur", "cleric");
  b1.login_and_enter("sb1", "Torvin");
  b2.login_and_enter("sb2", "Tamsa", "wizard");
  h.tick(2);
  a1.cmd("invite Sigur");
  a2.cmd("join");
  b1.cmd("invite Tamsa");
  b2.cmd("join");
  h.tick(5);
  a1.cmd("raidinvite Torvin");
  b1.cmd("raidaccept");
  h.tick(5);
  REQUIRE(h.server->raids().all().size() == 1);
  REQUIRE(h.flaky->real.load_raids()->size() == 1);
  // The deletion fails; the server learns of it only when the write's completion runs, after this.
  h.flaky->fail_raids = 1;
  a1.cmd("raiddisband");
  REQUIRE(h.server->raids().all().empty());
  REQUIRE(h.flaky->real.load_raids()->size() == 1);
  h.server->shutdown("The server is shutting down.");
  CHECK(h.flaky->real.load_raids()->empty());
}

TEST_CASE("hostile raid traffic never crashes the server", "[raid][server][fuzz]") {
  using namespace wire;
  Harness h;
  Client a(h);
  a.login_and_enter("rfuzz", "Rufus");
  for (const char* cmd : {"raidinvite", "raidinvite Rufus", "raidaccept", "raiddecline", "raidleave", "raidkick", "raidkick x",
                          "raidleader", "raidloot", "raidloot zz", "raiddisband", "raid", "rsay", "rs hi"})
    a.cmd(cmd);
  // A LootGive for nothing in particular.
  a.send(proto::LootGive{12345, 7, "Nobody"});
  a.send(proto::LootGive{0, 255, std::string(15, 'x')});
  h.tick(3);
  CHECK(h.transport.disconnecting.empty());
}

// ---------------------------------------------------------------------------- Phase 15

TEST_CASE("lockouts: one per boss, never extended, lapsing on real time, saved as JSON", "[raid][lockout]") {
  Lockouts l;
  CHECK_FALSE(l.locked("the_emberwyrm", 100));
  CHECK(l.lock("the_emberwyrm", 1000, 100));
  CHECK(l.until("the_emberwyrm", 100) == 1000);
  CHECK_FALSE(l.lock("the_emberwyrm", 5000, 200));  // still running: not extended
  CHECK(l.until("the_emberwyrm", 999) == 1000);
  CHECK(l.until("the_emberwyrm", 1000) == 0);       // ends at its time
  CHECK(l.lock("the_obsidian_tyrant", 700, 100));
  const auto active = l.active(100);
  REQUIRE(active.size() == 2);
  CHECK(active[0].first == "the_obsidian_tyrant");  // soonest first
  CHECK_FALSE(l.lock("", 900, 100));
  CHECK_FALSE(l.lock("x", 50, 100));                // already over
  const Lockouts back = Lockouts::from_json(l.to_json());
  CHECK(back.until("the_emberwyrm", 100) == 1000);
  CHECK(back.until("the_obsidian_tyrant", 100) == 700);
  CHECK(l.lock("the_emberwyrm", 3000, 1000));       // over: a new one may start
  l.prune(2000);
  CHECK(l.active(2000).size() == 1);
  for (const char* bad : {"", "[]", "{\"a\": \"soon\"}", "{\"a\": -5}", "not json"}) CHECK(Lockouts::from_json(bad).empty());
  CHECK(format_span(30) == "1m");
  CHECK(format_span(3600 * 2 + 300) == "2h 5m");
  CHECK(format_span(3600 * 20) == "20h");
  CHECK(format_span(86400 * 3 + 3600 * 4) == "3d 4h");
}

TEST_CASE("the raid leader names a main assist; the leader or main assist marks targets", "[raid]") {
  GroupRegistry groups = make_groups({{1, 2}, {3, 4}, {5, 6}});
  RaidRegistry raids(12);
  REQUIRE(raids.invite(groups, 1, "A", 3, 1e9).ok());
  const std::uint32_t rid = raids.accept(groups, 3, 0.0).raid;
  REQUIRE(raids.invite(groups, 1, "A", 5, 1e9).ok());
  REQUIRE(raids.accept(groups, 5, 0.0).ok());
  CHECK(raids.set_assist(groups, 3, 4).error == RaidError::NotRaidLeader);
  CHECK(raids.set_assist(groups, 1, 99).error == RaidError::NotMember);
  REQUIRE(raids.set_assist(groups, 1, 6).ok());
  CHECK(raids.find(rid)->assist == 6);
  // Marks: the leader or the main assist only, numbered 1..3; a target carries one mark at a time.
  CHECK(raids.set_mark(groups, 2, 1, "z", 77).error == RaidError::NotMarker);
  CHECK(raids.set_mark(groups, 1, 4, "z", 77).error == RaidError::BadMark);
  CHECK(raids.set_mark(groups, 1, 0, "z", 77).error == RaidError::BadMark);
  REQUIRE(raids.set_mark(groups, 1, 1, "z", 77).ok());
  REQUIRE(raids.set_mark(groups, 6, 2, "z", 88).ok());
  REQUIRE(raids.set_mark(groups, 6, 3, "z", 77).ok());  // 77 moves from mark 1 to mark 3
  const Raid* r = raids.find(rid);
  CHECK(r->marks[0].entity == 0);
  CHECK(r->marks[1].entity == 88);
  CHECK(r->marks[2].entity == 77);
  REQUIRE(raids.set_mark(groups, 1, 2, "z", 0).ok());   // cleared
  CHECK(raids.find(rid)->marks[1].entity == 0);
  CHECK(raids.clear_marks(groups, 4).error == RaidError::NotMarker);
  REQUIRE(raids.clear_marks(groups, 6).ok());
  CHECK(raids.find(rid)->marks[2].entity == 0);
  // A zone unloading takes its marks with it; marks elsewhere stay.
  REQUIRE(raids.set_mark(groups, 1, 1, "z", 77).ok());
  REQUIRE(raids.set_mark(groups, 1, 2, "y", 77).ok());
  CHECK(raids.forget_zone_marks("z") == 1);
  CHECK(raids.find(rid)->marks[0].entity == 0);
  CHECK(raids.find(rid)->marks[1].entity == 77);
  CHECK(raids.forget_zone_marks("z") == 0);
  REQUIRE(raids.clear_marks(groups, 1).ok());
  // The main assist's group is removed: they are no longer the main assist.
  REQUIRE(raids.kick(groups, 1, gid(groups, 5)).ok());
  CHECK(raids.find(rid)->assist == 0);
  // One who leaves their group (in the raid through it) is dropped at reconcile.
  REQUIRE(raids.set_assist(groups, 1, 4).ok());
  REQUIRE(groups.leave(4).ok());  // 3 is left alone: that group disbands, and the raid of one dissolves
  for (const RaidChange& c : raids.reconcile(groups)) (void)c;
  CHECK(raids.all().empty());
  // A restored raid keeps a main assist who is still in it, and no marks.
  GroupRegistry g2 = make_groups({{1, 2}, {3, 4}});
  RaidRegistry r2(12);
  Raid saved;
  saved.id = 9;
  saved.leader = 1;
  saved.groups = {gid(g2, 1), gid(g2, 3)};
  saved.assist = 4;
  saved.marks[0] = {"z", 5};
  REQUIRE(r2.restore(saved, g2));
  CHECK(r2.find(9)->assist == 4);
  CHECK(r2.find(9)->marks[0].entity == 0);
  RaidRegistry r3(12);
  saved.assist = 99;
  REQUIRE(r3.restore(saved, g2));
  CHECK(r3.find(9)->assist == 0);
}

TEST_CASE("a raid boss locks out everyone at its kill: next time their share goes to the rest", "[raid][zone][loot][lockout]") {
  RaidZone rz({{"plate_t1_chest", 1}}, 151, true);
  rz.zone.set_wall_clock(1'000'000);
  const EntityId first = rz.kill(true);
  REQUIRE(first != 0);
  const CorpseEntity* c1 = rz.corpse(first);
  REQUIRE(c1);
  CHECK(c1->party.size() == 4);
  CHECK_FALSE(c1->items.empty());
  for (EntityId e : {rz.a[0], rz.a[1], rz.b[0], rz.b[1]}) {
    CHECK(rz.zone.find(e)->lockouts.locked("road_brigand", 1'000'000));
    CHECK(rz.sink.chat_count(e, "treasure for 20h") == 1);
  }
  // A fifth raider joins Bregor's group, then the boss is back: only the newcomer shares its treasure,
  // while the rest still earn the experience.
  const EntityId newcomer = rz.zone.add_player(at(1, 2, "Nell", 20));
  REQUIRE(rz.groups.invite(rz.cid(rz.b[0]), "Bregor", rz.cid(newcomer), 1e9).ok());
  REQUIRE(rz.groups.accept(rz.cid(newcomer), "Nell").ok());
  const std::uint64_t xp0 = rz.zone.find(rz.a[1])->xp;
  rz.zone.set_wall_clock(1'000'000 + 3600);
  const EntityId second = rz.kill(true);
  REQUIRE(second != 0);
  REQUIRE(second != first);
  const CorpseEntity* c2 = rz.corpse(second);
  REQUIRE(c2);
  CHECK(c2->party == std::vector<std::int64_t>{rz.cid(newcomer)});
  CHECK(rz.zone.find(rz.a[1])->xp > xp0);
  CHECK(rz.sink.chat_count(rz.a[1], "still locked out of") == 1);
  CHECK(rz.sink.chat_count(rz.a[1], "(19h left)") == 1);
  CHECK(c2->loot_char_id == rz.cid(newcomer));  // the whole corpse, coin and all, is the newcomer's
  // Everyone locked out: the boss drops nothing at all.
  rz.zone.set_wall_clock(1'000'000 + 7200);
  const EntityId third = rz.kill(true);
  REQUIRE(third != 0);
  const CorpseEntity* c3 = rz.corpse(third);
  REQUIRE(c3);
  CHECK(c3->items.empty());
  CHECK(c3->coin == 0);
  // Twenty hours on, the first four may share it again.
  rz.zone.set_wall_clock(1'000'000 + 20 * 3600);
  const CorpseEntity* c4 = rz.corpse(rz.kill(true));
  REQUIRE(c4);
  CHECK(c4->party.size() == 4);
  CHECK_FALSE(c4->items.empty());
}

TEST_CASE("a raid boss that dies with nobody at its kill leaves no treasure lying about", "[raid][zone][loot][lockout]") {
  RaidZone rz({{"plate_t1_chest", 1}}, 154, true);
  MobDef foe = mob("road_brigand");
  foe.boss = foe.named = true;
  const EntityId m = rz.zone.spawn_mob(foe, {300, 0, 300});  // far from everyone
  rz.zone.tick(++rz.t, rz.sink);
  rz.zone.gm_kill_mob(m, rz.sink);
  const CorpseEntity* c = nullptr;
  rz.zone.for_each_corpse([&](const CorpseEntity& cc) {
    if (!cc.player_corpse) c = &cc;
  });
  REQUIRE(c);
  CHECK(c->items.empty());
  CHECK(c->coin == 0);
}

TEST_CASE("bosses outside raid dungeons, and raid trash, lock nobody out", "[raid][zone][lockout]") {
  {
    RaidZone rz({{"plate_t1_chest", 1}}, 152, false);
    rz.zone.set_wall_clock(1'000'000);
    REQUIRE(rz.kill(true) != 0);
    CHECK(rz.zone.find(rz.a[0])->lockouts.empty());
  }
  {
    RaidZone rz({{"plate_t1_chest", 1}}, 153, true);
    rz.zone.set_wall_clock(1'000'000);
    REQUIRE(rz.kill(false) != 0);
    CHECK(rz.zone.find(rz.a[0])->lockouts.empty());
  }
}

TEST_CASE("only a raid may take a zone line into a raid dungeon", "[raid][zone][travel]") {
  ZoneDef zd = test_zone();
  ZoneLineDef line;
  line.id = "to_lair";
  line.x = 10;
  line.z = 0;
  line.hx = line.hz = 2;
  line.to = "emberwyrm_lair";
  line.entry = "entrance";
  zd.zone_lines.push_back(line);
  Zone zone(zd, content(), kDt, 9);
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  const EntityId lone = zone.add_player(at(10, 0, "Lonely", 55));
  std::uint32_t t = 0;
  for (int i = 0; i < 100; ++i) zone.tick(++t, sink);  // five seconds on the line
  CHECK(zone.take_transfers().empty());
  CHECK(sink.chat_count(lone, "is a raid dungeon: only a raid may enter") == 2);  // said again after 3 s, not every tick
  // Now in a raid: through they go.
  const EntityId mate = zone.add_player(at(0, 0, "Mate", 55));
  const EntityId other = zone.add_player(at(0, 3, "Other", 55));
  const EntityId other2 = zone.add_player(at(0, 4, "Other2", 55));
  const auto cid = [&](EntityId e) { return zone.find(e)->char_id; };
  REQUIRE(groups.invite(cid(lone), "Lonely", cid(mate), 1e9).ok());
  REQUIRE(groups.accept(cid(mate), "Mate").ok());
  REQUIRE(groups.invite(cid(other), "Other", cid(other2), 1e9).ok());
  REQUIRE(groups.accept(cid(other2), "Other2").ok());
  REQUIRE(raids.invite(groups, cid(lone), "Lonely", cid(other), 1e9).ok());
  REQUIRE(raids.accept(groups, cid(other), 0.0).ok());
  zone.tick(++t, sink);
  const auto moves = zone.take_transfers();
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].to.zone == "emberwyrm_lair");
}

namespace {
// Phase 17: a raid dungeon whose way out leads to the Proving Flats.
ZoneDef raid_dungeon_with_exit() {
  ZoneDef zd = test_zone();
  zd.raid = true;
  zd.name = "The Test Lair";
  ZoneLineDef line;
  line.id = "out";
  line.x = 90;
  line.z = 90;
  line.hx = line.hz = 2;
  line.to = "proving_flats";
  line.entry = "emberhold";
  zd.zone_lines.push_back(line);
  return zd;
}
}  // namespace

TEST_CASE("a raid dungeon moves out anyone who is not in a raid", "[raid][zone][travel]") {
  Content c = content();
  c.social.raid_evict_seconds = 10.0f;
  Zone zone(raid_dungeon_with_exit(), c, kDt, 19);
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  std::uint32_t t = 0;
  // Logged in inside, alone: warned at once, moved out after evict_seconds (to the far side of the way out).
  const EntityId lone = zone.add_player(at(0, 0, "Straggler", 55));
  ZonePlayerInit gm_init = at(2, 0, "Overseer", 55);
  gm_init.gm = true;
  const EntityId gm = zone.add_player(gm_init);
  zone.tick(++t, sink);
  CHECK(sink.chat_count(lone, "is a raid dungeon and you are not in a raid: join one within 10 seconds") == 1);
  CHECK(sink.chat_count(gm, "you are not in a raid") == 0);  // a game master may stay
  for (int i = 0; i < 20 * 9; ++i) zone.tick(++t, sink);
  CHECK(zone.take_transfers().empty());
  for (int i = 0; i < 20 * 2; ++i) zone.tick(++t, sink);
  const auto moves = zone.take_transfers();
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].player == lone);
  CHECK(moves[0].to.zone == "proving_flats");
  CHECK(sink.chat_count(lone, "You are not in a raid, and are moved out of The Test Lair.") == 1);
  CHECK(sink.chat_count(lone, "join one within") == 1);  // said once, not every tick
}

TEST_CASE("logging out and back in does not restart the raid-dungeon countdown", "[raid][zone][travel]") {
  Content c = content();
  c.social.raid_evict_seconds = 10.0f;
  Zone zone(raid_dungeon_with_exit(), c, kDt, 23);
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  std::uint32_t t = 0;
  const ZonePlayerInit init = at(0, 0, "Lingerer", 55);
  const EntityId first = zone.add_player(init);
  zone.tick(++t, sink);
  CHECK(sink.chat_count(first, "join one within 10 seconds") == 1);
  for (int i = 0; i < 20 * 8; ++i) zone.tick(++t, sink);
  zone.remove_player(first, sink);
  zone.tick(++t, sink);
  const EntityId again = zone.add_player(init);  // the same character, back 8 s into the countdown
  zone.tick(++t, sink);
  CHECK(sink.chat_count(again, "join one within 2 seconds") == 1);
  for (int i = 0; i < 20 * 3; ++i) zone.tick(++t, sink);
  const auto moves = zone.take_transfers();
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].player == again);
}

TEST_CASE("the raid-dungeon countdown holds while the dungeon unloads and loads again", "[raid][zone][travel]") {
  Content c = content();
  c.social.raid_evict_seconds = 10.0f;
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  RaidEvictTable table;  // the game server's, outliving the zone
  std::uint32_t t = 0;
  const ZonePlayerInit init = at(0, 0, "Returner", 55);
  {
    Zone zone(raid_dungeon_with_exit(), c, kDt, 27);
    zone.set_groups(&groups);
    zone.set_raids(&raids);
    zone.set_raid_evictions(&table);
    const EntityId first = zone.add_player(init);
    zone.tick(++t, sink);
    CHECK(sink.chat_count(first, "join one within 10 seconds") == 1);
    for (int i = 0; i < 20 * 4; ++i) zone.tick(++t, sink);
    zone.remove_player(first, sink);
  }  // the empty dungeon unloads
  t += 20 * 60;  // a minute later it loads again, and the character returns
  Zone zone(raid_dungeon_with_exit(), c, kDt, 29);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  zone.set_raid_evictions(&table);
  const EntityId again = zone.add_player(init);
  zone.tick(++t, sink);
  CHECK(sink.chat_count(again, "join one within 1 second or") == 1);  // already past due: out next tick
  zone.tick(++t, sink);
  const auto moves = zone.take_transfers();
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].player == again);
  CHECK(table.empty());
}

TEST_CASE("coming back more than an hour past the raid-dungeon deadline gives a fresh grace", "[raid][zone][travel]") {
  Content c = content();
  c.social.raid_evict_seconds = 10.0f;
  Zone zone(raid_dungeon_with_exit(), c, kDt, 31);
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  std::uint32_t t = 0;
  const ZonePlayerInit init = at(0, 0, "Wanderer", 55);
  const EntityId first = zone.add_player(init);
  zone.tick(++t, sink);
  zone.remove_player(first, sink);
  t += 20 * 7200;  // two hours away
  const EntityId again = zone.add_player(init);
  zone.tick(++t, sink);
  CHECK(sink.chat_count(again, "join one within 10 seconds") == 1);
  zone.tick(++t, sink);
  CHECK(zone.take_transfers().empty());
}

TEST_CASE("the raid-dungeon countdown holds across the tick counter wrapping", "[raid][zone][travel]") {
  Content c = content();
  c.social.raid_evict_seconds = 10.0f;
  Zone zone(raid_dungeon_with_exit(), c, kDt, 25);
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  std::uint32_t t = std::numeric_limits<std::uint32_t>::max() - 20 * 5;  // wraps 5 s in
  const EntityId lone = zone.add_player(at(0, 0, "Wrapper", 55));
  zone.tick(++t, sink);
  CHECK(sink.chat_count(lone, "join one within 10 seconds") == 1);
  for (int i = 0; i < 20 * 9; ++i) zone.tick(++t, sink);
  CHECK(zone.take_transfers().empty());  // not moved early when the deadline wrapped past zero
  for (int i = 0; i < 20 * 2; ++i) zone.tick(++t, sink);
  CHECK(zone.take_transfers().size() == 1);
  CHECK(sink.chat_count(lone, "join one within") == 1);
}

TEST_CASE("joining a raid in time lets a player stay in a raid dungeon", "[raid][zone][travel]") {
  Content c = content();
  c.social.raid_evict_seconds = 10.0f;
  Zone zone(raid_dungeon_with_exit(), c, kDt, 21);
  RecordingSink sink;
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  zone.set_groups(&groups);
  zone.set_raids(&raids);
  std::uint32_t t = 0;
  const EntityId a = zone.add_player(at(0, 0, "Alda", 55));
  const EntityId a2 = zone.add_player(at(1, 0, "Alwen", 55));
  const EntityId b = zone.add_player(at(0, 2, "Borin", 55));
  const EntityId b2 = zone.add_player(at(1, 2, "Brisa", 55));
  const auto cid = [&](EntityId e) { return zone.find(e)->char_id; };
  zone.tick(++t, sink);
  CHECK(sink.chat_count(a, "join one within") == 1);
  REQUIRE(groups.invite(cid(a), "Alda", cid(a2), 1e9).ok());
  REQUIRE(groups.accept(cid(a2), "Alwen").ok());
  REQUIRE(groups.invite(cid(b), "Borin", cid(b2), 1e9).ok());
  REQUIRE(groups.accept(cid(b2), "Brisa").ok());
  REQUIRE(raids.invite(groups, cid(a), "Alda", cid(b), 1e9).ok());
  REQUIRE(raids.accept(groups, cid(b), 0.0).ok());
  for (int i = 0; i < 20 * 12; ++i) zone.tick(++t, sink);
  CHECK(zone.take_transfers().empty());
  CHECK(sink.chat_count(a, "You are in a raid now: you may stay in The Test Lair.") == 1);
  // The raid disbands inside: the countdown starts again for everyone in it.
  const RaidChange gone = raids.disband(groups, cid(a));
  REQUIRE(gone.ok());
  for (int i = 0; i < 20 * 11; ++i) zone.tick(++t, sink);
  CHECK(zone.take_transfers().size() == 4);
}

TEST_CASE("a mark on a monster clears when it dies", "[raid][marks]") {
  GroupRegistry groups(6);
  RaidRegistry raids(12);
  REQUIRE(groups.invite(1, "Lead", 2, 1e9).ok());
  REQUIRE(groups.accept(2, "Two").ok());
  REQUIRE(groups.invite(3, "Other", 4, 1e9).ok());
  REQUIRE(groups.accept(4, "Four").ok());
  REQUIRE(raids.invite(groups, 1, "Lead", 3, 1e9).ok());
  REQUIRE(raids.accept(groups, 3, 0.0).ok());
  REQUIRE(raids.set_mark(groups, 1, 1, "hollowmere_crypts", 77).ok());
  REQUIRE(raids.set_mark(groups, 1, 2, "hollowmere_crypts", 78).ok());
  CHECK(raids.forget_marks_on("thornwood", 77).empty());  // another zone's entity 77
  const auto changed = raids.forget_marks_on("hollowmere_crypts", 77);
  REQUIRE(changed.size() == 1);
  const Raid* r = raids.raid_of(groups, 1);
  REQUIRE(r);
  CHECK(r->marks[0].entity == 0);
  CHECK(r->marks[1].entity == 78);
  CHECK(raids.forget_marks_on("hollowmere_crypts", 0).empty());
}

TEST_CASE("main assist, marks and lockouts over the wire", "[raid][server][lockout]") {
  using namespace wire;
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_raid15_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->groups_loaded());
    Client a1(h), a2(h), b1(h), b2(h);
    a1.login_and_enter("ma1", "Marro");
    a2.login_and_enter("ma2", "Mela", "cleric");
    b1.login_and_enter("mb1", "Nodric");
    const auto eb2 = b2.login_and_enter("mb2", "Nyssa", "wizard");
    h.tick(2);
    a1.cmd("invite Mela");
    a2.cmd("join");
    b1.cmd("invite Nyssa");
    b2.cmd("join");
    h.tick(5);
    a1.cmd("raidinvite Nodric");
    b1.cmd("raidaccept");
    h.tick(5);
    REQUIRE(h.server->raids().all().size() == 1);
    // The main assist: only the leader names one, from the raid.
    b1.cmd("raidassist Nyssa");
    CHECK(raid_heard(h, b1, "Only the raid leader can do that."));
    a1.cmd("raidassist Nobody");
    CHECK(raid_heard(h, a1, "They are not in your raid."));
    a1.cmd("raidassist Nyssa");
    h.tick(2);
    CHECK(raid_heard(h, a2, "Nyssa is the raid's main assist"));
    for (Client* c : {&a1, &a2, &b1, &b2}) CHECK(h.last<proto::RaidState>(c->peer)->main_assist == "Nyssa");
    // Nyssa targets a monster and marks it; a member who is neither leader nor assist cannot.
    Zone* z = h.server->zone("proving_flats");
    REQUIRE(z);
    Vec3 here;
    z->for_each_player([&](const PlayerEntity& p) {
      if (p.name == "Nyssa") here = p.pos;
    });
    static MobDef calm = mob("road_brigand");
    calm.aggro_radius = 0;
    const EntityId foe = z->spawn_mob(calm, {here.x + 2.0f, here.y, here.z});
    REQUIRE(foe != 0);
    h.tick(2);  // in everyone's view first
    b2.send(proto::SetTarget{foe});
    h.tick(2);
    b2.cmd("mark 2");
    h.tick(2);
    auto st = h.last<proto::RaidState>(a2.peer);
    REQUIRE(st);
    REQUIRE(st->marks.size() == 1);
    CHECK(st->marks[0].slot == 2);
    CHECK(st->marks[0].entity_id == foe);
    CHECK(raid_heard(h, a1, "Nyssa marks "));
    a2.send(proto::SetTarget{foe});
    a2.cmd("mark 1");
    CHECK(raid_heard(h, a2, "Only the raid leader or main assist can mark targets."));
    a1.cmd("mark 7");
    CHECK(raid_heard(h, a1, "Usage: /mark"));
    // /assist with no name follows the main assist's target.
    a2.send(proto::SetTarget{0});
    h.tick(2);
    a2.cmd("assist");
    h.tick(2);
    const PlayerEntity* mela = nullptr;
    z->for_each_player([&](const PlayerEntity& p) {
      if (p.name == "Mela") mela = &p;
    });
    REQUIRE(mela);
    CHECK(mela->target == foe);
    a1.cmd("mark clear");
    h.tick(2);
    CHECK(h.last<proto::RaidState>(b1.peer)->marks.empty());
    // Lockouts: none yet.
    a1.cmd("lockouts");
    CHECK(raid_heard(h, a1, "You are not locked out of any raid boss's treasure."));
    (void)eb2;
    for (Client* c : {&a1, &a2, &b1, &b2}) c->send(proto::Logout{});
    h.tick(10);
  }
  {
    // A restart keeps the main assist.
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick(3);
    REQUIRE(h.server->raids().all().size() == 1);
    const std::int64_t assist = h.server->raids().all().begin()->second.assist;
    CHECK(assist != 0);
  }
  std::filesystem::remove(path);
}
