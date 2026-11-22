// Phase 6e: the quest book (pure), the shipped quests and epic chains, and quests played through a
// zone: dialogue, accepting, kill credit, quest items that drop only while needed, deliveries,
// rewards, abandoning, and progress that survives a save.
#include <catch2/catch_test_macros.hpp>

#include <set>

#include "zone_support.hpp"

using namespace zt;

namespace {

QuestDef kill_quest(const char* id, const char* mob, int count) {
  QuestDef q;
  q.id = id;
  q.name = id;
  q.giver = "giver";
  q.level = 5;
  q.min_level = 3;
  QuestStepDef s;
  s.type = QuestStepType::Kill;
  s.mobs = {mob};
  s.count = count;
  s.text = "kill";
  q.steps.push_back(s);
  return q;
}

}  // namespace

TEST_CASE("quest book: refusals, kills, collecting, talking, turning in, saving", "[quests]") {
  QuestBook b;
  QuestDef q = kill_quest("rats", "ember_rat", 2);
  CHECK(b.can_accept(q, 2, "warrior") == QuestRefusal::Level);
  QuestDef young = q;
  young.id = "young";
  young.prereqs = {"rats"};
  CHECK(b.can_accept(young, 2, "warrior") == QuestRefusal::Prereq);  // prerequisites before level
  QuestDef epic = q;
  epic.id = "epic";
  epic.classes = {"wizard"};
  CHECK(b.can_accept(epic, 10, "warrior") == QuestRefusal::Class);
  QuestDef chained = q;
  chained.id = "chained";
  chained.prereqs = {"rats"};
  CHECK(b.can_accept(chained, 10, "warrior") == QuestRefusal::Prereq);
  REQUIRE(b.can_accept(q, 5, "warrior") == QuestRefusal::None);
  REQUIRE(b.accept(q));
  CHECK(b.can_accept(q, 5, "warrior") == QuestRefusal::Active);
  const std::vector<const QuestDef*> defs{&q};
  CHECK(b.on_kill(defs, "dusk_beetle").empty());
  CHECK(b.on_kill(defs, "ember_rat").size() == 1);
  CHECK_FALSE(b.ready(q));
  b.on_kill(defs, "ember_rat");
  CHECK(b.ready(q));
  b.on_kill(defs, "ember_rat");  // a finished step takes no more
  CHECK(b.find("rats")->step == 1);
  REQUIRE(b.complete(q));
  CHECK(b.is_done("rats"));
  CHECK(b.can_accept(q, 5, "warrior") == QuestRefusal::Done);
  CHECK(b.can_accept(chained, 10, "warrior") == QuestRefusal::None);

  // Collect, then a delivery that needs the item.
  QuestDef fetch;
  fetch.id = "fetch";
  QuestStepDef c;
  c.type = QuestStepType::Collect;
  c.item = "rat_whisker";
  c.count = 3;
  c.text = "collect";
  QuestStepDef t;
  t.type = QuestStepType::Talk;
  t.npc = "bob";
  t.item = "sealed_letter";
  t.text = "deliver";
  fetch.steps = {c, t};
  REQUIRE(b.accept(fetch));
  CHECK_FALSE(b.try_collect(fetch, 2));
  CHECK(b.try_collect(fetch, 3));
  CHECK_FALSE(b.try_talk(fetch, "alice", true));
  CHECK_FALSE(b.try_talk(fetch, "bob", false));
  CHECK(b.try_talk(fetch, "bob", true));
  CHECK(b.ready(fetch));

  // Round trip, and garbage in.
  const QuestBook back = QuestBook::from_json(b.to_json());
  REQUIRE(back.find("fetch"));
  CHECK(back.find("fetch")->step == 2);
  CHECK(back.is_done("rats"));
  CHECK(QuestBook::from_json("not json").active().empty());
  CHECK(QuestBook::from_json(R"({"active":[{"id":7},{"id":"x","step":-5}],"done":[1,"y"]})").find("x")->step == 0);
  CHECK(QuestBook::from_json(R"({"active":[{"id":7},{"id":"x","step":-5}],"done":[1,"y"]})").is_done("y"));
  // A full book refuses more.
  QuestBook full;
  for (std::size_t i = 0; i < QuestBook::kMaxActive; ++i) {
    QuestDef f = kill_quest(("f" + std::to_string(i)).c_str(), "ember_rat", 1);
    REQUIRE(full.accept(f));
  }
  CHECK(full.can_accept(kill_quest("one_more", "ember_rat", 1), 10, "warrior") == QuestRefusal::Full);
  CHECK(quest_xp(0.5, 1000) == 500);
  CHECK(quest_xp(-1.0, 1000) == 0);
}

