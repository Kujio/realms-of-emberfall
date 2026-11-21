// emberfall-bot: headless test client.
//
// Default scenario: connect -> handshake -> register (or log in) -> create character if needed
// -> enter world -> walk in a circle for --duration seconds -> log out -> disconnect.
//
// Other modes:
//   --expect-version-reject   send a wrong protocol version and require a clear rejection
//   --fuzz N                  send N malformed packets across several connections, then verify
//                             the server still completes a normal handshake
//   --fight N                 walk to a level-appropriate camp (from --content), kill N mobs with
//                             auto-attack and abilities, and loot each corpse
//   --die                     pick a fight it cannot win, die, respawn, walk back, and loot its
//                             own corpse (checks the XP penalty and corpse recovery)
//   --caster (with --fight N) a Cleric or Wizard: find the class trainer, buy and scribe the
//                             level-1 scroll it lacks, buff itself, memorize the new spell, then
//                             kill N mobs with spells (standing still while casting). Needs a
//                             fresh level-1 character (a repeat run already knows every scroll).
//
// Exit code 0 = scenario passed.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <charconv>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "game/content.hpp"
#include "game/rules.hpp"
#include "net/protocol.gen.hpp"
#include "net/tls.hpp"

namespace proto = ef::proto;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 7777;
  std::string user = "bot";
  std::string pass = "botpassword";
  std::string name = "Botty";
  std::string race = "highlander";
  std::string class_id = "warrior";
  std::string city = "emberhold";
  int gender = 0;  // 0 = female, 1 = male
  double duration = 5.0;
  double radius = 6.0;
  std::uint16_t protocol_version = proto::kProtocolVersion;
  bool expect_version_reject = false;
  int fuzz = 0;
  int expect_peers = 0;
  bool delete_after = false;
  bool quiet = false;
  int fight = 0;
  bool die = false;
  bool caster = false;
  std::string content = "content";
};

void usage() {
  std::puts(
      "emberfall-bot [--host H] [--port P] [--user U] [--pass P] [--name N] [--race R] [--class C] [--city C] [--gender 0|1]\n"
      "              [--duration SEC] [--radius M] [--expect-peers N] [--delete-after] [--quiet]\n"
      "              [--protocol-version V --expect-version-reject] [--fuzz N]\n"
      "              [--fight N [--caster] | --die] [--content DIR]");
}

template <class T>
bool parse_number(const std::string& s, T& out) {
  const auto* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(s.data(), end, out);
  return ec == std::errc{} && ptr == end && !s.empty();
}

bool parse_args(int argc, char** argv, Options& o) {
  const std::map<std::string, bool*> flags{
      {"--expect-version-reject", &o.expect_version_reject}, {"--delete-after", &o.delete_after}, {"--quiet", &o.quiet},
      {"--die", &o.die}, {"--caster", &o.caster}};
  const std::map<std::string, std::string*> strings{{"--host", &o.host}, {"--user", &o.user}, {"--pass", &o.pass},
                                                    {"--name", &o.name}, {"--race", &o.race}, {"--class", &o.class_id},
                                                    {"--city", &o.city},  {"--content", &o.content}};
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (auto f = flags.find(a); f != flags.end()) {
      *f->second = true;
      continue;
    }
    if (i + 1 >= argc) {
      usage();
      return false;
    }
    const std::string v = argv[++i];
    bool ok = true;
    if (auto s = strings.find(a); s != strings.end()) *s->second = v;
    else if (a == "--port") ok = parse_number(v, o.port);
    else if (a == "--duration") ok = parse_number(v, o.duration);
    else if (a == "--radius") ok = parse_number(v, o.radius);
    else if (a == "--expect-peers") ok = parse_number(v, o.expect_peers);
    else if (a == "--protocol-version") ok = parse_number(v, o.protocol_version);
    else if (a == "--fuzz") ok = parse_number(v, o.fuzz);
    else if (a == "--fight") ok = parse_number(v, o.fight);
    else if (a == "--gender") ok = parse_number(v, o.gender) && o.gender >= 0 && o.gender <= 1;
    else ok = false;
    if (!ok) {
      std::fprintf(stderr, "bad argument: %s %s\n", a.c_str(), v.c_str());
      usage();
      return false;
    }
  }
  if (o.caster && o.fight <= 0) {
    std::fprintf(stderr, "--caster needs --fight N (N > 0)\n");
    usage();
    return false;
  }
  return true;
}

class Connection {
 public:
  ~Connection() { close(); }

  bool open(const std::string& host, std::uint16_t port, int timeout_ms = 5000) {
    host_ = enet_host_create(nullptr, 1, proto::kChannelCount, 0, 0);
    if (!host_) return false;
    ENetAddress addr{};
    if (enet_address_set_host(&addr, host.c_str()) != 0) return false;
    addr.port = port;
    peer_ = enet_host_connect(host_, &addr, proto::kChannelCount, 0);
    if (!peer_) return false;
    ENetEvent ev{};
    if (enet_host_service(host_, &ev, static_cast<enet_uint32>(timeout_ms)) > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) {
      connected_ = true;
      return true;
    }
    enet_peer_reset(peer_);
    peer_ = nullptr;
    return false;
  }

  void close() {
    if (peer_ && connected_) {
      enet_peer_disconnect(peer_, 0);
      ENetEvent ev{};
      const auto deadline = Clock::now() + std::chrono::milliseconds(1000);
      while (Clock::now() < deadline && enet_host_service(host_, &ev, 50) >= 0) {
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        if (ev.type == ENET_EVENT_TYPE_DISCONNECT) break;
      }
    }
    if (peer_) enet_peer_reset(peer_);
    peer_ = nullptr;
    connected_ = false;
    if (host_) enet_host_destroy(host_);
    host_ = nullptr;
  }

  void send_raw(std::uint8_t channel, const std::vector<std::uint8_t>& data, bool reliable) {
    if (!peer_) return;
    ENetPacket* p = enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send(peer_, channel, p) != 0) enet_packet_destroy(p);
  }

  template <class M>
  void send(const M& m) {
    send_raw(M::kChannel, proto::encode_packet(m), M::kChannel == proto::kChannelReliable);
  }

  void flush() {
    if (host_) enet_host_flush(host_);
  }

  // Services the connection; appends received packets. Returns false once disconnected.
  bool pump(int timeout_ms, std::vector<std::vector<std::uint8_t>>& out) {
    if (!host_ || !connected_) return false;
    ENetEvent ev{};
    int rc = enet_host_service(host_, &ev, static_cast<enet_uint32>(timeout_ms));
    while (rc > 0) {
      if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        out.emplace_back(ev.packet->data, ev.packet->data + ev.packet->dataLength);
        enet_packet_destroy(ev.packet);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        connected_ = false;
        return false;
      }
      rc = enet_host_service(host_, &ev, 0);
    }
    return rc >= 0;
  }

  bool connected() const { return connected_; }

 private:
  ENetHost* host_ = nullptr;
  ENetPeer* peer_ = nullptr;
  bool connected_ = false;
};

