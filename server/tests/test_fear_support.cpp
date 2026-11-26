// Phase 15: feared players run (the server steers them away from the fear's caster), and mobs with a
// support special heal or buff the allies fighting beside them.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "zone_support.hpp"

using namespace zt;

namespace {

int heals_from(const RecordingSink& sink, EntityId to, EntityId healer, EntityId healed) {
  int n = 0;
  for (const auto& e : sink.all<proto::CombatEvent>(to))
    if (e.kind == static_cast<std::uint8_t>(CombatKind::Heal) && e.source_id == healer && e.target_id == healed && e.amount > 0) ++n;
  return n;
}

bool carries(const MobEntity& m, const std::string& ability) {
  return std::any_of(m.buffs.begin(), m.buffs.end(), [&](const ActiveBuff& b) { return b.ability_id == ability; });
}

}  // namespace

TEST_CASE("a feared player runs away from the fear's caster, whatever they press, then walks again", "[fear][zone][movement]") {
  Zone zone(test_zone(), content(), kDt, 71);
  RecordingSink sink;
  static MobDef wraith = mob("road_brigand");
  wraith.aggro_radius = 0;
  wraith.damage_mult = 0.0f;
  const EntityId m = zone.spawn_mob(wraith, {3, 0, 0});
  const EntityId p = zone.add_player(at(0, 0, "Fearful", 40));
  std::uint32_t t = 0;
  zone.tick(++t, sink);
  const AbilityDef* wail = content().find_mob_spell("wail_31");
  REQUIRE(wail);
  REQUIRE(zone.force_effect(p, *wail, 31, sink, m, "a test wraith") == StackAction::Add);
  std::uint32_t seq = 0;
  // Pressing toward the wraith the whole time.
  for (int i = 0; i < 40; ++i) {
    zone.queue_input(p, ++seq, MoveIntent{1.0f, 0.0f, -1.5708f}, zone.find(p)->pos);
    zone.tick(++t, sink);
  }
  const PlayerEntity* pl = zone.find(p);
  CHECK(pl->fleeing);
  CHECK(pl->pos.x < -3.0f);                      // ran away from the wraith at +x
  CHECK(std::abs(pl->flee_yaw - 1.5708f) < 0.1f);  // heading -x: forward_of(pi/2)
  auto st = sink.last<proto::PlayerStats>(p);
  REQUIRE(st);
  CHECK((st->flags & kFlagFleeing) != 0);
  CHECK(std::abs(st->flee_yaw - pl->flee_yaw) < 0.01f);
  // The fear wears off (four seconds): the player goes where they press again.
  for (int i = 0; i < 60; ++i) {
    zone.queue_input(p, ++seq, MoveIntent{0.0f, 0.0f, 0.0f}, zone.find(p)->pos);
    zone.tick(++t, sink);
  }
  REQUIRE_FALSE(zone.find(p)->fleeing);
  const float x0 = zone.find(p)->pos.x;
  for (int i = 0; i < 10; ++i) {
    zone.queue_input(p, ++seq, MoveIntent{1.0f, 0.0f, -1.5708f}, zone.find(p)->pos);
    zone.tick(++t, sink);
  }
  CHECK(zone.find(p)->pos.x > x0 + 1.0f);
  CHECK(sink.last<proto::PlayerStats>(p)->flee_yaw == 0.0f);
}

TEST_CASE("a fleeing player turns aside at a wall, and never runs out through a zone line", "[fear][zone][movement]") {
  ZoneDef zd = test_zone();
  // A wall just behind the player (to the -x side), and a zone line beyond the wall's end.
  WallBox w;
  w.x = -2.0f;
  w.z = 0.0f;
  w.hx = 0.5f;
  w.hz = 6.0f;
  zd.walls.push_back(w);
  ZoneLineDef line;
  line.id = "out";
  line.x = 0.0f;
  line.z = 9.0f;
  line.hx = line.hz = 3.0f;
  line.to = "emberglade";
  line.entry = content().find_zone("emberglade")->entries.front().id;
  zd.zone_lines.push_back(line);
  Zone zone(zd, content(), kDt, 72);
  RecordingSink sink;
  static MobDef foe = mob("road_brigand");
  foe.aggro_radius = 0;
  foe.damage_mult = 0.0f;
  const EntityId m = zone.spawn_mob(foe, {3, 0, 0});
  const EntityId p = zone.add_player(at(0, 0, "Cornered", 40));
  std::uint32_t t = 0;
  for (int i = 0; i < 25; ++i) zone.tick(++t, sink);
  REQUIRE(zone.force_effect(p, *content().find_mob_spell("wail_31"), 31, sink, m, "a foe") == StackAction::Add);
  std::uint32_t seq = 0;
  float moved = 0.0f;
  Vec3 last = zone.find(p)->pos;
  for (int i = 0; i < 70; ++i) {
    zone.queue_input(p, ++seq, MoveIntent{0.0f, 0.0f, 0.0f}, zone.find(p)->pos);
    zone.tick(++t, sink);
    const Vec3 now = zone.find(p)->pos;
    moved += horizontal_distance(now, last);
    last = now;
  }
  // Running along or away from the wall rather than standing pressed against it.
  CHECK(moved > 6.0f);
  CHECK(zone.take_transfers().empty());
}

