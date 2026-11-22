// End-to-end session tests: real GameServer + in-memory SQLite + a fake transport.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <random>
#include <set>

#include "auth/auth_service.hpp"
#include "db/sqlite_repository.hpp"
#include "game/game_server.hpp"

using namespace ef;
using namespace ef::game;

namespace {

struct FakeTransport final : net::Transport {
  struct Sent {
    net::PeerId peer;
    std::uint8_t channel;
    std::vector<std::uint8_t> data;
  };
  std::vector<Sent> sent;
  std::set<net::PeerId> disconnecting;

  void send(net::PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool) override {
    sent.push_back({peer, channel, {data.begin(), data.end()}});
  }
  void disconnect_later(net::PeerId peer) override { disconnecting.insert(peer); }
};

// A repository whose character saves fail while `fail` is positive (each failed call counts down).
struct FlakyRepository final : db::Repository {
  db::SqliteRepository real{":memory:"};
  int fail = 0;
  std::optional<db::AccountRecord> find_account(std::string_view u) override { return real.find_account(u); }
  db::DbStatus create_account(std::string_view u, std::string_view h, std::int64_t& id) override { return real.create_account(u, h, id); }
  std::vector<db::CharacterRecord> list_characters(std::int64_t a) override { return real.list_characters(a); }
  db::DbStatus create_character(db::CharacterRecord& r, int m) override { return real.create_character(r, m); }
  db::DbStatus delete_character(std::int64_t a, std::int64_t c) override { return real.delete_character(a, c); }
  std::optional<db::CharacterRecord> load_character(std::int64_t a, std::int64_t c) override { return real.load_character(a, c); }
  db::DbStatus save_characters(const std::vector<db::CharacterRecord>& c) override {
    if (fail > 0) return --fail, db::DbStatus::Error;
    return real.save_characters(c);
  }
  db::DbStatus create_corpse(const db::CorpseRecord& c, const db::CharacterRecord& o) override { return real.create_corpse(c, o); }
  db::DbStatus update_corpse(const db::CorpseRecord& c, const db::CharacterRecord& l) override { return real.update_corpse(c, l); }
  db::DbStatus delete_corpse(std::int64_t id) override { return real.delete_corpse(id); }
  std::vector<db::CorpseRecord> load_corpses(std::int64_t now) override { return real.load_corpses(now); }
  std::optional<std::vector<db::AuctionRecord>> load_auctions() override { return real.load_auctions(); }
  db::DbStatus apply_auction(const std::vector<db::AuctionRecord>& u, const std::vector<std::int64_t>& d, const std::vector<db::CharacterRecord>& c,
                             bool ok) override {
    if (fail > 0) return --fail, db::DbStatus::Error;
    return real.apply_auction(u, d, c, ok);
  }
};

struct Harness {
  Config cfg;
  Content content = load_content(EF_CONTENT_DIR);
  FakeTransport transport;
  FlakyRepository* flaky = nullptr;  // set when the harness runs on a FlakyRepository
  db::DbWorker db;
  auth::TicketStore tickets;
  std::unique_ptr<auth::AuthService> auth;
  std::unique_ptr<GameServer> server;
  double now = 0.0;
  net::PeerId next_peer = 1;

  explicit Harness(std::uint32_t max_players = 200, bool flaky_db = false)
      : db(make_repo(flaky_db, flaky), db::DbWorker::Mode::Inline) {
    REQUIRE(auth::init_crypto());
    cfg.max_players = max_players;
    cfg.rng_seed = 99;  // deterministic combat
    auth = std::make_unique<auth::AuthService>(db, tickets, auth::PasswordParams::minimum());
    server = std::make_unique<GameServer>(cfg, content, transport, db, tickets, 7779, "test-cert");
  }

  static std::unique_ptr<db::Repository> make_repo(bool flaky_db, FlakyRepository*& out) {
    if (!flaky_db) return std::make_unique<db::SqliteRepository>(":memory:");
    auto r = std::make_unique<FlakyRepository>();
    out = r.get();
    return r;
  }

  void tick(int n = 1) {
    for (int i = 0; i < n; ++i) {
      now += 0.05;
      server->set_time(now);
      server->tick();
    }
  }

  template <class M>
  std::vector<M> received(net::PeerId peer) const {
    std::vector<M> out;
    for (const auto& s : transport.sent) {
      M m;
      if (s.peer == peer && proto::decode_packet(s.data, m)) out.push_back(m);
    }
    return out;
  }

  template <class M>
  std::optional<M> last(net::PeerId peer) const {
    auto v = received<M>(peer);
    if (v.empty()) return std::nullopt;
    return v.back();
  }
};

struct Client {
  Harness& h;
  net::PeerId peer;

  explicit Client(Harness& harness) : h(harness), peer(harness.next_peer++) { h.server->on_connect(peer, "127.0.0.1:1"); }

  template <class M>
  void send(const M& m) {
    h.server->on_packet(peer, M::kChannel, proto::encode_packet(m));
  }

  // Handshake; returns the session nonce the server assigned.
  std::string hello() {
    send(proto::Hello{proto::kProtocolVersion, "test"});
    auto hr = h.last<proto::HelloResult>(peer);
    REQUIRE(hr);
    REQUIRE(hr->ok);
    return hr->session_nonce;
  }

