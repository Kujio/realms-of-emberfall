// libFuzzer target: a live game server. Each input is a stream of client packets
// ([channel u8][length u16 LE][bytes] ...) sent by a character standing in the test zone, with a server
// tick after every few. The server must never crash, trip a sanitizer, or let the session corrupt state.
// A session that gets kicked is replaced by a freshly logged-in one.
// Build with the "fuzz" preset (Clang), then: build/fuzz/tools/fuzz/emberfall-fuzz-server -max_total_time=300
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "auth/auth_service.hpp"
#include "auth/password.hpp"
#include "auth/tickets.hpp"
#include "core/config.hpp"
#include "db/db_worker.hpp"
#include "db/sqlite_repository.hpp"
#include "game/content.hpp"
#include "game/game_server.hpp"
#include "net/protocol.gen.hpp"
#include "net/transport.hpp"

#include <spdlog/spdlog.h>

namespace {

using namespace ef;

struct Sink final : net::Transport {
  std::vector<std::pair<net::PeerId, std::vector<std::uint8_t>>> sent;
  std::set<net::PeerId> closing;
  void send(net::PeerId peer, std::uint8_t, std::span<const std::uint8_t> d, bool) override {
    if (sent.size() < 4096) sent.emplace_back(peer, std::vector<std::uint8_t>(d.begin(), d.end()));
  }
  void disconnect_later(net::PeerId peer) override { closing.insert(peer); }
  template <class M>
  bool last(net::PeerId peer, M& out) const {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it)
      if (it->first == peer && proto::decode_packet(it->second, out)) return true;
    return false;
  }
};

struct World {
  Config cfg;
  game::Content content = game::load_content(EF_DEFAULT_CONTENT_DIR);
  Sink transport;
  db::DbWorker db{std::make_unique<db::SqliteRepository>(":memory:"), db::DbWorker::Mode::Inline};
  auth::TicketStore tickets;
  std::unique_ptr<auth::AuthService> auth;
  std::unique_ptr<game::GameServer> server;
  double now = 0.0;
  net::PeerId next_peer = 1;
  net::PeerId peer = 0;  // the fuzzed session, in the world
  std::uint64_t char_id = 0;
  int accounts = 0;

  World() {
    spdlog::set_level(spdlog::level::off);
    auth::init_crypto();
    cfg.rng_seed = 7;
    cfg.start_zone = "proving_flats";
    cfg.max_violations = 1000000;  // keep one session alive as long as possible
    auth = std::make_unique<auth::AuthService>(db, tickets, auth::PasswordParams::minimum());
    server = std::make_unique<game::GameServer>(cfg, content, transport, db, tickets, 7779, "fuzz-cert");
  }

  void tick() {
    now += 0.05;
    server->set_time(now);
    server->tick();
    if (transport.sent.size() > 2048) transport.sent.clear();
  }

  template <class M>
  void send(const M& m) {
    server->on_packet(peer, M::kChannel, proto::encode_packet(m));
  }

  // A fresh account and character, standing in the test zone.
  void enter() {
    peer = next_peer++;
    transport.sent.clear();
    server->on_connect(peer, "127.0.0.1:9");
    send(proto::Hello{proto::kProtocolVersion, "fuzz"});
    proto::HelloResult hr;
    if (!transport.last(peer, hr)) return;
    const std::string user = "fuzzer" + std::to_string(++accounts);
    const auto out = auth->authenticate({user, "fuzzpassword", true, hr.session_nonce}, std::chrono::seconds(5));
    if (!out.ok) return;
    send(proto::TicketLogin{out.ticket});
    tick();
    send(proto::CharCreate{"Fuzzy" + std::string(1, static_cast<char>('a' + accounts % 26)) + std::string(1, static_cast<char>('a' + accounts / 26 % 26)),
                           "highlander", "warrior", "emberhold", 0});
    tick();
    proto::CharCreateResult cr;
    if (!transport.last(peer, cr) || !cr.ok) return;
    char_id = cr.character.id;
    send(proto::EnterWorld{char_id});
    tick();
  }
};

World& world() {
  static World w;
  return w;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  World& w = world();
  if (w.peer == 0 || w.transport.closing.contains(w.peer)) {
    if (w.peer != 0) w.server->on_disconnect(w.peer);
    w.enter();
  } else if (w.server->players_in_world() == 0) {
    w.send(proto::EnterWorld{w.char_id});  // logged out (or died and camped): back in, same character
    w.tick();
  }
  std::size_t at = 0;
  int sent = 0;
  while (at + 3 <= size) {
    const std::uint8_t channel = data[at] % 3;
    const std::size_t len = static_cast<std::size_t>(data[at + 1]) | (static_cast<std::size_t>(data[at + 2]) << 8);
    at += 3;
    const std::size_t n = std::min(len, size - at);
    w.server->on_packet(w.peer, channel, std::span<const std::uint8_t>(data + at, n));
    at += n;
    if (++sent % 4 == 0) w.tick();
  }
  w.tick();
  return 0;
}