TEST_CASE("a mob healer mends the most hurt ally in its fight", "[mob][zone][support]") {
  Zone zone(test_zone(), content(), kDt, 73);
  RecordingSink sink;
  static MobDef healer = mob("ashen_acolyte");
  healer.damage_mult = 0.01f;
  healer.aggro_radius = 20;
  static MobDef fighter = mob("cinder_zealot");
  fighter.specials.clear();
  fighter.damage_mult = 0.01f;
  fighter.aggro_radius = 20;
  REQUIRE(std::any_of(healer.specials.begin(), healer.specials.end(), [](const MobSpecial& s) { return s.type == MobSpecialType::Support; }));
  const EntityId h = zone.spawn_mob(healer, {5, 0, 0});
  const EntityId f = zone.spawn_mob(fighter, {5, 0, 2});
  const EntityId p = zone.add_player(at(0, 0, "Target", 50));
  // A mob of another faction, hurt but in no fight with the healer's side: never healed.
  static MobDef stranger = mob("road_brigand");
  stranger.aggro_radius = 0;
  const EntityId s = zone.spawn_mob(stranger, {6, 0, -2});
  std::uint32_t t = 0;
  for (int i = 0; i < 20; ++i) zone.tick(++t, sink);
  REQUIRE_FALSE(zone.find_mob(f)->threat.entries().empty());  // both are fighting the player
  MobEntity* fm = zone.find_mob(f);
  fm->hp = fm->stats.max_hp / 4;
  zone.find_mob(s)->hp = zone.find_mob(s)->stats.max_hp / 4;
  const int hurt = fm->hp;
  for (int i = 0; i < 20 * 25; ++i) {
    zone.tick(++t, sink);
    if (zone.find(p)->dead) break;
  }
  REQUIRE_FALSE(zone.find(p)->dead);
  CHECK(zone.find_mob(f)->hp > hurt);
  CHECK(heals_from(sink, p, h, f) >= 1);
  CHECK(heals_from(sink, p, h, s) == 0);
}

TEST_CASE("a war chant buffs the caster and its allies once each", "[mob][zone][support]") {
  Zone zone(test_zone(), content(), kDt, 74);
  RecordingSink sink;
  static MobDef chief = mob("skulk_warlord");
  chief.damage_mult = 0.01f;
  chief.aggro_radius = 20;
  static MobDef grunt = mob("skulk_trapper");
  grunt.damage_mult = 0.01f;
  grunt.aggro_radius = 20;
  std::string chant;
  for (const auto& sp : chief.specials)
    if (sp.type == MobSpecialType::Support) chant = sp.effect;
  REQUIRE_FALSE(chant.empty());
  const EntityId c = zone.spawn_mob(chief, {3, 0, 0});
  const EntityId g = zone.spawn_mob(grunt, {3, 0, 2});
  for (const EntityId e : {c, g}) {  // they outlast the fight
    MobEntity* m = zone.find_mob(e);
    m->stats.max_hp = m->hp = 100000;
  }
  const EntityId p = zone.add_player(at(0, 0, "Target", 18));
  std::uint32_t t = 0;
  zone.tick(++t, sink);
  zone.set_target(p, c, sink);
  zone.set_auto_attack(p, true, sink);
  for (int i = 0; i < 20 * 5; ++i) zone.tick(++t, sink);
  REQUIRE_FALSE(zone.find_mob(c)->threat.entries().empty());
  REQUIRE_FALSE(zone.find_mob(g)->threat.entries().empty());  // the grunt joins its chief
  for (int i = 0; i < 20 * 40; ++i) zone.tick(++t, sink);  // the first chant waits one interval (20 s), the next another
  CHECK(carries(*zone.find_mob(c), chant));
  CHECK(carries(*zone.find_mob(g), chant));
}