  // What the TLS login service does for this connection, minus the TLS socket.
  auth::LoginOutcome tls_login(const std::string& user, const std::string& pass, bool is_register) {
    auto hr = h.last<proto::HelloResult>(peer);
    REQUIRE(hr);
    return h.auth->authenticate({user, pass, is_register, hr->session_nonce}, std::chrono::seconds(5));
  }

  // Hello + TLS register/login + ticket redemption on the game connection.
  void login(const std::string& user, const std::string& pass = "password123", bool is_register = true) {
    hello();
    auto out = tls_login(user, pass, is_register);
    REQUIRE(out.ok);
    send(proto::TicketLogin{out.ticket});
    h.tick();
    REQUIRE(h.last<proto::AuthResult>(peer)->ok);
  }

  // Full happy path up to standing in the world.
  proto::EnterWorldResult login_and_enter(const std::string& user, const std::string& name, const std::string& class_id = "warrior") {
    login(user);
    send(proto::CharCreate{name, "highlander", class_id, "emberhold", 0});
    h.tick();
    auto created = h.last<proto::CharCreateResult>(peer);
    REQUIRE(created);
    REQUIRE(created->ok);
    send(proto::EnterWorld{created->character.id});
    h.tick();
    auto ew = h.last<proto::EnterWorldResult>(peer);
    REQUIRE(ew);
    REQUIRE(ew->ok);
    return *ew;
  }
};

}  // namespace

TEST_CASE("protocol version mismatch is rejected with a clear message", "[server]") {
  Harness h;
  Client c(h);
  c.send(proto::Hello{static_cast<std::uint16_t>(proto::kProtocolVersion + 1), "future"});
  auto r = h.last<proto::HelloResult>(c.peer);
  REQUIRE(r);
  CHECK_FALSE(r->ok);
  CHECK(r->server_protocol_version == proto::kProtocolVersion);
  CHECK(r->message.find("v" + std::to_string(proto::kProtocolVersion)) != std::string::npos);
  h.tick();
  CHECK(h.transport.disconnecting.contains(c.peer));

  CHECK(protocol_mismatch_message(1, 2)->find("Please update your client") != std::string::npos);
  CHECK_FALSE(protocol_mismatch_message(3, 3).has_value());
}

TEST_CASE("register, create, enter world, and see each other move", "[server]") {
  Harness h;
  Client a(h);
  Client b(h);
  auto ea = a.login_and_enter("alice", "Aldric");
  auto eb = b.login_and_enter("bobby", "Brenna");
  h.tick();

  // The starting area also has mobs in view; look at players only.
  auto players_spawned = [&](net::PeerId peer) {
    std::vector<proto::EntitySpawn> out;
    for (auto& sp : h.received<proto::EntitySpawn>(peer))
      if (sp.kind == static_cast<std::uint8_t>(EntityKind::Player)) out.push_back(sp);
    return out;
  };
  auto spawns_a = players_spawned(a.peer);
  REQUIRE(spawns_a.size() == 1);
  CHECK(spawns_a[0].name == "Brenna");
  CHECK(spawns_a[0].entity_id == eb.entity_id);
  REQUIRE(players_spawned(b.peer).size() == 1);

  // Alice walks north for one second.
  float pz = ea.z;
  for (std::uint32_t s = 1; s <= 20; ++s) {
    pz += ea.run_speed / static_cast<float>(ea.tick_rate);
    a.send(proto::MoveInput{s, 0.0f, 1.0f, 0.0f, ea.x, 0.0f, pz});
    h.tick();
  }
  auto snap_b = h.last<proto::Snapshot>(b.peer);
  REQUIRE(snap_b);
  auto alice = std::find_if(snap_b->entities.begin(), snap_b->entities.end(), [&](const auto& e) { return e.entity_id == ea.entity_id; });
  REQUIRE(alice != snap_b->entities.end());
  CHECK(alice->z > ea.z + 6.5f);
  auto snap_a = h.last<proto::Snapshot>(a.peer);
  CHECK(snap_a->ack_seq == 20);
  CHECK(h.received<proto::MoveCorrection>(a.peer).empty());

  // Say chat reaches the nearby player.
  a.send(proto::ChatSay{"  well met  "});
  auto chat = h.received<proto::ChatMessage>(b.peer);
  REQUIRE_FALSE(chat.empty());
  CHECK(chat.back().from_name == "Aldric");
  CHECK(chat.back().text == "well met");

  // Logout despawns for the other player.
  a.send(proto::Logout{});
  h.tick();
  auto despawns = h.received<proto::EntityDespawn>(b.peer);
  REQUIRE(std::count_if(despawns.begin(), despawns.end(), [&](const auto& d) { return d.entity_id == ea.entity_id; }) == 1);
  CHECK(h.server->players_in_world() == 1);
}

TEST_CASE("position persists across logout and re-login", "[server]") {
  Harness h;
  Client a(h);
  auto ea = a.login_and_enter("persist", "Pellam");
  for (std::uint32_t s = 1; s <= 10; ++s) {
    a.send(proto::MoveInput{s, 1.0f, 0.0f, 0.0f, ea.x + 0.35f * static_cast<float>(s), 0.0f, ea.z});
    h.tick();
  }
  a.send(proto::Logout{});
  h.tick();
  auto list = h.last<proto::CharList>(a.peer);
  REQUIRE(list);
  a.send(proto::EnterWorld{list->characters.at(0).id});
  h.tick();
  auto again = h.last<proto::EnterWorldResult>(a.peer);
  REQUIRE(again->ok);
  CHECK(again->x > ea.x + 3.0f);
}

