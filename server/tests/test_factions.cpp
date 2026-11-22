// Phase 6d: faction standing, regard tiers, guards that attack only foes, kill consequences,
// service NPCs refusing the disliked, illusions fooling a faction, and standings that persist.
#include <catch2/catch_test_macros.hpp>

#include "db/sqlite_repository.hpp"
#include "game/factions.hpp"
#include "zone_support.hpp"

using namespace zt;

TEST_CASE("regard tiers, attack-on-sight and refusal thresholds", "[factions]") {
  CHECK(regard_of(2000) == Regard::Ally);
  CHECK(regard_of(1100) == Regard::Ally);
  CHECK(regard_of(1099) == Regard::Warmly);
  CHECK(regard_of(0) == Regard::Indifferent);
  CHECK(regard_of(-99) == Regard::Indifferent);
  CHECK(regard_of(-100) == Regard::Apprehensive);
  CHECK(regard_of(-500) == Regard::Dubious);
  CHECK(regard_of(-750) == Regard::Threatening);
  CHECK(regard_of(-1000) == Regard::Scowls);
  CHECK_FALSE(attacks_on_sight(Regard::Dubious));
  CHECK(attacks_on_sight(Regard::Threatening));
  CHECK(refuses_service(Regard::Dubious));
  CHECK_FALSE(refuses_service(Regard::Apprehensive));
  CHECK(std::string(regard_text(Regard::Scowls)).find("ready to attack") != std::string::npos);
  FactionStandings fs;
  CHECK(fs.earned("x") == 0);
  CHECK(fs.add("x", 30) == 30);
  CHECK(fs.add("x", -50) == -20);
  CHECK(fs.add("x", -100000) == 2 * kFactionMin);
  CHECK(clamp_standing(5000) == kFactionMax);
}

TEST_CASE("shipped factions: every mob has one, citizens start liked by their city", "[factions][content]") {
  const Content& c = content();
  for (const auto& m : c.mobs) CHECK(c.find_faction(m.faction));
  for (const auto& city : c.cities) {
    INFO(city.id);
    const ZoneDef* z = c.find_zone(city.zone);
    REQUIRE(z);
    // The hub's services belong to its watch, and its races start out amiable with it.
    std::string watch;
    for (const auto& n : c.npcs)
      if (n.zone == z->id && n.role == "banker") watch = n.faction;
    REQUIRE_FALSE(watch.empty());
    const FactionDef* f = c.find_faction(watch);
    REQUIRE(f);
    for (const auto& r : c.races)
      if (std::find(r.cities.begin(), r.cities.end(), city.id) != r.cities.end()) CHECK(regard_of(f->standing_for(r.id, 0)) == Regard::Amiably);
    // And it has guards.
    bool guards = false;
    for (const auto& sp : z->spawns)
      if (const MobDef* m = c.find_mob(sp.mob); m && m->faction == watch) guards = true;
    CHECK(guards);
  }
}

TEST_CASE("guards leave friends alone and attack those their city hates", "[factions][zone]") {
  Zone zone(test_zone(), content(), kDt, 31);
  RecordingSink sink;
  std::uint32_t t = 0;
  const EntityId guard = zone.spawn_mob(mob("emberhold_watchman"), {0, 0, 0});
  const EntityId friendly = zone.add_player(as_class(at(10, 0, "Citizen", 20), "warrior"));
  auto villain_init = as_class(at(-10, 0, "Villain", 20), "warrior");
  villain_init.factions = {{"emberhold_watch", -1500}};
  const EntityId villain = zone.add_player(villain_init);
  for (int i = 0; i < 40; ++i) zone.tick(++t, sink);
  CHECK_FALSE(zone.find_mob(guard)->threat.contains(friendly));
  CHECK(zone.find_mob(guard)->threat.contains(villain));
  // Considering shows the regard.
  zone.set_target(villain, guard, sink);
  auto info = sink.last<proto::TargetInfo>(villain);
  REQUIRE(info);
  CHECK(info->regard == static_cast<std::uint8_t>(Regard::Scowls));
  CHECK(info->faction == "The Emberhold Watch");
  zone.set_target(friendly, guard, sink);
  CHECK(sink.last<proto::TargetInfo>(friendly)->regard == static_cast<std::uint8_t>(Regard::Amiably));  // a Highlander: +300
}

