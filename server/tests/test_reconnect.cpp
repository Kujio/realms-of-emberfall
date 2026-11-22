// Phase 8: reconnect handling. A connection that drops mid-fight leaves the character in the world for
// linger_seconds (so pulling the plug is no escape); logging back in reclaims it in place; anything else
// lets it leave, saved, as a logout would.
#include <catch2/catch_test_macros.hpp>

#include "server_support.hpp"

using namespace wire;

namespace {

// Starts a fight: a feeble mob next to the player, targeted and auto-attacked, until the zone calls it combat.
EntityId start_fight(Harness& h, Client& c, const proto::EnterWorldResult& ew) {
  Zone* z = h.server->zone(ew.zone_id);
  REQUIRE(z);
  const MobDef* base = h.content.find_mob("road_brigand");
  REQUIRE(base);
  MobDef foe = *base;
  foe.id = "lingering_foe";
  foe.level = 1;
  foe.hp_mult = 50.0f;  // long enough to stay in combat
  foe.damage_mult = 0.01f;
  foe.loot = "";
  const EntityId m = z->spawn_mob(foe, {ew.x + 1.5f, ew.y, ew.z});
  c.send(proto::SetTarget{m});
  c.send(proto::AutoAttack{true});
  for (int i = 0; i < 100 && !z->player_in_combat(ew.entity_id); ++i) h.tick();
  REQUIRE(z->player_in_combat(ew.entity_id));
  return m;
}

std::uint64_t char_id_of(Harness& h, const Client& c) {
  auto created = h.last<proto::CharCreateResult>(c.peer);
  REQUIRE(created);
  return created->character.id;
}

}  // namespace

TEST_CASE("the login handshake carries the message of the day and the population", "[server][reconnect]") {
  Harness h;
  h.cfg.motd = "Double experience this weekend.";
  Client a(h);
  a.login_and_enter("popular", "Popular");
  Client b(h);
  b.hello();
  auto st = h.last<proto::ServerStatus>(b.peer);
  REQUIRE(st);
  CHECK(st->motd == "Double experience this weekend.");
  CHECK(st->players_online == 1);
  CHECK(st->max_players == h.cfg.max_players);
  CHECK_FALSE(st->server_version.empty());
}

TEST_CASE("a dropped connection out of combat leaves the world at once", "[server][reconnect]") {
  Harness h;
  Client a(h);
  a.login_and_enter("calm", "Calmly");
  h.server->on_disconnect(a.peer);
  CHECK(h.server->players_in_world() == 0);
  CHECK(h.server->lingering_count() == 0);
}

TEST_CASE("a dropped connection mid-fight lingers, then leaves saved when the timer runs out", "[server][reconnect]") {
  Harness h;
  Client a(h);
  const auto ew = a.login_and_enter("lingerer", "Lingerer");
  start_fight(h, a, ew);
  h.server->on_disconnect(a.peer);
  CHECK(h.server->lingering_count() == 1);
  CHECK(h.server->players_in_world() == 1);
  REQUIRE(h.server->zone(ew.zone_id));
  CHECK(h.server->zone(ew.zone_id)->find(ew.entity_id) != nullptr);  // still standing in the fight
  h.tick(static_cast<int>(h.cfg.linger_seconds * h.cfg.tick_rate) - 40);
  CHECK(h.server->lingering_count() == 1);
  h.tick(80);
  CHECK(h.server->lingering_count() == 0);
  CHECK(h.server->players_in_world() == 0);
  // It was saved like a logout: logging back in finds it and enters normally.
  Client b(h);
  b.login("lingerer", "password123", false);
  auto list = h.last<proto::CharList>(b.peer);
  REQUIRE(list);
  REQUIRE(list->characters.size() == 1);
  b.send(proto::EnterWorld{list->characters[0].id});
  h.tick();
  auto again = h.last<proto::EnterWorldResult>(b.peer);
  REQUIRE(again);
  CHECK(again->ok);
}