class Bot {
 public:
  explicit Bot(Options o) : o_(std::move(o)) {}

  __attribute__((format(printf, 2, 3))) void log(const char* fmt, ...) {
    if (o_.quiet) return;
    std::printf("[bot %s] ", o_.user.c_str());
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
  }

  // Waits until a message of type M arrives (handling others in the background).
  template <class M>
  std::optional<M> wait_for(double timeout_s) {
    const auto deadline = Clock::now() + std::chrono::duration<double>(timeout_s);
    while (Clock::now() < deadline) {
      for (std::size_t i = 0; i < inbox_.size(); ++i) {
        M m;
        if (proto::decode_packet(inbox_[i], m)) {
          inbox_.erase(inbox_.begin() + static_cast<std::ptrdiff_t>(i));
          return m;
        }
      }
      std::vector<std::vector<std::uint8_t>> got;
      if (!conn_.pump(20, got)) {
        absorb(got);
        // Disconnected: still give queued messages a chance.
        for (std::size_t i = 0; i < inbox_.size(); ++i) {
          M m;
          if (proto::decode_packet(inbox_[i], m)) return m;
        }
        return std::nullopt;
      }
      absorb(got);
    }
    return std::nullopt;
  }

  void absorb(std::vector<std::vector<std::uint8_t>>& got) {
    for (auto& pkt : got) {
      std::uint16_t id = 0;
      if (!proto::peek_id(pkt, id)) continue;
      if (fighting_ && on_fight_message(id, pkt)) continue;
      if (id == static_cast<std::uint16_t>(proto::MsgId::Snapshot)) {
        proto::Snapshot s;
        if (proto::decode_packet(pkt, s)) on_snapshot(s);
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::EntitySpawn)) {
        proto::EntitySpawn s;
        if (proto::decode_packet(pkt, s)) {
          if (s.kind == 1) mobs_[s.entity_id] = {s.name, s.level, s.x, s.z, s.hp_pct};
          if (s.kind == 2) corpses_[s.entity_id] = {s.name, s.level, s.x, s.z, 0};
          if (s.kind == 3) npcs_[s.entity_id] = {s.name, s.level, s.x, s.z, 100};
          if (s.kind == 0) {  // players; mobs and corpses are tracked separately
            seen_.insert(s.name);
            visible_.insert(s.entity_id);
            log("sees %s (entity %u) at %.1f, %.1f", s.name.c_str(), s.entity_id, static_cast<double>(s.x), static_cast<double>(s.z));
          }
        }
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::EntityDespawn)) {
        proto::EntityDespawn d;
        if (proto::decode_packet(pkt, d)) {
          visible_.erase(d.entity_id);
          mobs_.erase(d.entity_id);
          corpses_.erase(d.entity_id);
          npcs_.erase(d.entity_id);
        }
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::MoveCorrection)) {
        proto::MoveCorrection c;
        if (proto::decode_packet(pkt, c)) {
          ++corrections_;
          log("server correction (%s)", c.reason.c_str());
          pos_x_ = c.x;
          pos_z_ = c.z;
        }
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::ChatMessage)) {
        proto::ChatMessage c;
        if (proto::decode_packet(pkt, c)) {
          if (!fighting_ || c.channel != 2) log("chat [%u] %s: %s", c.channel, c.from_name.c_str(), c.text.c_str());
          if (c.from_name != my_name_ && c.channel == 0) ++chats_heard_;
        }
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::Kick)) {
        proto::Kick k;
        if (proto::decode_packet(pkt, k)) log("kicked: %s", k.reason.c_str());
      }
      inbox_.push_back(std::move(pkt));
    }
  }

  void on_snapshot(const proto::Snapshot& s) {
    ++snapshots_;
    last_ack_ = s.ack_seq;
    server_x_ = s.self_x;
    server_z_ = s.self_z;
    if (have_start_) {
      // Track the farthest the server ever placed us: a full circle ends near the start.
      max_disp_ = std::max(max_disp_, std::hypot(static_cast<double>(server_x_ - start_x_), static_cast<double>(server_z_ - start_z_)));
    }
    max_entities_ = std::max<std::size_t>(max_entities_, s.entities.size());
    for (const auto& e : s.entities) {
      auto it = mobs_.find(e.entity_id);
      if (it == mobs_.end()) continue;
      it->second.x = e.x;
      it->second.z = e.z;
      it->second.hp_pct = e.hp_pct;
    }
  }

  bool handshake() {
    if (!conn_.open(o_.host, o_.port)) {
      log("cannot connect to %s:%u", o_.host.c_str(), o_.port);
      return false;
    }
    conn_.send(proto::Hello{o_.protocol_version, "emberfall-bot"});
    auto hr = wait_for<proto::HelloResult>(5.0);
    if (!hr) {
      log("no HelloResult");
      return false;
    }
    log("handshake: ok=%d server=v%u \"%s\"", hr->ok, hr->server_protocol_version, hr->message.c_str());
    hello_ok_ = hr->ok;
    hello_ = *hr;
    hello_message_ = hr->message;
    return true;
  }

  int run_version_reject() {
    if (!handshake()) return 1;
    if (hello_ok_) {
      log("FAIL: server accepted protocol v%u", o_.protocol_version);
      return 1;
    }
    if (hello_message_.find("update") == std::string::npos && hello_message_.find("matching") == std::string::npos) {
      log("FAIL: rejection message is unclear: %s", hello_message_.c_str());
      return 1;
    }
    std::vector<std::vector<std::uint8_t>> got;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline && conn_.pump(50, got)) {
    }
    if (conn_.connected()) {
      log("FAIL: server did not disconnect after rejecting the version");
      return 1;
    }
    log("PASS: version mismatch rejected and disconnected");
    return 0;
  }

  int run_fuzz() {
    std::mt19937 rng(0xE3BE7u);  // NOLINT(bugprone-random-generator-seed): reproducible fuzz runs
    int sent = 0;
    int connections = 0;
    while (sent < o_.fuzz) {
      Connection c;
      if (!c.open(o_.host, o_.port)) {
        log("FAIL: could not connect for fuzz round (server down?)");
        return 1;
      }
      ++connections;
      // Half the rounds fuzz after a valid handshake so deeper states get exercised.
      if (connections % 2 == 0) c.send(proto::Hello{proto::kProtocolVersion, "fuzz"});
      std::vector<std::vector<std::uint8_t>> got;
      for (int i = 0; i < 150 && sent < o_.fuzz && c.connected(); ++i, ++sent) {
        std::vector<std::uint8_t> pkt;
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0) {
          pkt.resize(rng() % 64);
          for (auto& b : pkt) b = static_cast<std::uint8_t>(rng());
        } else {
          // Valid-looking id with a corrupted/truncated/extended payload.
          static const std::uint16_t ids[] = {1, 10, 11, 20, 23, 25, 27, 29, 42, 50, 2, 43, 999};
          const std::uint16_t id = ids[rng() % (sizeof(ids) / sizeof(ids[0]))];
          pkt = {static_cast<std::uint8_t>(id & 0xFF), static_cast<std::uint8_t>(id >> 8)};
          const std::size_t len = kind == 1 ? rng() % 8 : rng() % 300;
          for (std::size_t k = 0; k < len; ++k) pkt.push_back(static_cast<std::uint8_t>(rng()));
          if (kind == 3 && pkt.size() >= 4) {  // giant string length prefix
            pkt[2] = 0xFF;
            pkt[3] = 0xFF;
          }
        }
        const auto ch = static_cast<std::uint8_t>(rng() % 2);
        c.send_raw(ch, pkt, ch == 0);
        if (i % 10 == 0) c.pump(5, got);
      }
      c.pump(50, got);
    }
    log("fuzz: sent %d malformed packets over %d connections", sent, connections);
    // The server must still be alive and handshake normally.
    if (!handshake() || !hello_ok_) {
      log("FAIL: server did not complete a clean handshake after fuzzing");
      return 1;
    }
    log("PASS: server survived fuzzing");
    return 0;
  }

  std::optional<proto::TlsLoginResult> tls_login(bool is_register) {
    try {
      const auto req = proto::encode_packet(proto::TlsLogin{o_.user, o_.pass, is_register, hello_.session_nonce});
      const auto resp = ef::net::tls_request(o_.host, hello_.login_port, hello_.cert_pem, req, std::chrono::seconds(20));
      proto::TlsLoginResult r;
      if (proto::decode_packet(resp, r)) return r;
      log("TLS login: undecodable response");
    } catch (const ef::net::TlsError& e) {
      log("TLS login error: %s", e.what());
    }
    return std::nullopt;
  }

  // Handshake, TLS login, character create/select, enter world. Sets mine_ and position state.
  std::optional<proto::EnterWorldResult> enter_world() {
    if (!handshake()) return std::nullopt;
    if (!hello_ok_) {
      log("FAIL: handshake rejected: %s", hello_message_.c_str());
      return std::nullopt;
    }

    // The password only travels over TLS, to the certificate the server advertised (the bot
    // trusts it on first use; the real client pins it). The ENet connection gets a ticket.
    const auto fp = ef::net::cert_fingerprint(hello_.cert_pem);
    log("TLS login on port %u, server fingerprint %.23s...", hello_.login_port, fp.c_str());
    auto tls = tls_login(true);
    if (tls && !tls->ok && tls->message.find("taken") != std::string::npos) {
      log("account exists; logging in");
      tls = tls_login(false);
    }
    if (!tls || !tls->ok) {
      log("FAIL: TLS login: %s", tls ? tls->message.c_str() : "no response");
      return std::nullopt;
    }
    conn_.send(proto::TicketLogin{tls->ticket});
    auto ar = wait_for<proto::AuthResult>(10.0);
    if (!ar || !ar->ok) {
      log("FAIL: auth: %s", ar ? ar->message.c_str() : "timeout");
      return std::nullopt;
    }
    log("auth ok: %s", ar->message.c_str());
    auto opts = wait_for<proto::CreateOptions>(5.0);
    auto list = wait_for<proto::CharList>(5.0);
    if (!opts || !list) {
      log("FAIL: missing CreateOptions/CharList");
      return std::nullopt;
    }
    log("create options: %zu races, %zu classes, %zu cities; %zu/%u characters", opts->races.size(), opts->classes.size(),
        opts->cities.size(), list->characters.size(), list->max_characters);

    std::optional<proto::CharSummary> mine;
    for (const auto& c : list->characters)
      if (c.name == o_.name) mine = c;
    if (!mine) {
      conn_.send(proto::CharCreate{o_.name, o_.race, o_.class_id, o_.city, static_cast<std::uint8_t>(o_.gender)});
      auto cr = wait_for<proto::CharCreateResult>(5.0);
      if (!cr || !cr->ok) {
        log("FAIL: create character: %s", cr ? cr->message.c_str() : "timeout");
        return std::nullopt;
      }
      log("created %s (id %llu)", cr->character.name.c_str(), static_cast<unsigned long long>(cr->character.id));
      if (cr->character.gender != o_.gender) {
        log("FAIL: created with gender %d, asked for %d", cr->character.gender, o_.gender);
        return std::nullopt;
      }
      mine = cr->character;
      wait_for<proto::CharList>(5.0);
    }
    my_name_ = mine->name;

    conn_.send(proto::EnterWorld{mine->id});
    auto ew = wait_for<proto::EnterWorldResult>(5.0);
    if (!ew || !ew->ok) {
      log("FAIL: enter world: %s", ew ? ew->message.c_str() : "timeout");
      return std::nullopt;
    }
    log("in world: %s (%s) entity %u at %.1f, %.1f speed %.1f", ew->zone_name.c_str(), ew->zone_id.c_str(), ew->entity_id,
        static_cast<double>(ew->x), static_cast<double>(ew->z), static_cast<double>(ew->run_speed));
    pos_x_ = ew->x;
    pos_z_ = ew->z;
    start_x_ = ew->x;
    start_z_ = ew->z;
    have_start_ = true;
    mine_ = *mine;
    return ew;
  }

  int run_scenario() {
    auto ew = enter_world();
    if (!ew) return 1;
    if (!mine_) return 1;
    const std::uint64_t my_char_id = mine_->id;
    const float min_x = ew->min_x, max_x = ew->max_x, min_z = ew->min_z, max_z = ew->max_z;
    const float dt = 1.0f / static_cast<float>(ew->tick_rate);
    const float speed = ew->run_speed;

    // Walk a circle at 20 Hz using the same step rule as the server (prediction).
    std::uint32_t seq = 0;
    const auto start = Clock::now();
    auto next = start;
    bool said = false;
    while (std::chrono::duration<double>(Clock::now() - start).count() < o_.duration) {
      std::vector<std::vector<std::uint8_t>> got;
      if (!conn_.pump(1, got)) {
        absorb(got);
        log("FAIL: disconnected while in world");
        return 1;
      }
      absorb(got);
      if (Clock::now() < next) continue;
      next += std::chrono::milliseconds(1000 / ew->tick_rate);

      const double t = std::chrono::duration<double>(Clock::now() - start).count();
      const double w = static_cast<double>(speed) / std::max(1.0, o_.radius);  // angular speed for the circle
      const auto mx = static_cast<float>(-std::sin(w * t));
      const auto mz = static_cast<float>(std::cos(w * t));
      pos_x_ = std::clamp(pos_x_ + mx * speed * dt, min_x, max_x);
      pos_z_ = std::clamp(pos_z_ + mz * speed * dt, min_z, max_z);
      ++seq;
      // Godot convention: yaw 0 faces -Z, so facing (mx, mz) is atan2(-mx, -mz).
      conn_.send(proto::MoveInput{seq, mx, mz, std::atan2(-mx, -mz), pos_x_, 0.0f, pos_z_});
      if (!said && t > o_.duration / 2) {
        conn_.send(proto::ChatSay{"Hello from " + my_name_ + "!"});
        said = true;
      }
      conn_.flush();
    }
    // Let the last inputs get acknowledged.
    for (int i = 0; i < 10; ++i) {
      std::vector<std::vector<std::uint8_t>> got;
      conn_.pump(20, got);
      absorb(got);
    }
    const double moved = max_disp_;
    const double drift = std::hypot(static_cast<double>(server_x_ - pos_x_), static_cast<double>(server_z_ - pos_z_));
    log("walked: sent %u inputs, last ack %u, %zu snapshots, %d corrections, server pos %.2f,%.2f (max distance from start %.2f m, prediction drift %.3f m)",
        seq, last_ack_, snapshots_, corrections_, static_cast<double>(server_x_), static_cast<double>(server_z_), moved, drift);
    log("saw %zu other players; heard %d chat lines", seen_.size(), chats_heard_);

    conn_.send(proto::Logout{});
    auto lo = wait_for<proto::LogoutResult>(5.0);
    if (!lo || !lo->ok) {
      log("FAIL: logout");
      return 1;
    }
    if (o_.delete_after) {
      wait_for<proto::CharList>(5.0);
      conn_.send(proto::CharDelete{my_char_id});
      auto dr = wait_for<proto::CharDeleteResult>(5.0);
      log("delete: %s", dr ? dr->message.c_str() : "timeout");
      if (!dr || !dr->ok) {
        log("FAIL: --delete-after did not delete the character");
        conn_.close();
        return 1;
      }
    }
    conn_.close();

    int failures = 0;
    if (snapshots_ < static_cast<std::size_t>(o_.duration * 10)) {
      log("FAIL: too few snapshots");
      ++failures;
    }
    if (last_ack_ + 5 < seq) {
      log("FAIL: server acknowledged only %u of %u inputs", last_ack_, seq);
      ++failures;
    }
    if (moved < 0.5 && o_.duration >= 1.0) {
      log("FAIL: server-side position barely changed");
      ++failures;
    }
    if (drift > 1.0) {
      log("FAIL: prediction drifted %.2f m from the server", drift);
      ++failures;
    }
    if (static_cast<int>(seen_.size()) < o_.expect_peers) {
      log("FAIL: expected to see %d other players, saw %zu", o_.expect_peers, seen_.size());
      ++failures;
    }
    if (failures == 0) log("PASS");
    return failures == 0 ? 0 : 1;
  }

  // ------------------------------------------------------------------ fight mode

  // Handles the combat-era messages; returns true when consumed.
  bool on_fight_message(std::uint16_t id, const std::vector<std::uint8_t>& pkt) {
    using proto::MsgId;
    if (id == static_cast<std::uint16_t>(MsgId::PlayerStats)) {
      proto::PlayerStats s;
      if (proto::decode_packet(pkt, s)) {
        if (s.level > level_ && level_ > 0 && xp_total_seen_ > 0) {
          ++level_ups_;
          log("reached level %u", s.level);
        }
        level_ = s.level;
        xp_into_ = s.xp;
        xp_total_seen_ = 1;
        hp_ = s.hp;
        max_hp_ = s.max_hp;
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::PlayerDied)) {
      proto::PlayerDied d;
      if (proto::decode_packet(pkt, d)) {
        dead_ = true;
        respawn_sent_ = false;
        ++deaths_;
        my_corpse_ = d.corpse_id;
        xp_lost_ = d.xp_lost;
        pre_death_items_ = items_;  // the emptied Inventory arrives after this message
        pre_death_coin_ = coin_;
        // What the rules say this death should cost, from the last stats we saw.
        if (content_ && level_ >= 1) {
          const auto& r = content_->rules;
          const std::uint64_t total = ef::game::total_xp_for_level(level_, r.xp) + xp_into_;
          expected_xp_lost_ = static_cast<std::int64_t>(ef::game::death_xp_loss(total, level_, r.death, r.xp));
        }
        loot_x_ = server_x_;
        loot_z_ = server_z_;
        target_ = 0;
        log("slain by %s (lost %u XP); corpse %u at %.1f, %.1f", d.killer.c_str(), d.xp_lost, d.corpse_id,
            static_cast<double>(loot_x_), static_cast<double>(loot_z_));
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::CombatEvent)) {
      proto::CombatEvent e;
      if (proto::decode_packet(pkt, e)) {
        ++combat_events_;
        if (e.source_id == my_entity_ && e.kind == kSpellHitEvent) {
          ++spell_hits_;
          spell_hit_mobs_.insert(e.target_id);
          if (!bought_name_.empty() && e.ability == bought_name_) ++bought_hits_;  // the spell we trained
        }
        if (e.kind == kDeathEvent && e.target_id == target_ && target_ != 0 && e.source_id == my_entity_) {
          ++kills_;
          if (spell_hit_mobs_.contains(target_)) ++spell_kills_;  // our spells helped bring it down
          const auto it = mobs_.find(target_);
          if (it != mobs_.end()) {
            kill_x_ = it->second.x;
            kill_z_ = it->second.z;
            kill_corpse_name_ = it->second.name + "'s corpse";  // the server names corpses this way
            log("killed %s (%d so far)", it->second.name.c_str(), kills_);
          }
          want_loot_ = true;
          want_loot_since_ = elapsed();
          // The new corpse spawns after this event; older corpses nearby belong to earlier kills.
          corpse_floor_ = corpses_.empty() ? 0 : corpses_.rbegin()->first;
          target_ = 0;
        }
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::LootWindow)) {
      proto::LootWindow w;
      if (proto::decode_packet(pkt, w)) {
        int left = 0;
        for (const auto& it : w.items) left += it.count;
        if (!w.items.empty() && !(w.corpse_id == last_window_corpse_ && left >= last_window_left_)) {
          conn_.send(proto::LootTake{w.corpse_id, 255});
        } else {
          if (!w.items.empty()) log("loot: nothing more fits; %d items left on corpse %u", left, w.corpse_id);
          if (!w.items.empty() && w.corpse_id == my_corpse_) recovery_incomplete_ = true;
          conn_.send(proto::LootClose{w.corpse_id});
        }
        last_window_corpse_ = w.corpse_id;
        last_window_left_ = left;
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::LootClosed)) {
      proto::LootClosed c;
      if (proto::decode_packet(pkt, c) && c.corpse_id == loot_corpse_) {
        if (!c.message.empty()) {
          log("loot: %s", c.message.c_str());
        } else {
          ++loots_;
          if (loot_corpse_ != my_corpse_) ++mob_loots_;
          if (loot_corpse_ == my_corpse_) {
            corpse_recovered_ = true;
            log("recovered my corpse: %llu copper, %d items carried", static_cast<unsigned long long>(coin_), items_);
          }
        }
        loot_corpse_ = 0;
        loot_opened_ = false;
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::Inventory)) {
      proto::Inventory inv;
      if (proto::decode_packet(pkt, inv)) {
        coin_ = inv.coin;
        items_ = 0;
        scroll_slot_ = -1;
        for (const auto& it : inv.items) {
          items_ += it.count;
          if (!bought_item_.empty() && it.item_id == bought_item_) scroll_slot_ = it.slot;  // the scroll we bought
        }
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::SpellBook)) {
      proto::SpellBook b;
      if (proto::decode_packet(pkt, b)) {
        book_.clear();
        for (const auto& sp : b.spells) {
          book_.push_back(sp.id);
          if (sp.id == bought_spell_) bought_name_ = sp.name;
        }
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::GemBar)) {
      proto::GemBar g;
      if (proto::decode_packet(pkt, g)) {
        gems_.clear();
        for (const auto& slot : g.gems) gems_.push_back(slot.spell_id);
        gems_ready_at_ = elapsed() + (g.gems.size() > 1 ? static_cast<double>(g.gems[1].ready_ms) / 1000.0 : 0.0);
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::BuffList)) {
      proto::BuffList l;
      if (proto::decode_packet(pkt, l) && l.entity_id == my_entity_) {
        self_buffs_.clear();
        for (const auto& b : l.buffs) self_buffs_.push_back(b.spell_id);
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::CastStart)) {
      proto::CastStart c;
      if (proto::decode_packet(pkt, c) && c.caster_id == my_entity_) casting_ = true;
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::CastEnd)) {
      proto::CastEnd c;
      if (proto::decode_packet(pkt, c) && c.caster_id == my_entity_) {
        casting_ = false;
        if (c.result == 0) ++casts_done_;
        else if (c.result == 1) ++casts_interrupted_;
      }
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::TrainerWindow)) {
      proto::TrainerWindow w;
      // Only the window we asked for counts: a late reply from a trainer we gave up on is ignored.
      if (proto::decode_packet(pkt, w) && w.npc_id == requested_npc_) trainer_ = w;
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::AbilityResult)) {
      proto::AbilityResult r;
      if (proto::decode_packet(pkt, r) && r.ok) ++abilities_ok_;
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::SkillValues)) {
      proto::SkillValues v;
      if (proto::decode_packet(pkt, v) && v.announce) skill_ups_ += static_cast<int>(v.skills.size());
      return true;
    }
    if (id == static_cast<std::uint16_t>(MsgId::TargetInfo) || id == static_cast<std::uint16_t>(MsgId::AbilityList)) return true;
    if (id == static_cast<std::uint16_t>(MsgId::MoveCorrection)) {
      proto::MoveCorrection c;
      if (proto::decode_packet(pkt, c) && c.reason == "respawn") {
        dead_ = false;
        loot_corpse_ = my_corpse_;
        loot_opened_ = false;
        log("respawned at %.1f, %.1f; walking back to my corpse", static_cast<double>(c.x), static_cast<double>(c.z));
      }
      return false;  // the generic handler moves us
    }
    return false;
  }

  double elapsed() const { return std::chrono::duration<double>(Clock::now() - fight_start_).count(); }

  // Nearest spawn camp whose mob suits this mode (and our level).
  bool pick_camp(const ef::game::ZoneDef& zone, float& cx, float& cz) const {
    float best = 1e30f;
    bool found = false;
    for (const auto& sp : zone.spawns) {
      const auto* m = content_->find_mob(sp.mob);
      // --die picks a mob that never aggroes on its own, so walking back to the corpse is safe.
      if (!m || !suits(m->level) || (dying() && m->aggro_radius > 0.0f)) continue;
      const float d = std::hypot(sp.x - pos_x_, sp.z - pos_z_);
      if (d < best) {
        best = d;
        cx = sp.x;
        cz = sp.z;
        found = true;
      }
    }
    return found;
  }

  bool passive_mob(const std::string& name) const {
    for (const auto& m : content_->mobs)
      if (m.name == name) return m.aggro_radius <= 0.0f;
    return false;
  }

  // --die (optionally after --fight N kills to fill the pack) goes looking for trouble.
  bool dying() const { return o_.die && mob_loots_ >= o_.fight && !want_loot_ && loot_corpse_ == 0; }

  bool suits(int mob_level) const {
    if (dying()) return mob_level >= level_ + 6;
    return mob_level <= level_ && mob_level >= level_ - 3;
  }

  int run_fight() {
    ef::game::Content content;
    try {
      content = ef::game::load_content(o_.content);
    } catch (const std::exception& e) {
      log("FAIL: cannot load content from '%s': %s", o_.content.c_str(), e.what());
      return 1;
    }
    content_ = &content;
    fighting_ = true;
    auto ew = enter_world();
    if (!ew) return 1;
    my_entity_ = ew->entity_id;
    if (o_.caster && mine_ && mine_->class_id != o_.class_id) {
      // An existing character with this name decides the class; combat choices must follow it.
      log("character %s is a %s, not a %s; playing it as a %s", mine_->name.c_str(), mine_->class_id.c_str(), o_.class_id.c_str(),
          mine_->class_id.c_str());
      o_.class_id = mine_->class_id;
    }
    const auto* zone = content.find_zone(ew->zone_id);
    if (!zone) {
      log("FAIL: zone %s is not in the content", ew->zone_id.c_str());
      return 1;
    }
    const float min_x = ew->min_x, max_x = ew->max_x, min_z = ew->min_z, max_z = ew->max_z;
    const float dt = 1.0f / static_cast<float>(ew->tick_rate);
    const float speed = ew->run_speed;
    std::uint32_t seq = 0;
    float yaw = ew->yaw;
    fight_start_ = Clock::now();
    auto next = fight_start_;
    double next_kick = 0.0;
    double next_pick = 0.0;
    double loot_sent_at = 0.0;
    const std::uint32_t start_level = static_cast<std::uint32_t>(mine_->level);
    for (;;) {
      std::vector<std::vector<std::uint8_t>> got;
      if (!conn_.pump(1, got)) {
        absorb(got);
        log("FAIL: disconnected while fighting");
        return 1;
      }
      absorb(got);
      inbox_.clear();  // nothing waits on queued messages in this loop
      if (Clock::now() < next) continue;
      next += std::chrono::milliseconds(1000 / ew->tick_rate);
      const double t = elapsed();
      if (t > o_.duration) break;
      if (o_.die ? corpse_recovered_ : (mob_loots_ >= o_.fight && loot_corpse_ == 0 && !want_loot_)) break;

      bool move = false;
      float gx = pos_x_, gz = pos_z_, stop = 0.0f;
      if (o_.caster && train_ != Train::Done && !dead_) {
        if (!train_step(t, move, gx, gz, stop)) return 1;
      } else if (dead_) {
        if (!respawn_sent_ && items_ > 0) log("died carrying %d items and %llu copper", items_, static_cast<unsigned long long>(coin_));
        if (!respawn_sent_) {
          conn_.send(proto::RespawnRequest{});
          respawn_sent_ = true;
        }
      } else if (want_loot_ && loot_corpse_ == 0) {
        float best = 4.0f;
        for (const auto& [cid, c] : corpses_) {
          const float d = std::hypot(c.x - kill_x_, c.z - kill_z_);
          if (cid > corpse_floor_ && cid != my_corpse_ && c.name == kill_corpse_name_ && d < best) {
            best = d;
            loot_corpse_ = cid;
            loot_x_ = c.x;
            loot_z_ = c.z;
          }
        }
        if (loot_corpse_ != 0) {
          want_loot_ = false;
          loot_opened_ = false;
        }
        if (want_loot_ && t - want_loot_since_ > 3.0) want_loot_ = false;  // never appeared
      } else if (loot_corpse_ != 0) {
        if (auto it = corpses_.find(loot_corpse_); it != corpses_.end()) {
          loot_x_ = it->second.x;
          loot_z_ = it->second.z;
        }
        const float d = std::hypot(loot_x_ - pos_x_, loot_z_ - pos_z_);
        if (d > 3.0f) {
          move = true;
          gx = loot_x_;
          gz = loot_z_;
          stop = 2.0f;
        } else if (!loot_opened_) {
          conn_.send(proto::LootOpen{loot_corpse_});
          loot_opened_ = true;
          loot_sent_at = t;
        } else if (t - loot_sent_at > 5.0) {
          log("loot of corpse %u timed out", loot_corpse_);
          loot_corpse_ = 0;
          loot_opened_ = false;
        }
      } else {
        if (target_ != 0 && !mobs_.contains(target_)) target_ = 0;
        if (target_ == 0 && t >= next_pick) {
          next_pick = t + 0.5;
          float best = 70.0f;
          for (const auto& [mid, m] : mobs_) {
            const float d = std::hypot(m.x - pos_x_, m.z - pos_z_);
            // Untouched mobs only, so two bots never fight over one kill (loot goes to top damage).
            if (suits(m.level) && m.hp_pct >= 100 && (!dying() || passive_mob(m.name)) && d < best) {
              best = d;
              target_ = mid;
            }
          }
          if (target_ != 0) {
            conn_.send(proto::SetTarget{target_});
            if (!o_.caster) conn_.send(proto::AutoAttack{true});
            swinging_ = !o_.caster;
            log("engaging %s (level %u) %.0f m away", mobs_[target_].name.c_str(), mobs_[target_].level, static_cast<double>(best));
          }
        }
        if (target_ != 0 && o_.caster) {
          // Casters fight from range and stand still while a spell is going (moving interrupts it).
          const auto& m = mobs_[target_];
          const float d = std::hypot(m.x - pos_x_, m.z - pos_z_);
          if (d > 40.0f && !casting_) {
            move = true;
            gx = m.x;
            gz = m.z;
            stop = 30.0f;
          } else {
            yaw = std::atan2(-(m.x - pos_x_), -(m.z - pos_z_));
          }
          if (d < 5.0f && !swinging_) {
            conn_.send(proto::AutoAttack{true});
            swinging_ = true;
          }
          if (!casting_ && !move && t >= next_kick) {
            const bool heal = o_.class_id == "cleric" && max_hp_ > 0 && hp_ * 2 < max_hp_;
            // Gem 1 holds the spell we bought; gem 0 the starter (a nuke, or the cleric's heal).
            const std::uint8_t gem = heal ? 0 : (o_.class_id == "wizard" && (static_cast<int>(t * 2) % 3 == 0) ? 0 : 1);
            conn_.send(proto::CastSpell{gem, target_});
            next_kick = t + 0.6;
          }
        } else if (target_ != 0) {
          const auto& m = mobs_[target_];
          const float d = std::hypot(m.x - pos_x_, m.z - pos_z_);
          if (d > 2.5f) {
            move = true;
            gx = m.x;
            gz = m.z;
            stop = 2.0f;
          } else {
            yaw = std::atan2(-(m.x - pos_x_), -(m.z - pos_z_));
          }
          if (d <= 4.0f && t >= next_kick) {
            conn_.send(proto::UseAbility{"rising_kick", target_});
            next_kick = t + 6.2;
          }
        } else if (float cx = 0, cz = 0; pick_camp(*zone, cx, cz)) {
          move = true;
          gx = cx;
          gz = cz;
          stop = 6.0f;
        }
      }

      float mx = 0.0f, mz = 0.0f;
      if (move && !dead_) {
        const float dx = gx - pos_x_, dz = gz - pos_z_;
        const float d = std::hypot(dx, dz);
        if (d > stop + 0.05f) {
          // Scale the intent so a partial step lands exactly where the server will put us.
          const float frac = std::min(1.0f, (d - stop) / (speed * dt));
          mx = dx / d * frac;
          mz = dz / d * frac;
          yaw = std::atan2(-dx, -dz);
        }
      }
      pos_x_ = std::clamp(pos_x_ + mx * speed * dt, min_x, max_x);
      pos_z_ = std::clamp(pos_z_ + mz * speed * dt, min_z, max_z);
      conn_.send(proto::MoveInput{++seq, mx, mz, yaw, pos_x_, 0.0f, pos_z_});
      conn_.flush();
    }
    conn_.send(proto::AutoAttack{false});
    conn_.send(proto::Logout{});
    wait_for<proto::LogoutResult>(5.0);
    conn_.close();
    if (o_.caster)
      log("spells: %d casts completed, %d interrupted, %d spell hits (%d with %s); book %zu spells; buffs on me: %zu", casts_done_,
          casts_interrupted_, spell_hits_, bought_hits_, bought_name_.c_str(), book_.size(), self_buffs_.size());
    log("fight: %d kills, %d corpses looted, %d deaths, level %u -> %d (%u XP into level), %llu copper, %d items, "
        "%d combat events, %d abilities used, %d skill-ups, %d corrections, corpse recovered: %s",
        kills_, loots_, deaths_, start_level, level_, xp_into_, static_cast<unsigned long long>(coin_), items_, combat_events_,
        abilities_ok_, skill_ups_, corrections_, corpse_recovered_ ? "yes" : "no");
    int failures = 0;
    auto fail = [&](const char* why) {
      log("FAIL: %s", why);
      ++failures;
    };
    if (combat_events_ == 0) fail("no combat events");
    if (o_.die) {
      if (deaths_ < 1) fail("did not die");
      if (deaths_ >= 1 && expected_xp_lost_ >= 0 && static_cast<std::int64_t>(xp_lost_) != expected_xp_lost_) {
        log("XP penalty: server took %u, rules say %lld", xp_lost_, static_cast<long long>(expected_xp_lost_));
        fail("death XP penalty does not match the rules");
      }
      if (!corpse_recovered_) fail("did not recover the corpse");
      if (o_.fight > 0 && mob_loots_ < o_.fight) fail("not enough kills looted before dying");
      if (items_ < pre_death_items_ || coin_ < pre_death_coin_) {
        log("recovery: had %d items and %llu copper before dying, %d and %llu after", pre_death_items_,
            static_cast<unsigned long long>(pre_death_coin_), items_, static_cast<unsigned long long>(coin_));
        fail("the recovered corpse did not give everything back");
      }
      if (recovery_incomplete_) fail("items were left on the corpse");
      if (o_.fight > 0 && pre_death_items_ + static_cast<int>(pre_death_coin_) == 0) fail("died with an empty pack; nothing to recover");
    } else {
      if (kills_ < o_.fight) fail("not enough kills");
      if (mob_loots_ < o_.fight) fail("did not loot every kill");
      if (abilities_ok_ == 0) fail("never used an ability");
      if (o_.caster) {
        if (train_ != Train::Done) fail("training did not finish");
        if (casts_done_ < o_.fight) fail("too few spells cast");
        if (spell_hits_ == 0) fail("no spell ever hit");
        if (bought_hits_ == 0) fail("the bought spell never hit in combat");
        if (spell_kills_ < kills_) fail("a kill had no spell damage in it");
        if (std::find(book_.begin(), book_.end(), bought_spell_) == book_.end()) fail("the bought spell is not in the spellbook");
        if (std::find(self_buffs_.begin(), self_buffs_.end(), buff_spell_) == self_buffs_.end()) fail("our buff is gone");
      }
      if (deaths_ > 0) fail("died to level-appropriate mobs");
      if (o_.fight >= 6 && level_ups_ < 1 && start_level < 2) fail("no level gained");
    }
    if (failures == 0) log("PASS");
    return failures == 0 ? 0 : 1;
  }

  // Training (--caster): trainer -> buy -> scribe -> buff -> memorize -> fight.
  enum class Train { FindNpc, AwaitWindow, Scribe, AwaitScribe, Buff, Memorize, Done };

  void train_goto(Train next, double t, const char* what) {
    train_ = next;
    train_since_ = t;
    log("training: %s", what);
  }

  // One step of the training phase; may ask to move. Returns false on a fatal problem.
  bool train_step(double t, bool& move, float& gx, float& gz, float& stop) {
    const double since = t - train_since_;
    switch (train_) {
      case Train::FindNpc: {
        if (book_.empty() || gems_.size() < 2) return true;  // wait for the spellbook
        std::uint32_t best_id = 0;
        float best = 90.0f;
        for (const auto& [nid, n] : npcs_) {
          const float d = std::hypot(n.x - pos_x_, n.z - pos_z_);
          if (!tried_npcs_.contains(nid) && d < best) {
            best = d;
            best_id = nid;
          }
        }
        if (best_id == 0) {
          if (since > 5.0) {
            log("FAIL: no trainer teaches a %s here", o_.class_id.c_str());
            return false;
          }
          return true;
        }
        const auto& n = npcs_[best_id];
        if (best > 4.0f) {
          move = true;
          gx = n.x;
          gz = n.z;
          stop = 3.0f;
          return true;
        }
        tried_npcs_.insert(best_id);
        trainer_.reset();
        requested_npc_ = best_id;
        conn_.send(proto::SetTarget{best_id});
        conn_.send(proto::TrainerOpen{best_id});
        train_goto(Train::AwaitWindow, t, ("asking " + n.name).c_str());
        return true;
      }
      case Train::AwaitWindow:
        if (trainer_) {
          bool unknown = false;
          for (const auto& e : trainer_->entries) {
            if (e.known || e.level > level_) continue;
            unknown = true;
            if (e.price > coin_) continue;
            bought_spell_ = e.spell_id;
            bought_item_ = e.item_id;
            scroll_slot_ = -1;
            conn_.send(proto::TrainerBuy{trainer_->npc_id, e.item_id});
            log("training: %s sells %zu scrolls; buying %s for %u copper", trainer_->name.c_str(), trainer_->entries.size(), e.name.c_str(), e.price);
            train_goto(Train::Scribe, t, "waiting for the scroll");
            return true;
          }
          if (!unknown)
            log("FAIL: %s has no scroll at our level that we do not know (this scenario needs a fresh level-1 character)",
                trainer_->name.c_str());
          else
            log("FAIL: %s has scrolls to learn, but we cannot afford them (%llu copper)", trainer_->name.c_str(),
                static_cast<unsigned long long>(coin_));
          return false;
        }
        if (since > 2.0) train_goto(Train::FindNpc, t, "not our trainer; trying the next one");
        return true;
      case Train::Scribe:
        if (scroll_slot_ >= 0) {
          conn_.send(proto::ScribeSpell{static_cast<std::uint8_t>(scroll_slot_)});
          train_goto(Train::AwaitScribe, t, "scribing");
        } else if (since > 3.0) {
          log("FAIL: the scroll never arrived");
          return false;
        }
        return true;
      case Train::AwaitScribe:
        if (std::find(book_.begin(), book_.end(), bought_spell_) != book_.end()) {
          buff_spell_ = gems_.size() > 1 ? gems_[1] : "";
          conn_.send(proto::SetTarget{my_entity_});
          train_goto(Train::Buff, t, ("scribed " + bought_spell_ + "; casting " + buff_spell_ + " on myself").c_str());
        } else if (since > 3.0) {
          log("FAIL: scribing did not add %s to the spellbook", bought_spell_.c_str());
          return false;
        }
        return true;
      case Train::Buff:
        if (std::find(self_buffs_.begin(), self_buffs_.end(), buff_spell_) != self_buffs_.end()) {
          conn_.send(proto::MemorizeSpell{1, bought_spell_});
          train_goto(Train::Memorize, t, ("memorizing " + bought_spell_ + " in gem 2").c_str());
        } else if (!casting_ && t >= next_train_cast_) {
          conn_.send(proto::CastSpell{1, my_entity_});
          next_train_cast_ = t + 1.0;
        } else if (since > 15.0) {
          log("FAIL: the buff never landed");
          return false;
        }
        return true;
      case Train::Memorize:
        if (gems_.size() > 1 && gems_[1] == bought_spell_ && t >= gems_ready_at_) {
          train_goto(Train::Done, t, "done; off to fight");
        } else if (since > 6.0) {
          log("FAIL: memorizing %s did not take", bought_spell_.c_str());
          return false;
        }
        return true;
      case Train::Done:
        return true;
    }
    return true;
  }

  static constexpr std::uint8_t kDeathEvent = 15;  // ef::game::CombatKind::Death
  static constexpr std::uint8_t kSpellHitEvent = 17;  // ef::game::CombatKind::SpellHit
  Train train_ = Train::FindNpc;
  double train_since_ = 0.0;
  double next_train_cast_ = 0.0;
  std::set<std::uint32_t> tried_npcs_;
  std::optional<proto::TrainerWindow> trainer_;
  std::string bought_spell_;
  std::string bought_item_;
  std::string buff_spell_;
  std::vector<std::string> book_;
  std::vector<std::string> gems_;
  std::vector<std::string> self_buffs_;
  double gems_ready_at_ = 0.0;
  int scroll_slot_ = -1;
  bool casting_ = false;
  bool swinging_ = false;
  int casts_done_ = 0;
  int casts_interrupted_ = 0;
  int spell_hits_ = 0;
  int bought_hits_ = 0;
  int spell_kills_ = 0;
  std::set<std::uint32_t> spell_hit_mobs_;
  std::uint32_t requested_npc_ = 0;
  std::string bought_name_;
  Clock::time_point fight_start_;
  double want_loot_since_ = 0.0;
  std::uint32_t corpse_floor_ = 0;
  std::string kill_corpse_name_;
  std::uint32_t last_window_corpse_ = 0;
  int last_window_left_ = 0;
  int pre_death_items_ = 0;
  std::uint64_t pre_death_coin_ = 0;
  bool recovery_incomplete_ = false;
  float loot_x_ = 0.0f, loot_z_ = 0.0f;

 private:
  Options o_;
  Connection conn_;
  std::vector<std::vector<std::uint8_t>> inbox_;
  bool hello_ok_ = false;
  std::string hello_message_;
  proto::HelloResult hello_;
  std::string my_name_;
  std::set<std::string> seen_;
  std::set<std::uint32_t> visible_;
  std::size_t snapshots_ = 0;
  std::size_t max_entities_ = 0;
  std::uint32_t last_ack_ = 0;
  int corrections_ = 0;
  int chats_heard_ = 0;
  float pos_x_ = 0.0f, pos_z_ = 0.0f;
  float server_x_ = 0.0f, server_z_ = 0.0f;
  float start_x_ = 0.0f, start_z_ = 0.0f;
  bool have_start_ = false;
  double max_disp_ = 0.0;
  std::optional<proto::CharSummary> mine_;
  // Fight mode state
  struct Seen {
    std::string name;
    std::uint8_t level = 0;
    float x = 0.0f;
    float z = 0.0f;
    std::uint8_t hp_pct = 0;
  };
  std::map<std::uint32_t, Seen> mobs_;
  std::map<std::uint32_t, Seen> corpses_;
  std::map<std::uint32_t, Seen> npcs_;
  bool fighting_ = false;
  std::uint32_t my_entity_ = 0;
  std::uint32_t target_ = 0;
  std::uint32_t loot_corpse_ = 0;   // corpse we are walking to / looting
  bool loot_opened_ = false;
  bool want_loot_ = false;          // a kill happened; loot the corpse that appears there
  float kill_x_ = 0.0f, kill_z_ = 0.0f;
  std::uint32_t my_corpse_ = 0;
  bool dead_ = false;
  bool respawn_sent_ = false;
  bool corpse_recovered_ = false;
  int kills_ = 0;
  int loots_ = 0;
  int mob_loots_ = 0;
  int deaths_ = 0;
  int level_ = 1;
  std::uint64_t xp_total_seen_ = 0;
  std::uint32_t xp_into_ = 0;
  int hp_ = 0, max_hp_ = 0;
  std::uint64_t coin_ = 0;
  int items_ = 0;
  int combat_events_ = 0;
  int abilities_ok_ = 0;
  int level_ups_ = 0;
  int skill_ups_ = 0;
  std::uint32_t xp_lost_ = 0;
  std::int64_t expected_xp_lost_ = -1;
  const ef::game::Content* content_ = nullptr;
};

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!parse_args(argc, argv, o)) return 2;
  if (enet_initialize() != 0) {
    std::fprintf(stderr, "enet_initialize failed\n");
    return 1;
  }
  int rc = 0;
  {
    Bot bot(o);
    if (o.expect_version_reject) rc = bot.run_version_reject();
    else if (o.fuzz > 0) rc = bot.run_fuzz();
    else if (o.fight > 0 || o.die) rc = bot.run_fight();
    else rc = bot.run_scenario();
  }
  enet_deinitialize();
  return rc;
}
