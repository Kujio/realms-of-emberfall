// Phase 6a: world geometry (terrain, collision, line of sight), the seeded dungeon generator, the
// navigation grid, world content (zone files, lines, entries, reachability) and zone transfers.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <deque>
#include <fstream>
#include <set>

#include "game/dungeon_gen.hpp"
#include "game/nav_grid.hpp"
#include "game/world_geom.hpp"
#include "zone_support.hpp"

using namespace zt;
using nlohmann::json;

namespace {

// Pinned: the client's dungeon_gen.gd must produce the same layouts (client/tests/test_runner.gd).
constexpr std::uint32_t kGoldenSeed = 12345;
constexpr int kGoldenRooms = 10;

bool reachable_floor(const DungeonLayout& d, int from_x, int from_z, int to_x, int to_z) {
  const TileGrid& t = d.tiles;
  std::vector<std::uint8_t> seen(t.solid.size(), 0);
  std::deque<std::pair<int, int>> q{{from_x, from_z}};
  seen[static_cast<std::size_t>(from_z * t.w + from_x)] = 1;
  while (!q.empty()) {
    auto [x, z] = q.front();
    q.pop_front();
    if (x == to_x && z == to_z) return true;
    const int dx[4] = {1, -1, 0, 0};
    const int dz[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
      const int nx = x + dx[k];
      const int nz = z + dz[k];
      if (t.solid_cell(nx, nz)) continue;
      auto& s = seen[static_cast<std::size_t>(nz * t.w + nx)];
      if (s) continue;
      s = 1;
      q.emplace_back(nx, nz);
    }
  }
  return false;
}

}  // namespace

TEST_CASE("terrain: base plus round hills, nothing past a hill's edge", "[world][geom]") {
  Terrain t;
  t.base = 2.0f;
  CHECK(t.height(10, 10) == 2.0f);
  t.hills.push_back({0, 0, 10, 5});
  CHECK(t.height(0, 0) == 7.0f);
  CHECK(t.height(10, 0) == 2.0f);
  CHECK(t.height(20, 0) == 2.0f);
  CHECK(t.height(5, 0) > 2.0f);
  CHECK(t.height(5, 0) < 7.0f);
  t.hills.push_back({0, 0, 10, -5});  // a hollow cancels the hill
  CHECK(t.height(3, 4) == 2.0f);
}

TEST_CASE("collision: bodies are pushed out of circles, rotated boxes and wall tiles", "[world][geom]") {
  SECTION("circle") {
    const CollisionMap map({{0, 0, 2}}, {}, {});
    const Vec3 out = map.resolve({1, 0, 0}, 0.5f);
    CHECK(std::fabs(out.x - 2.5f) < 1e-4f);
    CHECK(map.overlaps(2.4f, 0, 0.5f));
    CHECK_FALSE(map.overlaps(2.6f, 0, 0.5f));
  }
  SECTION("rotated box") {
    // A 10 x 2 wall turned 90 degrees runs along z.
    const CollisionMap map({}, {{0, 0, 5, 1, kPi / 2}}, {});
    CHECK(map.overlaps(0, 4, 0.4f));
    CHECK_FALSE(map.overlaps(4, 0, 0.4f));
    const Vec3 out = map.resolve({0.5f, 0, 3}, 0.4f);
    CHECK(std::fabs(out.x - 1.4f) < 1e-3f);  // out through the nearest face
    CHECK(std::fabs(out.z - 3.0f) < 1e-3f);
  }
  SECTION("tiles") {
    TileGrid t;
    t.w = 3;
    t.h = 3;
    t.cell = 2;
    t.solid = {1, 1, 1, 1, 0, 1, 1, 1, 1};
    const CollisionMap map({}, {}, t);
    const Vec3 out = map.resolve({2.1f, 0, 3}, 0.4f);  // centre cell spans x 2..4
    CHECK(out.x >= 2.4f - 1e-4f);
    CHECK_FALSE(map.overlaps(3, 3, 0.4f));
    CHECK(map.overlaps(1, 3, 0.4f));
    CHECK(map.overlaps(-5, -5, 0.1f));  // outside the grid counts as wall
  }
  SECTION("line of sight") {
    const CollisionMap map({{0, 0, 1}}, {{10, 0, 1, 5, 0}}, {});
    CHECK_FALSE(map.segment_clear({-5, 0, 0}, {5, 0, 0}));  // through the pillar
    CHECK(map.segment_clear({-5, 0, 3}, {5, 0, 3}));
    CHECK(map.segment_clear({0, 0, 0.5f}, {5, 0, 0.5f}));   // standing inside the pillar
    CHECK_FALSE(map.segment_clear({5, 0, 0}, {15, 0, 0}));  // through the wall
  }
}