TEST_CASE("shipped quests: 30+, givers in the world, an epic chain per class ending in its weapon", "[quests][content]") {
  const Content& c = content();
  int regular = 0;
  std::set<std::string> boss_quests;
  for (const auto& q : c.quests) {
    INFO(q.id);
    const NpcDef* g = c.find_npc(q.giver);
    REQUIRE(g);
    CHECK(c.find_zone(g->zone));
    if (!q.epic) ++regular;
    for (const auto& s : q.steps)
      if (s.type == QuestStepType::Kill)
        for (const auto& m : s.mobs) {
          // Every mob a quest asks for spawns somewhere.
          bool spawns = false;
          for (const auto& z : c.zones)
            for (const auto& sp : z.spawns) spawns = spawns || sp.mob == m || sp.rare == m;
          CHECK(spawns);
          if (const MobDef* md = c.find_mob(m); md && md->boss) boss_quests.insert(m);
        }
  }
  CHECK(regular >= 30);
  CHECK(boss_quests.size() >= 8);  // most dungeon bosses have a quest
  for (const auto& cl : c.classes) {
    INFO(cl.id);
    const QuestDef* last = nullptr;
    for (const auto& q : c.quests)
      if (q.epic && q.classes.size() == 1 && q.classes[0] == cl.id && !q.items.empty()) last = &q;
    REQUIRE(last);
    CHECK(last->prereqs.size() == 1);
    const ItemDef* weapon = c.find_item(last->items.front().item);
    REQUIRE(weapon);
    CHECK(weapon->rarity == "epic");
    CHECK(weapon->req_level >= 46);
    CHECK(weapon->req_level <= 50);
    CHECK(weapon->classes == std::vector<std::string>{cl.id});
    // The class can wear it.
    CHECK(std::find(cl.weapons.begin(), cl.weapons.end(), weapon->weapon) != cl.weapons.end());
    // And the chain can be walked back to a quest with no prerequisites.
    const QuestDef* q = last;
    int depth = 0;
    while (q && !q->prereqs.empty() && depth < 10) {
      q = c.find_quest(q->prereqs.front());
      ++depth;
    }
    CHECK(depth >= 2);
  }
}

namespace {

// A zone with every quest giver of the shipped content standing beside the player.
struct QuestTown {
  Content c = content();
  std::unique_ptr<Zone> zone;
  RecordingSink sink;
  std::uint32_t t = 0;
  EntityId p = 0;

