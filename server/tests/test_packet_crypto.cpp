// Phase 10: sealed game packets (net/packet_crypto.hpp) and the transport that seals them
// (net/secure_transport.hpp), down to a login through the real game server.
#include <catch2/catch_test_macros.hpp>

#include <random>
#include <string>
#include <vector>

#include "net/packet_crypto.hpp"
#include "net/secure_transport.hpp"
#include "server_support.hpp"

using namespace wire;

namespace {

std::string hex(const std::vector<std::uint8_t>& v) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (auto b : v) {
    s += d[b >> 4];
    s += d[b & 15];
  }
  return s;
}

std::string hex(const std::array<std::uint8_t, 32>& v) { return hex(std::vector<std::uint8_t>(v.begin(), v.end())); }

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

net::SessionKey test_key() {
  net::SessionKey k{};
  for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::uint8_t>(i);
  return k;
}

}  // namespace

// The vectors come from the openssl command line alone (`openssl mac ... HMAC` and
// `openssl enc -aes-256-ctr`), not from this code; the client's tests check the same ones.
TEST_CASE("sealed packets match the known-answer vectors", "[crypto]") {
  REQUIRE(auth::init_crypto());
  const auto key = test_key();
  CHECK(net::cipher_label(true, 0) == "emberfall/1 c2s 0");
  CHECK(net::cipher_label(false, 1) == "emberfall/1 s2c 1");
  CHECK(hex(net::derive_key(key, "emberfall/1 c2s 0 enc")) == "0a229cdfd35745c76c8d05bf49a1ce7d13aa36c8316896b0490ff3a7a7168faf");
  CHECK(hex(net::derive_key(key, "emberfall/1 c2s 0 mac")) == "f638384a71d33f2a118fa4c53f24cf652f172fe90236095d1575b59bb52cf872");

  net::PacketCipher c2s(key, "emberfall/1 c2s 0");
  std::vector<std::uint8_t> out;
  for (int i = 0; i < 5; ++i) c2s.seal(bytes("x"), out);  // counters 0..4
  c2s.seal(bytes("Realms of Emberfall: a sealed packet, three AES blocks."), out);
  CHECK(hex(out) ==
        "05000000000000000633fef70c7f895ee140f068cf133c6d74845e0362215c85f7bfa3201360183de2a6804d0e84e8d724a819aef9fdcb86"
        "45e4dc62788fc688ccbed464cfabad93c310c0a0a34544");

  net::SessionCrypto server(key, net::SessionCrypto::Role::Server);
  server.seal(1, {}, out);  // s2c on the unreliable channel, counter 0, nothing inside
  CHECK(hex(out) == "000000000000000038c8bf9e16be6291a6d68fc442997504");
}

TEST_CASE("sealed packets round-trip both ways on both channels", "[crypto]") {
  REQUIRE(auth::init_crypto());
  const auto key = net::random_session_key();
  net::SessionCrypto client(key, net::SessionCrypto::Role::Client);
  net::SessionCrypto server(key, net::SessionCrypto::Role::Server);
  std::vector<std::uint8_t> sealed, plain;
  for (std::uint8_t ch = 0; ch < 2; ++ch) {
    for (int i = 0; i < 50; ++i) {
      const auto msg = bytes("packet " + std::to_string(i) + std::string(static_cast<std::size_t>(i * 7), 'z'));
      client.seal(ch, msg, sealed);
      CHECK(sealed.size() == msg.size() + net::kSealOverhead);
      REQUIRE(server.open(ch, sealed, plain) == net::PacketCipher::Open::Ok);
      CHECK(plain == msg);
      server.seal(ch, msg, sealed);
      REQUIRE(client.open(ch, sealed, plain) == net::PacketCipher::Open::Ok);
      CHECK(plain == msg);
    }
  }
}

TEST_CASE("sealed packets refuse tampering, replays, reflection and the wrong key", "[crypto]") {
  REQUIRE(auth::init_crypto());
  const auto key = net::random_session_key();
  net::SessionCrypto client(key, net::SessionCrypto::Role::Client);
  net::SessionCrypto server(key, net::SessionCrypto::Role::Server);
  std::vector<std::uint8_t> sealed, plain;
  using O = net::PacketCipher::Open;

  client.seal(0, bytes("hello world"), sealed);
  for (std::size_t i = 0; i < sealed.size(); ++i) {  // any flipped bit, counter, body or tag
    auto bad = sealed;
    bad[i] ^= 0x01;
    CHECK(server.open(0, bad, plain) == O::BadTag);
  }
  CHECK(server.open(0, std::span(sealed).first(net::kSealOverhead - 1), plain) == O::Malformed);
  CHECK(server.open(1, sealed, plain) == O::BadTag);  // another channel's keys
  CHECK(client.open(0, sealed, plain) == O::BadTag);  // reflected back at its sender
  net::SessionCrypto stranger(net::random_session_key(), net::SessionCrypto::Role::Server);
  CHECK(stranger.open(0, sealed, plain) == O::BadTag);
  CHECK(server.open(2, sealed, plain) == O::Malformed);  // no such channel

  REQUIRE(server.open(0, sealed, plain) == O::Ok);
  CHECK(server.open(0, sealed, plain) == O::Replay);  // reliable: each counter once

  std::vector<std::uint8_t> second, third;
  client.seal(0, bytes("two"), second);
  client.seal(0, bytes("three"), third);
  CHECK(server.open(0, third, plain) == O::Replay);  // reliable: no gaps (ENet delivers in order)
  CHECK(server.open(0, second, plain) == O::Ok);
  CHECK(server.open(0, third, plain) == O::Ok);

  std::vector<std::uint8_t> u1, u2, u3;
  client.seal(1, bytes("u1"), u1);
  client.seal(1, bytes("u2"), u2);
  client.seal(1, bytes("u3"), u3);
  CHECK(server.open(1, u2, plain) == O::Ok);      // unreliable: a lost packet is fine
  CHECK(server.open(1, u1, plain) == O::Replay);  // but nothing older than the newest
  CHECK(server.open(1, u2, plain) == O::Replay);
  CHECK(server.open(1, u3, plain) == O::Ok);
}