TEST_CASE("stepping into a wall slides along it and follows the terrain", "[world][geom][movement]") {
  const CollisionMap map({}, {{5, 0, 1, 10, 0}}, {});
  Terrain terrain;
  terrain.hills.push_back({0, 0, 50, 4});
  const Bounds b{-100, -100, 100, 100};
  Vec3 p{0, 0, 0};
  for (int i = 0; i < 40; ++i) p = step_position(p, MoveIntent{0.8f, 0.6f, 0}, 7.0f, 0.05f, b, map, terrain, 0.4f);
  CHECK(p.x <= 3.6f + 1e-3f);  // stopped at the wall's face (x = 4 minus the body)
  CHECK(p.z > 5.0f);           // but kept sliding along it
  CHECK(std::fabs(p.y - terrain.height(p.x, p.z)) < 1e-4f);
}

TEST_CASE("dungeons: deterministic, exact room count, all connected, deepest room is the boss", "[world][dungeon]") {
  DungeonParams p;
  p.seed = 777;
  p.rooms = 12;
  const DungeonLayout a = generate_dungeon(p);
  const DungeonLayout b = generate_dungeon(p);
  REQUIRE(a.rooms.size() == 12);
  CHECK(dungeon_hash(a) == dungeon_hash(b));
  p.seed = 778;
  CHECK(dungeon_hash(generate_dungeon(p)) != dungeon_hash(a));
  const auto& e = a.rooms[0];
  for (std::size_t i = 0; i < a.rooms.size(); ++i) {
    const auto& r = a.rooms[i];
    CHECK(reachable_floor(a, e.x + e.w / 2, e.z + e.h / 2, r.x + r.w / 2, r.z + r.h / 2));
    CHECK(r.depth <= a.rooms[static_cast<std::size_t>(a.boss)].depth);
    CHECK(a.room_radius(static_cast<int>(i)) > 0.0f);
    const Vec3 c = a.room_center(static_cast<int>(i));
    CHECK_FALSE(CollisionMap({}, {}, a.tiles).overlaps(c.x, c.z, 0.5f));
  }
  CHECK(a.rooms[static_cast<std::size_t>(a.boss)].w == p.sector - 2);
  // The border is always wall.
  for (int x = 0; x < a.tiles.w; ++x) CHECK(a.tiles.solid_cell(x, 0));
  CHECK(check_dungeon_params(DungeonParams{1, 1}).find("rooms") != std::string::npos);
  CHECK(generate_dungeon(DungeonParams{1, 1}).rooms.empty());  // invalid parameters build nothing
}

TEST_CASE("dungeon golden layout (the client must match)", "[world][dungeon]") {
  DungeonParams p;
  p.seed = kGoldenSeed;
  p.rooms = kGoldenRooms;
  const DungeonLayout d = generate_dungeon(p);
  XorShift32 r(1);
  CHECK(r.next() == 270369u);  // xorshift32 from 1, the first value every implementation agrees on
  INFO("golden hash " << dungeon_hash(d));
  CHECK(dungeon_hash(d) == 3734157024u);
}

TEST_CASE("nav grid: paths go around walls; enclosed goals are not reached", "[world][nav]") {
  // A wall from z = -20 to 20 at x = 0, with open ends.
  const CollisionMap map({}, {{0, 0, 1, 20, 0}}, {});
  NavGrid nav;
  nav.build({-50, -50, 50, 50}, 1.0f, map, 0.5f);
  REQUIRE_FALSE(nav.empty());
  CHECK(nav.blocked_count() > 0);
  CHECK_FALSE(nav.walkable(0, 0));
  CHECK(nav.walkable(-10, 0));
  CHECK_FALSE(nav.line_walkable({-10, 0, 0}, {10, 0, 0}));
  std::vector<Vec3> path;
  REQUIRE(nav.find_path({-10, 0, 0}, {10, 0, 0}, path));
  CHECK(std::fabs(path.back().x - 10.0f) < 1e-4f);
  Vec3 at{-10, 0, 0};
  for (const auto& w : path) {
    CHECK(nav.line_walkable(at, w));
    at = w;
  }
  // A box around the goal: no way in.
  const CollisionMap boxed({}, {{20, 0, 6, 0.5f, 0}, {20, 6, 6, 0.5f, 0}, {14, 3, 0.5f, 3.5f, 0}, {26, 3, 0.5f, 3.5f, 0}}, {});
  NavGrid nav2;
  nav2.build({-50, -50, 50, 50}, 1.0f, boxed, 0.5f);
  REQUIRE(nav2.find_path({-10, 0, 3}, {20, 0, 3}, path));  // gets as close as it can
  CHECK(horizontal_distance(path.back(), {20, 0, 3}) > 2.0f);
  NavGrid none;
  none.build({-50, -50, 50, 50}, 1.0f, CollisionMap{}, 0.5f);
  CHECK(none.empty());  // nothing to avoid: mobs walk straight
}