  explicit QuestTown(int level = 10) {
    for (auto& n : c.npcs)
      if (n.role == "questgiver") {
        n.zone = "test";
        n.x = 3.0f;
        n.z = 0.0f;
      }
    zone = std::make_unique<Zone>(test_zone(), c, kDt, 41);
    p = zone->add_player(as_class(at(0, 0, "Questor", level), "warrior"));
    tick();
  }
  void tick(int n = 1) {
    for (int i = 0; i < n; ++i) zone->tick(++t, sink);
  }
  EntityId npc(const char* id) {
    EntityId out = 0;
    zone->for_each_npc([&](const NpcEntity& e) {
      if (e.def->id == id) out = e.id;
    });
    return out;
  }
  bool option(const char* key) {
    auto d = sink.last<proto::NpcDialog>(p);
    if (!d) return false;
    for (const auto& o : d->options)
      if (o.key == key) return true;
    return false;
  }
  // Kills one mob of this id next to the player, credited to them.
  void kill(const char* mob_id) {
    const EntityId m = zone->spawn_mob(zt::mob(mob_id), {1.5f, 0, 0});
    tick();
    zone->find_mob(m)->damage_by.emplace_back(zone->find(p)->char_id, 1);
    AbilityDef root;
    root.id = root.name = root.stack_group = "test_root";
    root.target = TargetType::Enemy;
    EffectDef held;
    held.type = EffectType::Root;
    held.duration = 600;
    root.effects.push_back(held);
    zone->force_effect(m, root, 60, sink);
    zone->set_target(p, m, sink);
    zone->set_auto_attack(p, true, sink);
    for (int i = 0; i < 3000 && zone->find_mob(m) && zone->find_mob(m)->alive(); ++i) {
      zone->find_mob(m)->hp = 1;
      zone->find(p)->hp = zone->find(p)->stats.max_hp;
      tick();
    }
    REQUIRE((!zone->find_mob(m) || !zone->find_mob(m)->alive()));
  }
};

}  // namespace

TEST_CASE("a kill quest from offer to reward", "[quests][zone]") {
  QuestTown town(5);
  const EntityId brack = town.npc("captain_brack");
  REQUIRE(brack);
  town.zone->npc_talk(town.p, brack, town.sink);
  REQUIRE(town.option("offer:emberglade_vermin"));
  CHECK_FALSE(town.option("offer:the_rattle_king"));  // needs warren_bones first
  town.zone->npc_choose(town.p, brack, "offer:emberglade_vermin", town.sink);
  REQUIRE(town.option("accept:emberglade_vermin"));
  town.zone->npc_choose(town.p, brack, "accept:emberglade_vermin", town.sink);
  CHECK(town.sink.chat_count(town.p, "You have accepted the quest: Vermin in the Glade.") == 1);
  const QuestDef* q = town.c.find_quest("emberglade_vermin");
  REQUIRE(q);
  town.tick();
  auto j = town.sink.last<proto::QuestJournal>(town.p);
  REQUIRE(j);
  REQUIRE(j->quests.size() == 1);
  CHECK(j->quests[0].need == static_cast<std::uint16_t>(q->steps[0].count));
  // Turning in early: not offered.
  town.zone->npc_talk(town.p, brack, town.sink);
  CHECK(town.option("status:emberglade_vermin"));
  CHECK_FALSE(town.option("complete:emberglade_vermin"));
  const auto xp_before = town.zone->find(town.p)->xp;
  const auto coin_before = town.zone->find(town.p)->coin;
  const int watch_before = town.zone->standing(*town.zone->find(town.p), "emberhold_watch");
  for (int i = 0; i < q->steps[0].count; ++i) town.kill(q->steps[0].mobs.front().c_str());
  town.tick();
  CHECK(town.sink.last<proto::QuestJournal>(town.p)->quests[0].ready);
  town.zone->npc_talk(town.p, brack, town.sink);
  REQUIRE(town.option("complete:emberglade_vermin"));
  town.zone->npc_choose(town.p, brack, "complete:emberglade_vermin", town.sink);
  CHECK(town.sink.chat_count(town.p, "You have completed the quest: Vermin in the Glade!") == 1);
  CHECK(town.zone->find(town.p)->xp > xp_before);
  CHECK(town.zone->find(town.p)->coin >= coin_before + static_cast<std::uint64_t>(q->coin));
  CHECK(town.zone->standing(*town.zone->find(town.p), "emberhold_watch") > watch_before);
  CHECK(town.zone->find(town.p)->quests.is_done("emberglade_vermin"));
  town.zone->npc_talk(town.p, brack, town.sink);
  CHECK_FALSE(town.option("offer:emberglade_vermin"));  // done once
}