TEST_CASE("passwords are checked by the TLS login service; bogus tickets lock out", "[server]") {
  Harness h;
  Client a(h);
  a.login("carol");

  Client b(h);
  b.hello();
  auto bad = b.tls_login("carol", "wrongpassword", false);
  CHECK_FALSE(bad.ok);
  CHECK(bad.ticket.empty());
  CHECK(bad.message == "Invalid username or password.");
  CHECK_FALSE(b.tls_login("nobody", "password123", false).ok);
  CHECK_FALSE(b.tls_login("carol", "password123", true).ok);  // already taken

  for (int i = 0; i < 5; ++i) {
    b.send(proto::TicketLogin{"not-a-real-ticket"});
    h.tick();
    CHECK_FALSE(h.last<proto::AuthResult>(b.peer)->ok);
  }
  CHECK(h.transport.disconnecting.contains(b.peer));
}

TEST_CASE("tickets are single-use and bound to the connection they were issued for", "[server]") {
  Harness h;
  Client a(h);
  Client thief(h);
  a.hello();
  thief.hello();
  auto out = a.tls_login("victim", "password123", true);
  REQUIRE(out.ok);

  thief.send(proto::TicketLogin{out.ticket});  // sniffed ticket, wrong connection
  h.tick();
  CHECK_FALSE(h.last<proto::AuthResult>(thief.peer)->ok);

  a.send(proto::TicketLogin{out.ticket});  // still valid for its owner
  h.tick();
  CHECK(h.last<proto::AuthResult>(a.peer)->ok);

  Client again(h);
  again.hello();
  again.send(proto::TicketLogin{out.ticket});  // already consumed
  h.tick();
  CHECK_FALSE(h.last<proto::AuthResult>(again.peer)->ok);
}

TEST_CASE("duplicate login kicks the older session", "[server]") {
  Harness h;
  Client a(h);
  a.login_and_enter("dupe", "Dorian");
  Client b(h);
  b.login("dupe", "password123", false);
  CHECK(h.transport.disconnecting.contains(a.peer));
  CHECK(h.last<proto::Kick>(a.peer)->reason.find("another location") != std::string::npos);
  CHECK(h.server->players_in_world() == 0);  // old character saved and removed
}

TEST_CASE("character rules are enforced server-side", "[server]") {
  Harness h;
  Client a(h);
  a.login("eve_1");

  a.send(proto::CharCreate{"Gorm", "deepkin", "wizard", "cragmouth", 0});  // class not allowed for race
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);
  a.send(proto::CharCreate{"Gorm", "deepkin", "warrior", "emberhold", 0});  // city not allowed for race
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);
  a.send(proto::CharCreate{"X1", "deepkin", "warrior", "cragmouth", 0});  // bad name
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);

  const char* names[] = {"Aone", "Atwo", "Athree", "Afour", "Afive"};
  for (const char* n : names) {
    a.send(proto::CharCreate{n, "highlander", "monk", "emberhold", 0});
    h.tick();
  }
  auto last = h.last<proto::CharCreateResult>(a.peer);
  CHECK_FALSE(last->ok);
  CHECK(last->message.find("4 characters") != std::string::npos);
  CHECK(h.last<proto::CharList>(a.peer)->characters.size() == 4);

  // Delete, and someone else's id is not deletable.
  const auto id = h.last<proto::CharList>(a.peer)->characters[0].id;
  a.send(proto::CharDelete{id + 1000});
  h.tick();
  CHECK_FALSE(h.last<proto::CharDeleteResult>(a.peer)->ok);
  a.send(proto::CharDelete{id});
  h.tick();
  CHECK(h.last<proto::CharDeleteResult>(a.peer)->ok);
  CHECK(h.last<proto::CharList>(a.peer)->characters.size() == 3);
}

TEST_CASE("messages in the wrong state or direction are violations", "[server]") {
  Harness h;
  Client a(h);
  a.send(proto::EnterWorld{1});  // before hello
  a.send(proto::Snapshot{});     // server->client type
  a.send(proto::Hello{proto::kProtocolVersion, "t"});
  a.send(proto::Hello{proto::kProtocolVersion, "t"});  // duplicate
  // Correct id but wrong channel.
  h.server->on_packet(a.peer, 0, proto::encode_packet(proto::MoveInput{}));
  CHECK_FALSE(h.transport.disconnecting.contains(a.peer));
  for (int i = 0; i < 10; ++i) a.send(proto::EnterWorld{1});
  h.tick();
  CHECK(h.transport.disconnecting.contains(a.peer));
}