TEST_CASE("shipped world: zones parse, lines land on entries, dungeons have layouts", "[world][content]") {
  const Content& c = content();
  REQUIRE_FALSE(c.zones.empty());
  for (const auto& z : c.zones) {
    INFO(z.id);
    CHECK(z.layout_hash != 0);
    if (z.has_dungeon) {
      REQUIRE(z.layout);
      CHECK(z.layout->rooms.size() == static_cast<std::size_t>(z.dungeon.rooms));
      for (const auto& sp : z.spawns) CHECK_FALSE(CollisionMap({}, {}, z.layout->tiles).overlaps(sp.x, sp.z, 0.4f));
    }
    for (const auto& l : z.zone_lines) {
      const ZoneDef* to = c.find_zone(l.to);
      REQUIRE(to);
      CHECK(to->find_entry(l.entry));
    }
  }
  for (const auto& n : c.npcs) CHECK(c.find_zone(n.zone));
}

namespace {

std::string world_error(const ContentDocs& d) {
  try {
    (void)parse_content(d);
  } catch (const ContentError& e) {
    return e.what();
  }
  return {};
}

json small_zone(const std::string& id, const std::string& kind = "outdoor") {
  return json::parse(R"({"id": ")" + id + R"(", "name": "Z", "kind": ")" + kind + R"(", "levels": {"min": 1, "max": 10},
    "bounds": {"min_x": -100, "min_z": -100, "max_x": 100, "max_z": 100},
    "safe_spawn": {"x": 0, "y": 0, "z": 0, "yaw": 0},
    "entries": [{"id": "west", "x": -80, "z": 0, "yaw": 0}, {"id": "east", "x": 80, "z": 0, "yaw": 0}]})");
}

}  // namespace

TEST_CASE("world content checks: lines, entries, reachability, dungeons, travel", "[world][content]") {
  auto base = read_content_docs(EF_CONTENT_DIR);
  REQUIRE(world_error(base).empty());
  SECTION("a line to a zone or entry that does not exist") {
    auto d = base;
    auto z = small_zone("lonely");
    z["zone_lines"] = json::parse(R"([{"id": "out", "x": 95, "z": 0, "hx": 4, "hz": 4, "to": "nowhere", "entry": "x"}])");
    d.zones["zones"].push_back(z);
    const auto msg = world_error(d);
    CHECK_THAT(msg, Catch::Matchers::ContainsSubstring("leads to unknown zone 'nowhere'"));
    CHECK_THAT(msg, Catch::Matchers::ContainsSubstring("zone 'lonely' cannot be reached from any starting city"));
  }
  SECTION("an entry inside a zone line would bounce players") {
    auto d = base;
    auto z = small_zone("bouncy", "test");
    z["zone_lines"] = json::parse(R"([{"id": "out", "x": 80, "z": 0, "hx": 4, "hz": 4, "to": "bouncy", "entry": "west"}])");
    d.zones["zones"].push_back(z);
    CHECK_THAT(world_error(d), Catch::Matchers::ContainsSubstring("entry 'east' lies inside zone line 'out'"));
  }
  SECTION("dungeon parameters and rooms are checked") {
    auto d = base;
    auto z = json::parse(R"({"id": "pit", "name": "Pit", "kind": "dungeon", "dungeon": {"seed": 5, "rooms": 1},
      "spawns": [{"mob": "ember_rat", "room": "boss"}]})");
    d.zones["zones"].push_back(z);
    CHECK_THAT(world_error(d), Catch::Matchers::ContainsSubstring("dungeon rooms must be 2..64"));
    d.zones["zones"].back()["dungeon"]["rooms"] = 4;
    d.zones["zones"].back()["spawns"] = json::parse(R"([{"mob": "ember_rat", "room": 9}])");
    CHECK_THAT(world_error(d), Catch::Matchers::ContainsSubstring("names room '9', which the layout does not have"));
  }
  SECTION("travel destinations and roles") {
    auto d = base;
    d.npcs["npcs"].push_back(json::parse(R"({"id": "bad_stone", "name": "Stone", "role": "teleporter", "zone": "proving_flats", "x": 5, "z": 5,
      "travel": [{"dest": "atlantis", "fee": 10}]})"));
    d.npcs["npcs"].push_back(json::parse(R"({"id": "bad_binder", "name": "Binder", "role": "binder", "zone": "proving_flats", "x": 6, "z": 5,
      "travel": [{"dest": "emberhold"}]})"));
    const auto msg = world_error(d);
    CHECK_THAT(msg, Catch::Matchers::ContainsSubstring("travels to unknown destination 'atlantis'"));
    CHECK_THAT(msg, Catch::Matchers::ContainsSubstring("a binder offers exactly one option"));
  }
}

