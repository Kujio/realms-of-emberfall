// emberfall-bot: headless test client.
//
// Default scenario: connect -> handshake -> register (or log in) -> create character if needed
// -> enter world -> walk in a circle for --duration seconds -> log out -> disconnect.
//
// Other modes:
//   --expect-version-reject   send a wrong protocol version and require a clear rejection
//   --fuzz N                  send N malformed packets across several connections, then verify
//                             the server still completes a normal handshake
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
  double duration = 5.0;
  double radius = 6.0;
  std::uint16_t protocol_version = proto::kProtocolVersion;
  bool expect_version_reject = false;
  int fuzz = 0;
  int expect_peers = 0;
  bool delete_after = false;
  bool quiet = false;
};

void usage() {
  std::puts(
      "emberfall-bot [--host H] [--port P] [--user U] [--pass P] [--name N] [--race R] [--class C] [--city C]\n"
      "              [--duration SEC] [--radius M] [--expect-peers N] [--delete-after] [--quiet]\n"
      "              [--protocol-version V --expect-version-reject] [--fuzz N]");
}

template <class T>
bool parse_number(const std::string& s, T& out) {
  const auto* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(s.data(), end, out);
  return ec == std::errc{} && ptr == end && !s.empty();
}

bool parse_args(int argc, char** argv, Options& o) {
  const std::map<std::string, bool*> flags{
      {"--expect-version-reject", &o.expect_version_reject}, {"--delete-after", &o.delete_after}, {"--quiet", &o.quiet}};
  const std::map<std::string, std::string*> strings{{"--host", &o.host}, {"--user", &o.user}, {"--pass", &o.pass},
                                                    {"--name", &o.name}, {"--race", &o.race}, {"--class", &o.class_id},
                                                    {"--city", &o.city}};
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
    else ok = false;
    if (!ok) {
      std::fprintf(stderr, "bad argument: %s %s\n", a.c_str(), v.c_str());
      usage();
      return false;
    }
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
      if (id == static_cast<std::uint16_t>(proto::MsgId::Snapshot)) {
        proto::Snapshot s;
        if (proto::decode_packet(pkt, s)) on_snapshot(s);
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::EntitySpawn)) {
        proto::EntitySpawn s;
        if (proto::decode_packet(pkt, s)) {
          seen_.insert(s.name);
          visible_.insert(s.entity_id);
          log("sees %s (entity %u) at %.1f, %.1f", s.name.c_str(), s.entity_id, static_cast<double>(s.x), static_cast<double>(s.z));
        }
        continue;
      }
      if (id == static_cast<std::uint16_t>(proto::MsgId::EntityDespawn)) {
        proto::EntityDespawn d;
        if (proto::decode_packet(pkt, d)) visible_.erase(d.entity_id);
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
          log("chat [%u] %s: %s", c.channel, c.from_name.c_str(), c.text.c_str());
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

  int run_scenario() {
    if (!handshake()) return 1;
    if (!hello_ok_) {
      log("FAIL: handshake rejected: %s", hello_message_.c_str());
      return 1;
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
      return 1;
    }
    conn_.send(proto::TicketLogin{tls->ticket});
    auto ar = wait_for<proto::AuthResult>(10.0);
    if (!ar || !ar->ok) {
      log("FAIL: auth: %s", ar ? ar->message.c_str() : "timeout");
      return 1;
    }
    log("auth ok: %s", ar->message.c_str());
    auto opts = wait_for<proto::CreateOptions>(5.0);
    auto list = wait_for<proto::CharList>(5.0);
    if (!opts || !list) {
      log("FAIL: missing CreateOptions/CharList");
      return 1;
    }
    log("create options: %zu races, %zu classes, %zu cities; %zu/%u characters", opts->races.size(), opts->classes.size(),
        opts->cities.size(), list->characters.size(), list->max_characters);

    std::optional<proto::CharSummary> mine;
    for (const auto& c : list->characters)
      if (c.name == o_.name) mine = c;
    if (!mine) {
      conn_.send(proto::CharCreate{o_.name, o_.race, o_.class_id, o_.city});
      auto cr = wait_for<proto::CharCreateResult>(5.0);
      if (!cr || !cr->ok) {
        log("FAIL: create character: %s", cr ? cr->message.c_str() : "timeout");
        return 1;
      }
      log("created %s (id %llu)", cr->character.name.c_str(), static_cast<unsigned long long>(cr->character.id));
      mine = cr->character;
      wait_for<proto::CharList>(5.0);
    }
    my_name_ = mine->name;

    conn_.send(proto::EnterWorld{mine->id});
    auto ew = wait_for<proto::EnterWorldResult>(5.0);
    if (!ew || !ew->ok) {
      log("FAIL: enter world: %s", ew ? ew->message.c_str() : "timeout");
      return 1;
    }
    log("in world: %s (%s) entity %u at %.1f, %.1f speed %.1f", ew->zone_name.c_str(), ew->zone_id.c_str(), ew->entity_id,
        static_cast<double>(ew->x), static_cast<double>(ew->z), static_cast<double>(ew->run_speed));
    pos_x_ = ew->x;
    pos_z_ = ew->z;
    start_x_ = ew->x;
    start_z_ = ew->z;
    have_start_ = true;
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
      conn_.send(proto::CharDelete{mine->id});
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
    else rc = bot.run_scenario();
  }
  enet_deinitialize();
  return rc;
}
