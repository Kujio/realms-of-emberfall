// End-to-end session tests: real GameServer + in-memory SQLite + a fake transport.
#include <catch2/catch_test_macros.hpp>

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

struct Harness {
  Config cfg;
  Content content = load_content(EF_CONTENT_DIR);
  FakeTransport transport;
  db::DbWorker db{std::make_unique<db::SqliteRepository>(":memory:"), db::DbWorker::Mode::Inline};
  auth::TicketStore tickets;
  std::unique_ptr<auth::AuthService> auth;
  std::unique_ptr<GameServer> server;
  double now = 0.0;
  net::PeerId next_peer = 1;

  explicit Harness(std::uint32_t max_players = 200) {
    REQUIRE(auth::init_crypto());
    cfg.max_players = max_players;
    auth = std::make_unique<auth::AuthService>(db, tickets, auth::PasswordParams::minimum());
    server = std::make_unique<GameServer>(cfg, content, transport, db, tickets, 7779, "test-cert");
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
  proto::EnterWorldResult login_and_enter(const std::string& user, const std::string& name) {
    login(user);
    send(proto::CharCreate{name, "highlander", "warrior", "emberhold"});
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

  auto spawns_a = h.received<proto::EntitySpawn>(a.peer);
  REQUIRE(spawns_a.size() == 1);
  CHECK(spawns_a[0].name == "Brenna");
  CHECK(spawns_a[0].entity_id == eb.entity_id);
  REQUIRE(h.received<proto::EntitySpawn>(b.peer).size() == 1);

  // Alice walks north for one second.
  float pz = ea.z;
  for (std::uint32_t s = 1; s <= 20; ++s) {
    pz += ea.run_speed / static_cast<float>(ea.tick_rate);
    a.send(proto::MoveInput{s, 0.0f, 1.0f, 0.0f, ea.x, 0.0f, pz});
    h.tick();
  }
  auto snap_b = h.last<proto::Snapshot>(b.peer);
  REQUIRE(snap_b);
  REQUIRE(snap_b->entities.size() == 1);
  CHECK(snap_b->entities[0].entity_id == ea.entity_id);
  CHECK(snap_b->entities[0].z > ea.z + 6.5f);
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
  REQUIRE(despawns.size() == 1);
  CHECK(despawns[0].entity_id == ea.entity_id);
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

  a.send(proto::CharCreate{"Gorm", "deepkin", "wizard", "cragmouth"});  // class not allowed for race
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);
  a.send(proto::CharCreate{"Gorm", "deepkin", "warrior", "emberhold"});  // city not allowed for race
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);
  a.send(proto::CharCreate{"X1", "deepkin", "warrior", "cragmouth"});  // bad name
  h.tick();
  CHECK_FALSE(h.last<proto::CharCreateResult>(a.peer)->ok);

  const char* names[] = {"Aone", "Atwo", "Athree", "Afour", "Afive"};
  for (const char* n : names) {
    a.send(proto::CharCreate{n, "highlander", "monk", "emberhold"});
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
      if (i % 50 == 0) h.tick();
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
  b.send(proto::CharCreate{"Secondin", "highlander", "warrior", "emberhold"});
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