TEST_CASE("walking into a zone line asks to leave for the entry it names", "[world][zone]") {
  ZoneDef z = test_zone();
  z.zone_lines.push_back({"edge", 40, 0, 4, 10, 0, "proving_flats", "emberhold", ""});
  Zone zone(z, content(), kDt, 3);
  RecordingSink sink;
  std::uint32_t t = 0;
  const EntityId p = zone.add_player(at(30, 0, "Walker"));
  std::uint32_t seq = 1;
  std::vector<ZoneTransfer> moves;
  int steps = 0;
  for (; steps < 80 && moves.empty(); ++steps) {
    const PlayerEntity* pl = zone.find(p);
    const Vec3 next = step_position(pl->pos, MoveIntent{1, 0, 0}, content().rules.movement.run_speed, kDt, zone.bounds(),
                                    zone.collision(), z.terrain, kPlayerRadius);
    zone.queue_input(p, seq++, MoveIntent{1, 0, 0}, next);
    zone.tick(++t, sink);
    moves = zone.take_transfers();
  }
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].player == p);
  CHECK(moves[0].to.zone == "proving_flats");
  const ZoneEntryDef* e = content().find_zone("proving_flats")->find_entry("emberhold");
  CHECK(horizontal_distance(moves[0].to.pos, {e->at.x, 0, e->at.z}) < 0.01f);
  CHECK(zone.find(p)->transferring);
  zone.tick(++t, sink);
  CHECK(zone.take_transfers().empty());  // asked once
  // Someone arriving inside a line is not sent straight back.
  Zone zone2(z, content(), kDt, 4);
  const EntityId q = zone2.add_player(at(40, 0, "Arrival"));
  zone2.queue_input(q, 1, MoveIntent{0, 0, 0}, zone2.find(q)->pos);
  zone2.tick(1, sink);
  CHECK(zone2.take_transfers().empty());
}