TEST_CASE("reconnecting mid-fight reclaims the same character where it stands", "[server][reconnect]") {
  Harness h;
  Client a(h);
  const auto ew = a.login_and_enter("comeback", "Comeback");
  const auto cid = char_id_of(h, a);
  const EntityId mob = start_fight(h, a, ew);
  {
    // The old connection had sent thousands of inputs (standing still here).
    const Vec3 at = h.server->zone(ew.zone_id)->find(ew.entity_id)->pos;
    a.send(proto::MoveInput{3000, 0.0f, 0.0f, 0.0f, at.x, at.y, at.z});
    h.tick();
  }
  h.server->on_disconnect(a.peer);
  h.tick(20);
  Client b(h);
  b.login("comeback", "password123", false);  // no "logged in elsewhere" kick for a lingering character
  CHECK(h.server->lingering_count() == 1);
  b.send(proto::EnterWorld{cid});
  h.tick(3);
  auto back = h.last<proto::EnterWorldResult>(b.peer);
  REQUIRE(back);
  REQUIRE(back->ok);
  CHECK(back->entity_id == ew.entity_id);  // the same entity, not a fresh copy from the database
  CHECK(h.server->lingering_count() == 0);
  CHECK(h.server->players_in_world() == 1);
  Zone* z = h.server->zone(ew.zone_id);
  REQUIRE(z);
  REQUIRE(z->find(ew.entity_id));
  CHECK(z->find(ew.entity_id)->session == b.peer);
  // The new client is shown its surroundings again, the mob it was fighting included.
  bool saw_mob = false;
  for (const auto& sp : h.received<proto::EntitySpawn>(b.peer)) saw_mob = saw_mob || sp.entity_id == mob;
  CHECK(saw_mob);
  CHECK(h.last<proto::Inventory>(b.peer));
  // The new connection counts its inputs from 1, below the old connection's count: they must move it.
  const Vec3 before = z->find(ew.entity_id)->pos;
  float pz = before.z;
  for (std::uint32_t seq = 1; seq <= 5; ++seq) {
    pz -= 0.35f;
    b.send(proto::MoveInput{seq, 0.0f, -1.0f, 0.0f, before.x, 0.0f, pz});
    h.tick();
  }
  CHECK(horizontal_distance(h.server->zone(ew.zone_id)->find(ew.entity_id)->pos, before) > 1.0f);
}

TEST_CASE("choosing another character lets the lingering one leave first", "[server][reconnect]") {
  Harness h;
  Client a(h);
  const auto ew = a.login_and_enter("twochars", "Firstone");
  start_fight(h, a, ew);
  h.server->on_disconnect(a.peer);
  Client b(h);
  b.login("twochars", "password123", false);
  b.send(proto::CharCreate{"Secondone", "highlander", "warrior", "emberhold", 0});
  h.tick();
  auto created = h.last<proto::CharCreateResult>(b.peer);
  REQUIRE(created);
  REQUIRE(created->ok);
  b.send(proto::EnterWorld{created->character.id});
  h.tick(2);
  CHECK(h.server->lingering_count() == 0);
  CHECK(h.server->players_in_world() == 1);
  auto in = h.last<proto::EnterWorldResult>(b.peer);
  REQUIRE(in);
  CHECK(in->ok);
  CHECK(in->entity_id != ew.entity_id);
}

TEST_CASE("a server shutdown ends every linger, saving the character", "[server][reconnect]") {
  Harness h;
  Client a(h);
  const auto ew = a.login_and_enter("kickme", "Kickme");
  start_fight(h, a, ew);
  h.server->on_disconnect(a.peer);
  REQUIRE(h.server->lingering_count() == 1);
  h.server->shutdown("Server restarting.");
  CHECK(h.server->lingering_count() == 0);
  CHECK(h.server->players_in_world() == 0);
}

TEST_CASE("a connection that never logs in is dropped after the login timeout", "[server][reconnect][anticheat]") {
  Harness h;
  Client idle(h);  // connects and says nothing
  Client half(h);
  half.hello();  // handshakes but never redeems a ticket
  Client player(h);
  player.login_and_enter("patient", "Patient");
  h.tick(static_cast<int>(h.cfg.login_timeout_seconds * h.cfg.tick_rate) - 40);
  CHECK_FALSE(h.transport.disconnecting.contains(idle.peer));
  h.tick(80);
  CHECK(h.transport.disconnecting.contains(idle.peer));
  CHECK(h.transport.disconnecting.contains(half.peer));
  CHECK_FALSE(h.transport.disconnecting.contains(player.peer));  // logged in long ago: unaffected
  auto kick = h.last<proto::Kick>(idle.peer);
  REQUIRE(kick);
  CHECK(kick->reason.find("timed out") != std::string::npos);
}