TEST_CASE("a delivery chain: the giver hands over a letter, and the next NPC takes it", "[quests][zone]") {
  QuestTown town(5);
  const EntityId wenlow = town.npc("scholar_wenlow");
  const EntityId mirrin = town.npc("elder_mirrin");
  REQUIRE(wenlow);
  REQUIRE(mirrin);
  town.zone->npc_choose(town.p, wenlow, "accept:letter_to_thornwood", town.sink);
  CHECK(town.zone->carried(*town.zone->find(town.p), "sealed_letter") == 1);
  town.zone->npc_talk(town.p, mirrin, town.sink);  // the delivery: the letter is taken
  CHECK(town.zone->carried(*town.zone->find(town.p), "sealed_letter") == 0);
  REQUIRE(town.option("complete:letter_to_thornwood"));
  town.zone->npc_choose(town.p, mirrin, "complete:letter_to_thornwood", town.sink);
  CHECK(town.zone->find(town.p)->quests.is_done("letter_to_thornwood"));
  // The next link of the chain opens at Mirrin.
  town.zone->npc_talk(town.p, mirrin, town.sink);
  CHECK(town.option("offer:reply_from_thornwood"));
}

TEST_CASE("quest items drop only while needed, and abandoning forgets the quest", "[quests][zone]") {
  QuestTown town(14);
  const QuestDef* q = town.c.find_quest("fen_idols");
  REQUIRE(q);
  const QuestStepDef& s = q->steps[0];
  REQUIRE_FALSE(s.drops_from.empty());
  // Before accepting: no idols, however many die.
  for (int i = 0; i < 6; ++i) town.kill(s.drops_from.front().c_str());
  CHECK(town.zone->carried(*town.zone->find(town.p), s.item) == 0);
  const EntityId orla = town.npc("fenwatcher_orla");
  town.zone->npc_choose(town.p, orla, "accept:fen_idols", town.sink);
  for (int i = 0; i < 60 && town.zone->carried(*town.zone->find(town.p), s.item) < s.count; ++i) town.kill(s.drops_from.front().c_str());
  CHECK(town.zone->carried(*town.zone->find(town.p), s.item) == s.count);
  // No more once enough are carried.
  for (int i = 0; i < 6; ++i) town.kill(s.drops_from.front().c_str());
  CHECK(town.zone->carried(*town.zone->find(town.p), s.item) == s.count);
  town.zone->npc_talk(town.p, orla, town.sink);
  REQUIRE(town.option("complete:fen_idols"));
  CHECK(town.zone->carried(*town.zone->find(town.p), s.item) == 0);  // handed over
  // Abandon: a fresh quest, then gone from the journal.
  town.zone->npc_choose(town.p, orla, "accept:fen_mirefolk", town.sink);
  REQUIRE(town.zone->find(town.p)->quests.find("fen_mirefolk"));
  town.zone->quest_abandon(town.p, "fen_mirefolk", town.sink);
  CHECK_FALSE(town.zone->find(town.p)->quests.find("fen_mirefolk"));
}

TEST_CASE("an epic chain is refused to other classes and to the too-young", "[quests][zone]") {
  QuestTown town(30);
  const EntityId hagen = town.npc("epic_warlord_hagen");
  REQUIRE(hagen);
  town.zone->npc_talk(town.p, hagen, town.sink);
  CHECK_FALSE(town.option("offer:epic_warrior_1"));  // level 30: not yet, not even a hint
  QuestTown vet(44);
  vet.zone->npc_talk(vet.p, vet.npc("epic_warlord_hagen"), vet.sink);
  CHECK(vet.option("offer:epic_warrior_1"));
  vet.zone->npc_talk(vet.p, vet.npc("epic_archmagus_quill"), vet.sink);
  CHECK_FALSE(vet.option("offer:epic_wizard_1"));  // a warrior is no wizard
}