TEST_CASE("malformed packet storm never crashes the server", "[server][fuzz]") {
  Harness h;
  Client good(h);
  auto e = good.login_and_enter("steady", "Steady");
  std::mt19937 rng(4242);

  // Many hostile peers in every state blast garbage and mutated packets.
  for (int round = 0; round < 60; ++round) {
    Client bad(h);
    const int stage = round % 3;
    if (stage == 1) bad.hello();
    if (stage == 2) bad.login("bad" + std::to_string(round));
    for (int i = 0; i < 400; ++i) {
      std::vector<std::uint8_t> pkt(rng() % 80);
      for (auto& b : pkt) b = static_cast<std::uint8_t>(rng());
      if (pkt.size() >= 2 && (i & 1)) {
        static const std::uint16_t ids[] = {1, 10, 11, 20, 23, 25, 27, 29, 42, 50};
        const auto id = ids[rng() % std::size(ids)];
        pkt[0] = static_cast<std::uint8_t>(id);
        pkt[1] = static_cast<std::uint8_t>(id >> 8);
      }
      h.server->on_packet(bad.peer, static_cast<std::uint8_t>(rng() % 3), pkt);
      h.tick();  // one intent per tick: under the packet rate limit, so every one is processed
    }
    h.tick();
    CHECK(h.transport.disconnecting.contains(bad.peer));
    h.server->on_disconnect(bad.peer);
  }
  h.server->on_packet(999999, 0, std::vector<std::uint8_t>{1, 2, 3});  // unknown peer

  // The honest player is unaffected and still simulated.
  CHECK_FALSE(h.transport.disconnecting.contains(good.peer));
  good.send(proto::MoveInput{1, 1.0f, 0.0f, 0.0f, e.x + 0.35f, 0.0f, e.z});
  h.tick(2);
  CHECK(h.last<proto::Snapshot>(good.peer)->ack_seq == 1);
}

TEST_CASE("rate limiting drops floods and eventually kicks", "[server]") {
  Harness h;
  Client a(h);
  a.login("flooder");
  // CharListRequest is legal in this state, so only the rate limiter can stop it.
  for (int i = 0; i < 5000; ++i) a.send(proto::CharListRequest{});
  CHECK(h.transport.disconnecting.empty());  // kick is applied at end of tick
  h.tick();
  CHECK(h.transport.disconnecting.contains(a.peer));
}

TEST_CASE("max_players caps how many characters are in the world", "[server]") {
  Harness h(1);
  Client a(h);
  a.login_and_enter("first", "Firstin");
  Client b(h);
  b.login("second");
  b.send(proto::CharCreate{"Secondin", "highlander", "warrior", "emberhold", 0});
  h.tick();
  b.send(proto::EnterWorld{h.last<proto::CharCreateResult>(b.peer)->character.id});
  h.tick();
  auto r = h.last<proto::EnterWorldResult>(b.peer);
  REQUIRE(r);
  CHECK_FALSE(r->ok);
  CHECK(r->message.find("full") != std::string::npos);
  CHECK(h.server->players_in_world() == 1);

  a.send(proto::Logout{});  // a slot frees up
  h.tick();
  b.send(proto::EnterWorld{h.last<proto::CharCreateResult>(b.peer)->character.id});
  h.tick();
  CHECK(h.last<proto::EnterWorldResult>(b.peer)->ok);
}

TEST_CASE("shutdown kicks everyone and saves", "[server]") {
  Harness h;
  Client a(h);
  a.login_and_enter("closer", "Closer");
  h.server->shutdown("bye");
  CHECK(h.last<proto::Kick>(a.peer)->reason == "bye");
  CHECK(h.server->players_in_world() == 0);
}

TEST_CASE("entering the world sends abilities, skills, inventory, and stats", "[server][combat]") {
  Harness h;
  Client a(h);
  a.login_and_enter("fighter", "Fighter");
  auto abilities = h.last<proto::AbilityList>(a.peer);
  REQUIRE(abilities);
  CHECK(abilities->abilities.size() >= 10);
  CHECK(abilities->abilities.front().level == 1);
  REQUIRE(h.last<proto::SkillValues>(a.peer));
  auto inv = h.last<proto::Inventory>(a.peer);
  REQUIRE(inv);
  CHECK(inv->bank_slots == 16);
  auto stats = h.last<proto::PlayerStats>(a.peer);
  REQUIRE(stats);
  CHECK(stats->level == 1);
  CHECK(stats->hp == stats->max_hp);
  CHECK(stats->max_hp > 0);
}

TEST_CASE("combat over the wire: target, fight, die, respawn, recover the corpse", "[server][combat]") {
  Harness h;
  Client a(h);
  auto ew = a.login_and_enter("warrior", "Warbold");
  Zone* z = h.server->zone(ew.zone_id);
  REQUIRE(z);
  // A rat right next to us.
  const EntityId rat = z->spawn_mob(*h.content.find_mob("ember_rat"), {ew.x + 2.0f, 0.0f, ew.z});
  h.tick();
  a.send(proto::SetTarget{rat});
  auto ti = h.last<proto::TargetInfo>(a.peer);
  REQUIRE(ti);
  CHECK(ti->entity_id == rat);
  CHECK(ti->name == "an ember rat");
  a.send(proto::UseAbility{"rising_kick", 0});
  CHECK(h.last<proto::AbilityResult>(a.peer)->ok);
  a.send(proto::AutoAttack{true});
  for (int i = 0; i < 20 * 120; ++i) {
    const MobEntity* m = z->find_mob(rat);
    if (!m || !m->alive()) break;
    h.tick();
  }
  const MobEntity* m = z->find_mob(rat);
  REQUIRE((!m || !m->alive()));
  CHECK_FALSE(h.received<proto::CombatEvent>(a.peer).empty());
  CHECK(h.last<proto::PlayerStats>(a.peer)->xp > 0);

  // Now something we cannot beat.
  const EntityId boss = z->spawn_mob(*h.content.find_mob("grimjaw"), {ew.x + 2.0f, 0.0f, ew.z + 1.0f});
  for (int i = 0; i < 20 * 60 && !h.last<proto::PlayerDied>(a.peer); ++i) h.tick();
  auto died = h.last<proto::PlayerDied>(a.peer);
  REQUIRE(died);
  CHECK(died->killer == "Grimjaw the Ancient");
  // Camping while dead revives at the bind point; the corpse stays behind (and is in the database).
  a.send(proto::Logout{});
  h.tick();
  CHECK(z->find_corpse(died->corpse_id) != nullptr);
  (void)boss;

  // A fresh server on the same database restores the corpse.
  h.server = std::make_unique<GameServer>(h.cfg, h.content, h.transport, h.db, h.tickets, 7779, "test-cert");
  h.tick();
  Zone* z2 = h.server->zone(ew.zone_id);
  REQUIRE(z2->corpse_count() == 1);
  std::string owner;
  z2->for_each_corpse([&](const CorpseEntity& c) { owner = c.owner_name; });
  CHECK(owner == "Warbold");
}