TEST_CASE("killing a faction's members moves standing with it and its enemies", "[factions][zone]") {
  Zone zone(test_zone(), content(), kDt, 32);
  RecordingSink sink;
  std::uint32_t t = 0;
  const EntityId p = zone.add_player(as_class(at(0, 0, "Hunter", 30), "warrior"));
  zone.tick(++t, sink);
  const int before = zone.standing(*zone.find(p), "emberhold_watch");
  const EntityId b = zone.spawn_mob(mob("bandit_raider"), {2, 0, 0});
  zone.find_mob(b)->damage_by.emplace_back(zone.find(p)->char_id, 1);
  zone.find_mob(b)->hp = 1;
  AbilityDef root;  // so it cannot run off at low health
  root.id = root.name = root.stack_group = "test_root";
  root.target = TargetType::Enemy;
  EffectDef held;
  held.type = EffectType::Root;
  held.duration = 600;
  root.effects.push_back(held);
  zone.force_effect(b, root, 60, sink);
  zone.tick(++t, sink);  // the player sees it
  zone.set_target(p, b, sink);
  zone.set_auto_attack(p, true, sink);
  for (int i = 0; i < 2000 && zone.find_mob(b) && zone.find_mob(b)->alive(); ++i) {
    zone.find_mob(b)->hp = 1;  // one landed blow ends it
    zone.tick(++t, sink);
  }
  REQUIRE((!zone.find_mob(b) || !zone.find_mob(b)->alive()));
  CHECK(zone.standing(*zone.find(p), "emberhold_watch") == before + 3);
  CHECK(sink.chat_count(p, "Your standing with The Emberhold Watch got better.") == 1);
  zone.tick(++t, sink);
  auto list = sink.last<proto::FactionStandings>(p);
  REQUIRE(list);
  bool listed = false;
  for (const auto& f : list->factions) listed = listed || (f.id == "emberhold_watch" && f.standing == before + 3);
  CHECK(listed);
}

TEST_CASE("service NPCs refuse the disliked; an illusion of a liked race fools them", "[factions][zone]") {
  const Content& c = content();
  const ZoneDef* hub = c.find_zone("thornwood");
  REQUIRE(hub);
  Zone zone(*hub, c, kDt, 33);
  RecordingSink sink;
  std::uint32_t t = 0;
  EntityId vendor = 0;
  zone.for_each_npc([&](const NpcEntity& n) {
    if (n.def->role == "vendor" && vendor == 0) vendor = n.id;
  });
  REQUIRE(vendor);
  const Vec3 at_v = zone.find_npc(vendor)->pos;
  auto init = as_class(at(at_v.x + 2, at_v.z, "Outcast", 20), "warrior");
  init.race = "morrok";  // -250 with the wardens: apprehensive, still served
  init.factions = {{"thornwood_wardens", -300}};  // now dubious
  const EntityId p = zone.add_player(init);
  zone.tick(++t, sink);
  zone.vendor_open(p, vendor, sink);
  CHECK_FALSE(sink.last<proto::VendorWindow>(p));
  CHECK(sink.chat_count(p, "I will not deal with the likes of you.") == 1);
  // Disguised as a Vaelith (+300 with the wardens), the vendor serves.
  AbilityDef disguise;
  disguise.id = disguise.name = disguise.stack_group = "test_disguise";
  disguise.target = TargetType::Self;
  EffectDef ill;
  ill.type = EffectType::Illusion;
  ill.duration = 600;
  ill.ref = "vaelith";
  disguise.effects.push_back(ill);
  zone.force_effect(p, disguise, 30, sink);
  zone.vendor_open(p, vendor, sink);
  CHECK(sink.last<proto::VendorWindow>(p));
}

TEST_CASE("faction standing and quest progress survive a save (schema v7)", "[factions][db]") {
  ef::db::SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("loyal", "h", acct) == ef::db::DbStatus::Ok);
  ef::db::CharacterRecord c;
  c.account_id = acct;
  c.name = "Loyal";
  c.race = "highlander";
  c.class_id = "warrior";
  c.city = "emberhold";
  c.zone = "emberhold";
  REQUIRE(repo.create_character(c, 4) == ef::db::DbStatus::Ok);
  c.factions = {{"emberhold_watch", 250}, {"brigands", -40}};
  c.quests = R"({"active":{"q":1}})";
  REQUIRE(repo.save_characters({c}) == ef::db::DbStatus::Ok);
  auto back = repo.load_character(acct, c.id);
  REQUIRE(back);
  REQUIRE(back->factions.size() == 2);
  CHECK(back->factions[0].first == "brigands");
  CHECK(back->factions[0].second == -40);
  CHECK(back->quests == R"({"active":{"q":1}})");
}