TEST_CASE("mobs path around walls to reach a player", "[world][zone][nav]") {
  ZoneDef z = test_zone();
  z.walls.push_back({0, 0, 1, 30, 0});  // a wall between the mob and its foe
  Zone zone(z, content(), kDt, 4);
  RecordingSink sink;
  std::uint32_t t = 0;
  REQUIRE_FALSE(zone.nav().empty());
  const EntityId p = zone.add_player(at(10, 0, "Bait", 1));
  MobDef def = mob("ember_rat");
  def.aggro_radius = 0;
  def.leash_radius = 300;
  def.run_speed = 8;
  const EntityId m = zone.spawn_mob(def, {-10, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  zone.use_ability(p, "iron_challenge", m, sink);  // a taunt from across the wall pulls it
  if (!zone.find_mob(m)->threat.contains(p)) {
    zone.find_mob(m)->threat.add(p, 100.0);
  }
  float best = 1e9f;
  for (int i = 0; i < 400; ++i) {
    zone.tick(++t, sink);
    const MobEntity* mm = zone.find_mob(m);
    REQUIRE(mm);
    CHECK_FALSE(zone.collision().overlaps(mm->pos.x, mm->pos.z, 0.45f));  // never inside the wall
    best = std::min(best, horizontal_distance(mm->pos, zone.find(p)->pos));
  }
  CHECK(best < 5.0f);  // it got round
}

TEST_CASE("travel NPCs: soulbinders bind, waystones charge, ferries sail after boarding", "[world][zone][travel]") {
  Content c = content();  // a copy with a ferry added
  NpcDef ferry;
  ferry.id = "test_ferry";
  ferry.name = "Ferryman Test";
  ferry.role = "ferry";
  ferry.zone = "emberhold";
  ferry.x = 150;
  ferry.z = 140;
  ferry.travel.push_back({"cragmouth", 30, 2.0f});
  c.npcs.push_back(ferry);
  Zone zone(*c.find_zone("emberhold"), c, kDt, 9);
  RecordingSink sink;
  std::uint32_t t = 0;
  EntityId binder = 0, stone = 0, boat = 0;
  zone.for_each_npc([&](const NpcEntity& n) {
    if (n.def->role == "binder") binder = n.id;
    if (n.def->role == "teleporter") stone = n.id;
    if (n.def->role == "ferry") boat = n.id;
  });
  REQUIRE(binder);
  REQUIRE(stone);
  REQUIRE(boat);
  const Vec3 bpos = zone.find_npc(binder)->pos;
  const EntityId p = zone.add_player(at(bpos.x, bpos.z + 3, "Traveller", 10));
  zone.tick(++t, sink);
  zone.travel_open(p, binder, sink);
  REQUIRE(sink.last<proto::TravelOptions>(p));
  CHECK(sink.last<proto::TravelOptions>(p)->options.size() == 1);
  const int saves = sink.saves;
  zone.travel_choose(p, binder, "bind", sink);
  CHECK(zone.find(p)->bind_zone == "emberhold");
  CHECK(horizontal_distance(zone.find(p)->bind_pos, zone.find(p)->pos) < 0.01f);
  CHECK(sink.saves == saves + 1);
  zone.travel_choose(p, stone, "thornwood", sink);  // out of reach of the stone
  CHECK(zone.take_transfers().empty());

  // The waystone: costs coin, then leaves for the destination's zone.
  const Vec3 spos = zone.find_npc(stone)->pos;
  const EntityId q = zone.add_player(at(spos.x + 2, spos.z + 2, "Hopper", 10));
  zone.tick(++t, sink);
  zone.find(q)->coin = 49;
  zone.travel_choose(q, stone, "thornwood", sink);
  CHECK(zone.take_transfers().empty());
  zone.find(q)->coin = 60;
  zone.travel_choose(q, stone, "thornwood", sink);
  auto moves = zone.take_transfers();
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].to.zone == c.find_city("thornwood")->zone);
  CHECK(zone.find(q)->coin == 10);

  // The ferry: board, wait, then sail (paying on departure); walking off the dock misses it.
  const Vec3 fpos = zone.find_npc(boat)->pos;
  const EntityId f1 = zone.add_player(at(fpos.x + 1, fpos.z, "Sailor", 10));
  const EntityId f2 = zone.add_player(at(fpos.x - 1, fpos.z, "Lubber", 10));
  zone.tick(++t, sink);
  zone.find(f1)->coin = 100;
  zone.find(f2)->coin = 100;
  zone.travel_choose(f1, boat, "cragmouth", sink);
  zone.travel_choose(f2, boat, "cragmouth", sink);
  CHECK(zone.take_transfers().empty());
  zone.find(f2)->pos = {fpos.x - 60, 0, fpos.z};
  for (int i = 0; i < 45; ++i) zone.tick(++t, sink);
  moves = zone.take_transfers();
  REQUIRE(moves.size() == 1);
  CHECK(moves[0].player == f1);
  CHECK(zone.find(f1)->coin == 70);
  CHECK(zone.find(f2)->coin == 100);
  CHECK(sink.chat_count(f2, "sails without you") == 1);
}

TEST_CASE("a summoned pet follows its owner into the next zone", "[world][zone][pets]") {
  Zone zone(test_zone(), content(), kDt, 10);
  RecordingSink sink;
  const EntityId p = zone.add_player(at(0, 0, "Petkeeper", 20));
  zone.tick(1, sink);
  const PetDef& def = content().pets.front();
  zone.restore_pet(p, def.id, 20, 0.5f, sink);
  const PlayerEntity* pl = zone.find(p);
  REQUIRE(pl->pet != 0);
  const MobEntity* pet = zone.find_mob(pl->pet);
  REQUIRE(pet);
  CHECK(pet->pet_def == &def);
  CHECK(std::abs(pet->hp - pet->stats.max_hp / 2) <= 1);
  zone.restore_pet(p, def.id, 20, 1.0f, sink);  // never a second one
  std::size_t pets = 0;
  zone.for_each_mob([&](const MobEntity& m) { pets += m.owner == p ? 1 : 0; });
  CHECK(pets == 1);
}