TEST_CASE("combat intents outside the world are violations; spam is dropped", "[server][combat]") {
  Harness h;
  Client a(h);
  a.hello();
  a.send(proto::SetTarget{1});
  a.send(proto::LootOpen{1});
  CHECK(h.server->session_count() == 1);
  Client b(h);
  b.login_and_enter("spammer", "Spammer");
  for (int i = 0; i < 200; ++i) b.send(proto::UseAbility{"rising_kick", 12345});
  h.tick();
  const auto results = h.received<proto::AbilityResult>(b.peer);
  CHECK(results.size() < 50);  // the action limiter dropped most of them
  CHECK_FALSE(h.transport.disconnecting.contains(b.peer));
}

namespace {
// The id of the character this client is playing (from the list it saw at character select).
std::uint64_t list_before_logout(Harness& h, Client& a) {
  const auto ew = h.last<proto::EnterWorldResult>(a.peer);
  REQUIRE(ew);
  const auto lists = h.received<proto::CharList>(a.peer);
  REQUIRE_FALSE(lists.empty());
  REQUIRE_FALSE(lists.back().characters.empty());
  return lists.back().characters.front().id;
}
}  // namespace

TEST_CASE("a wizard over the wire: cast, buy, scribe, memorize, and keep it all across a relog", "[server][spells]") {
  Harness h;
  Client a(h);
  auto ew = a.login_and_enter("caster", "Magewin", "wizard");
  Zone* z = h.server->zone(ew.zone_id);
  REQUIRE(z);
  auto book = h.last<proto::SpellBook>(a.peer);
  REQUIRE(book);
  CHECK(book->spells.size() == 2);
  auto bar = h.last<proto::GemBar>(a.peer);
  REQUIRE(bar);
  CHECK(bar->unlocked == 2);
  REQUIRE(bar->gems.size() == 8);
  CHECK(bar->gems[0].spell_id == "ember_dart");
  CHECK(h.last<proto::BuffList>(a.peer));

  const EntityId rat = z->spawn_mob(*h.content.find_mob("ember_rat"), {ew.x + 15.0f, 0.0f, ew.z});
  h.tick();
  a.send(proto::SetTarget{rat});
  a.send(proto::CastSpell{0, 0});
  auto start = h.last<proto::CastStart>(a.peer);
  REQUIRE(start);
  CHECK(start->spell_id == "ember_dart");
  h.tick(60);
  auto end = h.last<proto::CastEnd>(a.peer);
  REQUIRE(end);
  CHECK(end->result == static_cast<std::uint8_t>(CastResult::Completed));
  h.tick(40);                      // past the global cooldown
  a.send(proto::CastSpell{1, 0});  // Ward of Glass on ourselves
  h.tick(80);
  auto self = h.last<proto::BuffList>(a.peer);
  REQUIRE(self);
  bool warded = false;
  for (const auto& b : self->buffs) warded = warded || b.spell_id == "ward_of_glass";
  CHECK(warded);

  // Hold the rat (it is fighting us) so we can leave combat and memorize.
  if (z->find_mob(rat) && z->find_mob(rat)->alive()) {
    AbilityDef hold;
    hold.id = hold.name = hold.stack_group = "test_hold";
    hold.target = TargetType::Enemy;
    EffectDef mez;
    mez.type = EffectType::Mez;
    mez.duration = 600;
    hold.effects.push_back(mez);
    z->force_effect(rat, hold, 60, *h.server);
  }
  // Level up, buy and scribe a scroll from the trainer, and memorize it.
  PlayerEntity* me = nullptr;
  z->for_each_player([&](const PlayerEntity& p) { me = z->find(p.id); });
  REQUIRE(me);
  z->grant_xp(me->id, total_xp_for_level(5, h.content.rules.xp), *h.server);
  me->coin = 500;
  EntityId trainer = 0;
  z->for_each_npc([&](const NpcEntity& n) {
    if (n.def->id == "archmage_torvan") trainer = n.id;
  });
  REQUIRE(trainer != 0);
  h.tick(30);  // wait out combat before memorizing
  a.send(proto::TrainerOpen{trainer});
  auto win = h.last<proto::TrainerWindow>(a.peer);
  REQUIRE(win);
  CHECK(win->name == "Archmage Torvan Quill");
  a.send(proto::TrainerBuy{trainer, "scroll_frost_needle"});
  int slot = -1;
  const auto inv = h.last<proto::Inventory>(a.peer);
  REQUIRE(inv);
  for (const auto& it : inv->items)
    if (it.item_id == "scroll_frost_needle") slot = it.slot;
  REQUIRE(slot >= 0);
  a.send(proto::ScribeSpell{static_cast<std::uint16_t>(slot)});
  CHECK(h.last<proto::SpellBook>(a.peer)->spells.size() == 3);
  h.tick(20 * 7);
  a.send(proto::MemorizeSpell{2, "frost_needle"});
  {
    const auto bar_now = h.last<proto::GemBar>(a.peer);
    REQUIRE(bar_now);
    REQUIRE(bar_now->gems.size() >= 3);
    CHECK(bar_now->gems[2].spell_id == "frost_needle");
  }

  const std::uint64_t my_id = list_before_logout(h, a);
  h.transport.sent.clear();  // only what logging out sends counts below
  a.send(proto::Logout{});
  h.tick();
  auto list = h.last<proto::CharList>(a.peer);
  REQUIRE(list);  // logging out returns us to character select with a fresh list
  REQUIRE(std::any_of(list->characters.begin(), list->characters.end(), [&](const auto& c) { return c.id == my_id; }));
  h.transport.sent.clear();  // only what re-entering sends counts below
  a.send(proto::EnterWorld{my_id});
  h.tick();
  const auto again = h.last<proto::EnterWorldResult>(a.peer);
  REQUIRE(again);
  REQUIRE(again->ok);
  REQUIRE(h.last<proto::SpellBook>(a.peer));
  REQUIRE(h.last<proto::GemBar>(a.peer));
  CHECK(h.last<proto::SpellBook>(a.peer)->spells.size() == 3);
  {
    const auto bar_now = h.last<proto::GemBar>(a.peer);
    REQUIRE(bar_now);
    REQUIRE(bar_now->gems.size() >= 3);
    CHECK(bar_now->gems[2].spell_id == "frost_needle");
  }
  bool kept = false;
  const auto buffs = h.last<proto::BuffList>(a.peer);
  REQUIRE(buffs);
  for (const auto& b : buffs->buffs) kept = kept || b.spell_id == "ward_of_glass";
  CHECK(kept);
  // A bad gem is refused with a message.
  h.transport.sent.clear();
  a.send(proto::CastSpell{200, 0});
  const auto refused = h.last<proto::AbilityResult>(a.peer);
  REQUIRE(refused);
  CHECK(refused->message == "You have no spell memorized there.");
}