TEST_CASE("a feared player who sends no input still runs", "[fear][zone][movement]") {
  Zone zone(test_zone(), content(), kDt, 75);
  RecordingSink sink;
  static MobDef foe = mob("road_brigand");
  foe.aggro_radius = 0;
  foe.damage_mult = 0.0f;
  const EntityId m = zone.spawn_mob(foe, {3, 0, 0});
  const EntityId p = zone.add_player(at(0, 0, "Silent", 40));
  std::uint32_t t = 0;
  zone.tick(++t, sink);
  REQUIRE(zone.force_effect(p, *content().find_mob_spell("wail_31"), 31, sink, m, "a foe") == StackAction::Add);
  for (int i = 0; i < 20; ++i) zone.tick(++t, sink);  // a client that stops sending
  CHECK(zone.find(p)->pos.x < -2.0f);
  // A burst of inputs moves no faster: one step a tick, the inputs only acknowledged (two a tick).
  const float x0 = zone.find(p)->pos.x;
  std::uint32_t seq = 0;
  for (int k = 0; k < 4; ++k) zone.queue_input(p, ++seq, MoveIntent{-1.0f, 0.0f, 1.5708f}, zone.find(p)->pos);
  zone.tick(++t, sink);
  zone.tick(++t, sink);
  const float step = content().rules.movement.run_speed * kDt;
  CHECK(x0 - zone.find(p)->pos.x < 2.0f * step + 0.01f);
  CHECK(zone.find(p)->last_applied_seq == seq);  // all of them acknowledged
}

TEST_CASE("a mob's heal over time ticks on its ally", "[mob][zone][support]") {
  Zone zone(test_zone(), content(), kDt, 76);
  RecordingSink sink;
  static MobDef caller = mob("mirefolk_tidecaller");
  caller.damage_mult = 0.01f;
  caller.aggro_radius = 20;
  static MobDef hunter = mob("mirefolk_hunter");
  hunter.damage_mult = 0.01f;
  hunter.aggro_radius = 20;
  const EntityId c = zone.spawn_mob(caller, {4, 0, 0});
  const EntityId h = zone.spawn_mob(hunter, {4, 0, 2});
  for (const EntityId e : {c, h}) {
    MobEntity* mm = zone.find_mob(e);
    mm->stats.max_hp = 10000;
  }
  zone.find_mob(c)->hp = 10000;
  zone.find_mob(h)->hp = 2000;
  const EntityId p = zone.add_player(at(0, 0, "Bait", 20));
  std::uint32_t t = 0;
  zone.tick(++t, sink);
  zone.set_target(p, c, sink);
  zone.set_auto_attack(p, true, sink);
  for (int i = 0; i < 20 * 30; ++i) zone.tick(++t, sink);
  int ticks = 0;
  for (const auto& e : sink.all<proto::CombatEvent>(p))
    if (e.kind == static_cast<std::uint8_t>(CombatKind::HotTick) && e.target_id == h && e.amount > 0) ++ticks;
  CHECK(ticks >= 2);
  CHECK(zone.find_mob(h)->hp > 2000);
}

TEST_CASE("fear holds a player on a zone line back; once it ends, the line takes them", "[fear][zone][travel]") {
  ZoneDef zd = test_zone();
  ZoneLineDef line;
  line.id = "out";
  line.x = 0.0f;
  line.z = 0.0f;
  line.hx = line.hz = 60.0f;  // wide enough to stay on while running
  line.to = "emberglade";
  line.entry = content().find_zone("emberglade")->entries.front().id;
  zd.zone_lines.push_back(line);
  Zone zone(zd, content(), kDt, 77);
  RecordingSink sink;
  static MobDef foe = mob("road_brigand");
  foe.aggro_radius = 0;
  foe.damage_mult = 0.0f;
  const EntityId m = zone.spawn_mob(foe, {2, 0, 0});
  const ZonePlayerInit init = at(0, 0, "Doorway", 40);
  const EntityId p = zone.add_player(init);
  zone.find(p)->arrived_tick = 0;
  std::uint32_t t = 0;
  REQUIRE(zone.force_effect(p, *content().find_mob_spell("wail_31"), 31, sink, m, "a foe") == StackAction::Add);
  for (int i = 0; i < 60; ++i) zone.tick(++t, sink);  // three seconds of the four, on the line
  CHECK(zone.take_transfers().empty());
  for (int i = 0; i < 60; ++i) zone.tick(++t, sink);  // the fear is over
  CHECK(zone.take_transfers().size() == 1);
}