TEST_CASE("random bytes never open as a sealed packet", "[crypto]") {
  REQUIRE(auth::init_crypto());
  net::SessionCrypto server(net::random_session_key(), net::SessionCrypto::Role::Server);
  std::mt19937 rng(7);
  std::vector<std::uint8_t> junk, plain;
  for (int i = 0; i < 5000; ++i) {
    junk.resize(rng() % 200);
    for (auto& b : junk) b = static_cast<std::uint8_t>(rng());
    CHECK(server.open(static_cast<std::uint8_t>(rng() % 3), junk, plain) != net::PacketCipher::Open::Ok);
  }
}

TEST_CASE("session keys travel as 64 hex digits", "[crypto]") {
  REQUIRE(auth::init_crypto());
  const auto key = net::random_session_key();
  net::SessionKey back{};
  REQUIRE(net::parse_session_key_hex(net::session_key_hex(key), back));
  CHECK(back == key);
  CHECK(net::session_key_hex(test_key()) == "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  CHECK(net::parse_session_key_hex("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", back));
  CHECK(back == test_key());
  CHECK_FALSE(net::parse_session_key_hex("", back));
  CHECK_FALSE(net::parse_session_key_hex(std::string(63, 'a'), back));
  CHECK_FALSE(net::parse_session_key_hex(std::string(64, 'g'), back));
  CHECK(net::random_session_key() != net::random_session_key());
}

namespace {

// Answers every packet while still holding it, as the game server does, and keeps what it read
// after answering.
struct Echo final : net::TransportHandler {
  net::Transport* out = nullptr;
  std::vector<std::vector<std::uint8_t>> seen_after_reply;
  void on_connect(net::PeerId, const std::string&) override {}
  void on_disconnect(net::PeerId) override {}
  void on_packet(net::PeerId p, std::uint8_t ch, std::span<const std::uint8_t> d) override {
    const std::vector<std::uint8_t> reply(300, 0xAB);  // longer than the request: a buffer would grow
    out->send(p, ch, reply, ch == 0);
    seen_after_reply.emplace_back(d.begin(), d.end());
  }
};

struct Recorder final : net::TransportHandler {
  std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> packets;
  std::vector<net::PeerId> connects, disconnects;
  void on_connect(net::PeerId p, const std::string&) override { connects.push_back(p); }
  void on_disconnect(net::PeerId p) override { disconnects.push_back(p); }
  void on_packet(net::PeerId, std::uint8_t ch, std::span<const std::uint8_t> d) override { packets.emplace_back(ch, std::vector(d.begin(), d.end())); }
};

}  // namespace

TEST_CASE("secure transport passes the handshake through and seals after login", "[crypto]") {
  REQUIRE(auth::init_crypto());
  FakeTransport link;
  Recorder game;
  net::SecureTransport secure(link);
  secure.set_handler(game);
  secure.on_connect(4, "10.0.0.1:5");
  CHECK(game.connects == std::vector<net::PeerId>{4});

  secure.on_packet(4, 0, bytes("hello"));  // before login: as it is
  REQUIRE(game.packets.size() == 1);
  CHECK(game.packets[0].second == bytes("hello"));
  secure.send(4, 0, bytes("welcome"), true);
  CHECK(link.sent.back().data == bytes("welcome"));

  const auto key = net::random_session_key();
  secure.enable_encryption(4, key);
  CHECK(secure.encrypted(4));
  net::SessionCrypto client(key, net::SessionCrypto::Role::Client);
  std::vector<std::uint8_t> sealed, plain;

  secure.send(4, 1, bytes("snapshot"), false);
  CHECK(link.sent.back().data.size() == 8 + net::kSealOverhead);
  REQUIRE(client.open(1, link.sent.back().data, plain) == net::PacketCipher::Open::Ok);
  CHECK(plain == bytes("snapshot"));

  client.seal(0, bytes("move"), sealed);
  secure.on_packet(4, 0, sealed);
  REQUIRE(game.packets.size() == 2);
  CHECK(game.packets[1].second == bytes("move"));

  // Unreliable failures are dropped until there are too many; one reliable failure closes.
  for (std::uint32_t i = 0; i + 1 < net::SecureTransport::kMaxUnreliableFailures; ++i) secure.on_packet(4, 1, bytes("junk junk junk junk junk junk"));
  CHECK(game.packets.size() == 2);
  CHECK(link.disconnecting.empty());
  secure.on_packet(4, 1, bytes("junk junk junk junk junk junk"));
  CHECK(link.disconnecting.count(4) == 1);
  client.seal(0, bytes("late"), sealed);
  secure.on_packet(4, 0, sealed);  // closing: nothing more is delivered
  CHECK(game.packets.size() == 2);

  secure.on_connect(5, "10.0.0.2:6");
  secure.enable_encryption(5, key);
  secure.on_packet(5, 0, bytes("plaintext after login"));
  CHECK(link.disconnecting.count(5) == 1);
  CHECK(secure.rejected_packets() == net::SecureTransport::kMaxUnreliableFailures + 1);

  secure.on_disconnect(4);
  CHECK_FALSE(secure.encrypted(4));
  CHECK(game.disconnects == std::vector<net::PeerId>{4});
}

TEST_CASE("answering a packet while handling it leaves the packet intact", "[crypto]") {
  REQUIRE(auth::init_crypto());
  FakeTransport link;
  Echo game;
  net::SecureTransport secure(link);
  game.out = &secure;
  secure.set_handler(game);
  secure.on_connect(1, "a");
  const auto key = net::random_session_key();
  secure.enable_encryption(1, key);
  net::SessionCrypto client(key, net::SessionCrypto::Role::Client);
  std::vector<std::uint8_t> sealed, plain;
  client.seal(0, bytes("the request"), sealed);
  secure.on_packet(1, 0, sealed);
  REQUIRE(game.seen_after_reply.size() == 1);
  CHECK(game.seen_after_reply[0] == bytes("the request"));
  REQUIRE(client.open(0, link.sent.back().data, plain) == net::PacketCipher::Open::Ok);
  CHECK(plain == std::vector<std::uint8_t>(300, 0xAB));
}

TEST_CASE("a login through the game server seals everything after the ticket", "[crypto]") {
  REQUIRE(auth::init_crypto());
  Config cfg;
  cfg.rng_seed = 99;
  cfg.start_zone = "proving_flats";
  const Content content = load_content(EF_CONTENT_DIR);
  db::DbWorker db(std::make_unique<db::SqliteRepository>(":memory:"), db::DbWorker::Mode::Inline);
  auth::TicketStore tickets;
  auth::AuthService auth_service(db, tickets, auth::PasswordParams::minimum());
  FakeTransport link;
  net::SecureTransport secure(link);
  GameServer server(cfg, content, secure, db, tickets, 7779, "test-cert");
  secure.set_handler(server);
  double now = 0.0;
  auto tick = [&] {
    now += 0.05;
    server.set_time(now);
    server.tick();
  };

  secure.on_connect(1, "127.0.0.1:1");
  secure.on_packet(1, 0, proto::encode_packet(proto::Hello{proto::kProtocolVersion, "test"}));
  proto::HelloResult hr;
  REQUIRE(proto::decode_packet(link.sent.at(0).data, hr));  // the handshake is readable
  REQUIRE(hr.ok);
  const auto out = auth_service.authenticate({"sealed", "password123", true, hr.session_nonce}, std::chrono::seconds(5));
  REQUIRE(out.ok);
  net::SessionKey key{};
  REQUIRE(net::parse_session_key_hex(out.session_key, key));
  net::SessionCrypto client(key, net::SessionCrypto::Role::Client);

  link.sent.clear();
  secure.on_packet(1, 0, proto::encode_packet(proto::TicketLogin{out.ticket}));
  tick();
  REQUIRE_FALSE(link.sent.empty());
  std::vector<std::uint8_t> plain;
  bool authed = false;
  for (const auto& s : link.sent) {
    proto::AuthResult ar;
    CHECK_FALSE(proto::decode_packet(s.data, ar));  // nothing readable on the link any more
    REQUIRE(client.open(s.channel, s.data, plain) == net::PacketCipher::Open::Ok);
    if (proto::decode_packet(plain, ar)) authed = ar.ok;
  }
  CHECK(authed);

  link.sent.clear();
  std::vector<std::uint8_t> sealed;
  client.seal(0, proto::encode_packet(proto::CharListRequest{}), sealed);
  secure.on_packet(1, 0, sealed);
  tick();
  bool listed = false;
  for (const auto& s : link.sent) {
    REQUIRE(client.open(s.channel, s.data, plain) == net::PacketCipher::Open::Ok);
    proto::CharList cl;
    if (proto::decode_packet(plain, cl)) listed = true;
  }
  CHECK(listed);

  secure.on_packet(1, 0, proto::encode_packet(proto::CharListRequest{}));  // unsealed now: closed
  CHECK(link.disconnecting.count(1) == 1);
}