TEST_CASE("gender is validated, stored, and shown to other players", "[server][models]") {
  Harness h;
  Client a(h);
  a.login("gwen_g");
  a.send(proto::CharCreate{"Gwenna", "highlander", "warrior", "emberhold", 2});  // no such gender
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);
  a.send(proto::CharCreate{"Gorrik", "deepkin", "warrior", "cragmouth", 1});
  h.tick();
  auto created = h.last<proto::CharCreateResult>(a.peer);
  REQUIRE(created->ok);
  CHECK(created->character.gender == 1);
  REQUIRE(h.last<proto::CharList>(a.peer)->characters.size() == 1);
  CHECK(h.last<proto::CharList>(a.peer)->characters[0].gender == 1);
  a.send(proto::EnterWorld{created->character.id});
  h.tick();
  REQUIRE(h.last<proto::EnterWorldResult>(a.peer)->ok);

  Client b(h);
  b.login_and_enter("brin_g", "Brinna");  // gender 0
  h.tick();
  bool saw = false;
  for (const auto& sp : h.received<proto::EntitySpawn>(b.peer)) {
    if (sp.name != "Gorrik") continue;
    saw = true;
    CHECK(sp.gender == 1);
    CHECK(sp.race == "deepkin");
  }
  CHECK(saw);
  for (const auto& sp : h.received<proto::EntitySpawn>(a.peer))
    if (sp.name == "Brinna") CHECK(sp.gender == 0);
}

// ---------------------------------------------------------------- Phase 5: items over the wire

namespace {

int carried(const proto::Inventory& inv, const std::string& id) {
  int n = 0;
  for (const auto& s : inv.items)
    if (s.item_id == id) n += s.count;
  return n;
}

std::uint16_t slot_of(const proto::Inventory& inv, const std::string& id) {
  for (const auto& s : inv.items)
    if (s.item_id == id) return s.slot;
  return 0xFFFF;
}

// Logs an existing account back in and enters the world with its first character.
proto::EnterWorldResult relog(Harness& h, Client& c, const std::string& user) {
  c.login(user, "password123", false);
  auto list = h.last<proto::CharList>(c.peer);
  REQUIRE(list);
  REQUIRE_FALSE(list->characters.empty());
  c.send(proto::EnterWorld{list->characters.front().id});
  h.tick();
  auto ew = h.last<proto::EnterWorldResult>(c.peer);
  REQUIRE(ew);
  REQUIRE(ew->ok);
  return *ew;
}

}  // namespace

TEST_CASE("a new character starts with its class kit worn and packed", "[server][items]") {
  Harness h;
  Client a(h);
  a.login_and_enter("kitted", "Kitted", "warrior");
  auto inv = h.last<proto::Inventory>(a.peer);
  REQUIRE(inv);
  CHECK(slot_of(*inv, "training_sword") == equip_code(EquipSlot::Primary));
  CHECK(slot_of(*inv, "training_shield") == equip_code(EquipSlot::Secondary));
  CHECK(carried(*inv, "trail_rations") == 5);
  CHECK(slot_of(*inv, "cloth_pouch") == general_code(0));
  auto sheet = h.last<proto::CharacterSheet>(a.peer);
  REQUIRE(sheet);
  CHECK(sheet->weapon_damage >= 6);
}

