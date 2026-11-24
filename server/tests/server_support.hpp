// Shared wire-test support: a real GameServer on in-memory (or file) SQLite behind a fake transport,
// and a scripted client that logs in and enters the world. Used by test_game_server.cpp and test_social_server.cpp.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "auth/auth_service.hpp"
#include "db/sqlite_repository.hpp"
#include "game/game_server.hpp"

namespace wire {

using namespace ef;
using namespace ef::game;


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
  // The game server's logic is tested in plaintext; the keys it hands over are recorded
  // (SecureTransport, which does the sealing, has its own tests).
  std::map<net::PeerId, net::SessionKey> keys;
  void enable_encryption(net::PeerId peer, const net::SessionKey& key) override { keys[peer] = key; }
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
  std::optional<db::AccountRecord> find_account_by_character(std::string_view n) override { return real.find_account_by_character(n); }
  db::DbStatus set_account_ban(std::int64_t a, std::int64_t u, std::string_view r) override { return real.set_account_ban(a, u, r); }
  db::DbStatus set_account_mute(std::int64_t a, std::int64_t u) override { return real.set_account_mute(a, u); }
  db::DbStatus set_account_admin(std::string_view u, bool a) override { return real.set_account_admin(u, a); }
  db::DbStatus append_audit(const db::AuditRecord& e) override { return real.append_audit(e); }
  std::vector<db::AuditRecord> recent_audit(int n) override { return real.recent_audit(n); }
  std::optional<std::vector<db::GuildRecord>> load_guilds() override { return real.load_guilds(); }
  db::DbStatus create_guild(std::string_view n, std::int64_t l, std::int64_t& id) override { return real.create_guild(n, l, id); }
  db::DbStatus delete_guild(std::int64_t g) override { return real.delete_guild(g); }
  db::DbStatus set_guild_motd(std::int64_t g, std::string_view m) override { return real.set_guild_motd(g, m); }
  db::DbStatus set_guild_member(std::int64_t g, std::int64_t c, int r) override { return real.set_guild_member(g, c, r); }
  db::DbStatus remove_guild_member(std::int64_t c) override { return real.remove_guild_member(c); }
  db::DbStatus set_guild_ranks(std::int64_t g, const std::vector<std::pair<std::int64_t, int>>& r) override { return real.set_guild_ranks(g, r); }
  std::optional<std::vector<db::GroupRecord>> load_groups() override { return real.load_groups(); }
  int fail_groups = 0;  // group saves that fail (each failed call counts down)
  db::DbStatus save_group(const db::GroupRecord& g) override {
    if (fail_groups > 0) return --fail_groups, db::DbStatus::Error;
    return real.save_group(g);
  }
  db::DbStatus delete_group(std::int64_t g) override { return real.delete_group(g); }
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

  // `start_zone`: the wire tests play in the test zone unless a test wants the real starting cities ("").
  // `db_path`: a database file that outlives the harness (restart tests); in memory by default.
  explicit Harness(std::uint32_t max_players = 200, bool flaky_db = false, const char* start_zone = "proving_flats",
                   std::uint32_t zone_idle_seconds = 1, const std::string& db_path = ":memory:")
      : db(make_repo(flaky_db, flaky, db_path), db::DbWorker::Mode::Inline) {
    REQUIRE(auth::init_crypto());
    cfg.max_players = max_players;
    cfg.rng_seed = 99;  // deterministic combat
    cfg.start_zone = start_zone;
    cfg.zone_idle_seconds = zone_idle_seconds;
    auth = std::make_unique<auth::AuthService>(db, tickets, auth::PasswordParams::minimum());
    server = std::make_unique<GameServer>(cfg, content, transport, db, tickets, 7779, "test-cert");
  }

  static std::unique_ptr<db::Repository> make_repo(bool flaky_db, FlakyRepository*& out, const std::string& path) {
    if (!flaky_db) return std::make_unique<db::SqliteRepository>(path);
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
    // Phase 10: the transport was told to seal this connection with the key the TLS reply carried.
    net::SessionKey key{};
    REQUIRE(net::parse_session_key_hex(out.session_key, key));
    REQUIRE(h.transport.keys.count(peer) == 1);
    REQUIRE(h.transport.keys.at(peer) == key);
  }

  // Phase 7: a slash command (without the slash) and a chat line.
  void cmd(const std::string& text) { send(proto::SlashCommand{text}); }
  void say_on(ChatChannel ch, const std::string& text, const std::string& target = "") { send(proto::ChatSend{chat_id(ch), target, text}); }

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


}  // namespace wire