TEST_CASE("trades over the wire are saved for both sides at once; a disconnect mid-trade moves nothing", "[server][items][trade]") {
  Harness h;
  Client a(h);
  Client b(h);
  const auto ea = a.login_and_enter("trader_a", "Traderone").entity_id;
  const auto eb = b.login_and_enter("trader_b", "Tradertwo").entity_id;
  Zone* z = h.server->zone("proving_flats");
  REQUIRE(z);
  z->find(ea)->pos = {0, 0, 0};
  z->find(eb)->pos = {3, 0, 0};
  REQUIRE(z->give(ea, "longsword_t2", 1, *h.server));
  z->find(eb)->coin = 400;
  h.tick(25);
  const auto sword = slot_of(*h.last<proto::Inventory>(a.peer), "longsword_t2");
  REQUIRE(sword != 0xFFFF);
  a.send(proto::TradeRequest{eb});
  REQUIRE(h.last<proto::TradeInvite>(b.peer));
  b.send(proto::TradeRespond{ea, true});
  a.send(proto::TradeSetItem{0, sword});
  const auto seen = h.last<proto::TradeState>(a.peer);  // what A looks at before B adds coin
  REQUIRE(seen);
  b.send(proto::TradeSetCoin{150});
  // A's accept names the offer it saw; B's coin changed it since, so it does not count.
  a.send(proto::TradeAccept{true, seen->trade_id, seen->revision});
  CHECK_FALSE(h.last<proto::TradeState>(b.peer)->their_accept);
  const auto now = h.last<proto::TradeState>(a.peer);
  a.send(proto::TradeAccept{true, now->trade_id, now->revision});
  b.send(proto::TradeAccept{true, now->trade_id, now->revision});
  h.tick();
  auto closed = h.last<proto::TradeClosed>(a.peer);
  REQUIRE(closed);
  CHECK(closed->completed);
  // Both leave at once; what was saved is what they log back in with.
  h.server->on_disconnect(a.peer);
  h.server->on_disconnect(b.peer);
  h.tick();
  Client a2(h);
  Client b2(h);
  const auto ea2 = relog(h, a2, "trader_a").entity_id;
  const auto eb2 = relog(h, b2, "trader_b").entity_id;
  CHECK(carried(*h.last<proto::Inventory>(a2.peer), "longsword_t2") == 0);
  CHECK(carried(*h.last<proto::Inventory>(b2.peer), "longsword_t2") == 1);
  CHECK(h.last<proto::Inventory>(b2.peer)->coin == 250);
  CHECK(h.last<proto::Inventory>(a2.peer)->coin >= 150);
  // Second trade: B offers the sword back, A accepts, B vanishes before accepting.
  z->find(ea2)->pos = {0, 0, 0};
  z->find(eb2)->pos = {3, 0, 0};
  h.tick(25);
  const auto back = slot_of(*h.last<proto::Inventory>(b2.peer), "longsword_t2");
  b2.send(proto::TradeRequest{ea2});
  a2.send(proto::TradeRespond{eb2, true});
  b2.send(proto::TradeSetItem{0, back});
  const auto st2 = h.last<proto::TradeState>(a2.peer);
  REQUIRE(st2);
  a2.send(proto::TradeAccept{true, st2->trade_id, st2->revision});
  h.server->on_disconnect(b2.peer);
  h.tick(2);
  auto gone = h.last<proto::TradeClosed>(a2.peer);
  REQUIRE(gone);
  CHECK_FALSE(gone->completed);
  CHECK(z->find(ea2)->trade == 0);
  CHECK(carried(*h.last<proto::Inventory>(a2.peer), "longsword_t2") == 0);
  Client b3(h);
  relog(h, b3, "trader_b");
  CHECK(carried(*h.last<proto::Inventory>(b3.peer), "longsword_t2") == 1);
}

TEST_CASE("the auction board over the wire: list with a fee, find, buy, and collect the proceeds", "[server][items][auction]") {
  Harness h;
  Client s(h);
  Client b(h);
  const auto es = s.login_and_enter("auc_seller", "Sellsword").entity_id;
  const auto eb = b.login_and_enter("auc_buyer", "Buyerling").entity_id;
  h.tick();  // auctions load
  Zone* z = h.server->zone("proving_flats");
  EntityId auctioneer = 0;
  z->for_each_npc([&](const NpcEntity& n) {
    if (n.def->role == "auctioneer") auctioneer = n.id;
  });
  REQUIRE(auctioneer != 0);
  const Vec3 at_npc = z->find_npc(auctioneer)->pos;
  z->find(es)->pos = {at_npc.x + 2, 0, at_npc.z};
  z->find(eb)->pos = {at_npc.x - 2, 0, at_npc.z};
  z->find(es)->coin = 100;
  z->find(eb)->coin = 5000;
  REQUIRE(z->give(es, "plate_t2_chest", 1, *h.server));
  h.tick(25);
  s.send(proto::AuctionOpen{auctioneer});
  REQUIRE(h.last<proto::AuctionWindow>(s.peer));
  const auto slot = slot_of(*h.last<proto::Inventory>(s.peer), "plate_t2_chest");
  s.send(proto::AuctionPost{auctioneer, slot, "longsword_t2", 1000});  // not what is in that slot: refused
  CHECK(h.server->auctions().size() == 0);
  s.send(proto::AuctionPost{auctioneer, slot, "plate_t2_chest", 1000});
  h.tick();
  CHECK(z->find(es)->coin == 50);  // 5% fee
  CHECK(carried(*h.last<proto::Inventory>(s.peer), "plate_t2_chest") == 0);
  REQUIRE(h.server->auctions().size() == 1);
  b.send(proto::AuctionSearch{auctioneer, "breastplate", "", 0, 0, 0, false});
  auto found = h.last<proto::AuctionResults>(b.peer);
  REQUIRE(found);
  REQUIRE(found->entries.size() == 1);
  CHECK(found->entries[0].seller == "Sellsword");
  b.send(proto::AuctionBuy{auctioneer, found->entries[0].id, 999});  // the listed price changed: refused
  CHECK(z->find(eb)->coin == 5000);
  b.send(proto::AuctionBuy{auctioneer, found->entries[0].id, 1000});
  h.tick();
  CHECK(z->find(eb)->coin == 4000);
  CHECK(carried(*h.last<proto::Inventory>(b.peer), "plate_t2_chest") == 1);
  s.send(proto::AuctionOpen{auctioneer});  // collects the sale
  h.tick();
  CHECK(z->find(es)->coin == 1050);
  CHECK(h.server->auctions().size() == 0);
  // Too far from the auctioneer: refused.
  z->find(eb)->pos = {at_npc.x + 100, 0, at_npc.z};
  h.tick(25);
  b.send(proto::AuctionPost{auctioneer, slot_of(*h.last<proto::Inventory>(b.peer), "plate_t2_chest"), "plate_t2_chest", 10});
  CHECK(h.server->auctions().size() == 0);
}

TEST_CASE("item intents from a hostile client are refused without harm", "[server][items][fuzz]") {
  Harness h;
  Client a(h);
  const auto ea = a.login_and_enter("prober", "Prober").entity_id;
  std::mt19937 rng(9);
  Zone* z = h.server->zone("proving_flats");
  for (int i = 0; i < 1500; ++i) {
    const auto s1 = static_cast<std::uint16_t>(rng());
    const auto s2 = static_cast<std::uint16_t>(rng() % 3 ? rng() % 0x300 : rng());
    switch (rng() % 8) {
      case 0: a.send(proto::MoveItem{s1, s2, static_cast<std::uint16_t>(rng())}); break;
      case 1: a.send(proto::DestroyItem{s1, "training_sword"}); break;
      case 2: a.send(proto::UseItem{s2, static_cast<std::uint32_t>(rng())}); break;
      case 3: a.send(proto::VendorSell{static_cast<std::uint32_t>(rng()), s2, 0}); break;
      case 4: a.send(proto::TradeSetItem{static_cast<std::uint8_t>(rng()), s2}); break;
      case 5: a.send(proto::BankCoin{rng() % 2 == 0, rng()}); break;
      case 6: a.send(proto::AuctionBuy{static_cast<std::uint32_t>(rng()), rng(), rng()}); break;
      case 7: a.send(proto::LootRollChoice{static_cast<std::uint32_t>(rng()), static_cast<std::uint8_t>(rng() % 3)}); break;
    }
    h.tick();  // one intent per tick: under the packet rate limit, so every one is processed
  }
  REQUIRE(z->find(ea));
  // The kit is still all there (moves may rearrange it, never duplicate it).
  int swords = 0;
  z->find(ea)->inv.for_each([&](SlotCode, const ItemStackState& st) {
    if (st.item_id == "training_sword") ++swords;
  });
  CHECK(swords <= 1);
}

TEST_CASE("a trade whose save fails is retried with both sides together, even after a logout", "[server][items][trade][db]") {
  Harness h(200, true);
  REQUIRE(h.flaky);
  Client a(h);
  Client b(h);
  const auto ea = a.login_and_enter("flaky_a", "Flakyone").entity_id;
  const auto eb = b.login_and_enter("flaky_b", "Flakytwo").entity_id;
  Zone* z = h.server->zone("proving_flats");
  z->find(ea)->pos = {0, 0, 0};
  z->find(eb)->pos = {3, 0, 0};
  REQUIRE(z->give(ea, "longsword_t2", 1, *h.server));
  h.tick(25);
  const auto sword = slot_of(*h.last<proto::Inventory>(a.peer), "longsword_t2");
  a.send(proto::TradeRequest{eb});
  b.send(proto::TradeRespond{ea, true});
  a.send(proto::TradeSetItem{0, sword});
  const auto st = h.last<proto::TradeState>(a.peer);
  REQUIRE(st);
  h.flaky->fail = 6;  // the trade save and its two retries fail, and so does the logout save below
  a.send(proto::TradeAccept{true, st->trade_id, st->revision});
  b.send(proto::TradeAccept{true, st->trade_id, st->revision});
  REQUIRE(h.last<proto::TradeClosed>(a.peer)->completed);
  h.tick();  // the failure comes back to the game thread: both sides are queued again
  h.server->on_disconnect(a.peer);  // A leaves while saves still fail: kept in memory for the retry
  h.tick();
  CHECK(h.flaky->fail == 0);
  h.tick(25);  // the next flush stores A (offline) and B together
  h.server->on_disconnect(b.peer);
  h.tick();
  Client a2(h);
  Client b2(h);
  relog(h, a2, "flaky_a");
  relog(h, b2, "flaky_b");
  CHECK(carried(*h.last<proto::Inventory>(a2.peer), "longsword_t2") == 0);
  CHECK(carried(*h.last<proto::Inventory>(b2.peer), "longsword_t2") == 1);
}
