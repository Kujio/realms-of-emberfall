#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "net/protocol.gen.hpp"

using namespace ef;

TEST_CASE("primitive round trip is little-endian", "[protocol]") {
  std::vector<std::uint8_t> buf;
  net::Writer w(buf);
  w.u16(0x1234);
  w.u32(0xA1B2C3D4u);
  REQUIRE(buf == std::vector<std::uint8_t>{0x34, 0x12, 0xD4, 0xC3, 0xB2, 0xA1});
}

TEST_CASE("every message type round-trips", "[protocol]") {
  proto::CharList list;
  list.max_characters = 4;
  list.characters.push_back({42, "Aldric", "highlander", "warrior", "emberhold", 7, "proving_flats"});
  list.characters.push_back({43, "Brenna", "deepkin", "cleric", "cragmouth", 1, "proving_flats"});
  auto bytes = proto::encode_packet(list);
  proto::CharList out;
  REQUIRE(proto::decode_packet(bytes, out));
  REQUIRE(out.characters.size() == 2);
  CHECK(out.characters[0].name == "Aldric");
  CHECK(out.characters[1].level == 1);

  proto::Snapshot snap;
  snap.server_tick = 99;
  snap.ack_seq = 7;
  snap.self_x = 1.5f;
  snap.entities.push_back({3, 1.0f, 0.0f, -2.0f, 0.5f});
  bytes = proto::encode_packet(snap);
  proto::Snapshot snap2;
  REQUIRE(proto::decode_packet(bytes, snap2));
  CHECK(snap2.entities.at(0).z == -2.0f);
  CHECK(snap2.ack_seq == 7);

  proto::CreateOptions opts;
  opts.races.push_back({"highlander", "Highlander", "desc", {"warrior", "cleric"}, {"emberhold"}});
  bytes = proto::encode_packet(opts);
  proto::CreateOptions opts2;
  REQUIRE(proto::decode_packet(bytes, opts2));
  CHECK(opts2.races.at(0).classes.at(1) == "cleric");
}

TEST_CASE("decoder rejects every truncation", "[protocol]") {
  proto::EnterWorldResult r;
  r.ok = true;
  r.message = "hi";
  r.zone_id = "proving_flats";
  auto bytes = proto::encode_packet(r);
  for (std::size_t len = 0; len < bytes.size(); ++len) {
    proto::EnterWorldResult out;
    INFO("length " << len);
    CHECK_FALSE(proto::decode_packet(std::span(bytes.data(), len), out));
  }
  proto::EnterWorldResult full;
  CHECK(proto::decode_packet(bytes, full));
}

TEST_CASE("decoder rejects trailing bytes and wrong ids", "[protocol]") {
  auto bytes = proto::encode_packet(proto::Hello{1, "client"});
  bytes.push_back(0);
  proto::Hello h;
  CHECK_FALSE(proto::decode_packet(bytes, h));
  bytes.pop_back();
  proto::TicketLogin l;
  CHECK_FALSE(proto::decode_packet(bytes, l));
}

TEST_CASE("decoder enforces string limits and UTF-8", "[protocol]") {
  // TlsLogin username max is 32 bytes.
  std::vector<std::uint8_t> pkt{10, 0, 33, 0};
  pkt.insert(pkt.end(), 33, 'a');
  pkt.insert(pkt.end(), {0, 0, 0, 0, 0});
  proto::TlsLogin l;
  CHECK_FALSE(proto::decode_packet(pkt, l));

  // Length prefix larger than the packet.
  std::vector<std::uint8_t> lie{10, 0, 0xFF, 0xFF, 'a'};
  CHECK_FALSE(proto::decode_packet(lie, l));

  // Invalid UTF-8 (overlong encoding of '/').
  std::vector<std::uint8_t> bad{50, 0, 2, 0, 0xC0, 0xAF};
  proto::ChatSay c;
  CHECK_FALSE(proto::decode_packet(bad, c));

  // Valid multi-byte UTF-8 is fine.
  auto ok = proto::encode_packet(proto::ChatSay{"h\xC3\xA9llo \xE2\x9C\x93"});
  CHECK(proto::decode_packet(ok, c));
}

TEST_CASE("decoder rejects non-finite floats and bad bools", "[protocol]") {
  proto::MoveInput m{1, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  auto bytes = proto::encode_packet(m);
  // move_x lives at offset 2 (id) + 4 (seq).
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const auto bits = std::bit_cast<std::uint32_t>(nan);
  for (int i = 0; i < 4; ++i) bytes[static_cast<std::size_t>(6 + i)] = static_cast<std::uint8_t>(bits >> (8 * i));
  proto::MoveInput out;
  CHECK_FALSE(proto::decode_packet(bytes, out));

  auto hr = proto::encode_packet(proto::HelloResult{true, 1, "", 7779, "", ""});
  hr[2] = 2;  // bool must be 0 or 1
  proto::HelloResult hro;
  CHECK_FALSE(proto::decode_packet(hr, hro));
}

TEST_CASE("decoder enforces array caps", "[protocol]") {
  // Snapshot with count 257 (> max 256) and no data.
  proto::Snapshot s;
  auto bytes = proto::encode_packet(s);
  bytes[bytes.size() - 2] = 0x01;
  bytes[bytes.size() - 1] = 0x01;
  proto::Snapshot out;
  CHECK_FALSE(proto::decode_packet(bytes, out));
}

TEST_CASE("encoder truncates oversize strings on a UTF-8 boundary", "[protocol]") {
  std::string longtext(300, 'x');
  longtext += "\xE2\x9C\x93";
  auto bytes = proto::encode_packet(proto::ChatSay{longtext});
  proto::ChatSay out;
  REQUIRE(proto::decode_packet(bytes, out));
  CHECK(out.text.size() == 256);

  std::string s(255, 'a');
  s += "\xE2\x9C\x93";  // 3-byte char straddles the 256 limit
  bytes = proto::encode_packet(proto::ChatSay{s});
  REQUIRE(proto::decode_packet(bytes, out));
  CHECK(out.text.size() == 255);
}

TEST_CASE("TLS-only messages are never valid on ENet channels", "[protocol]") {
  proto::Direction d{};
  std::uint8_t ch = 0;
  REQUIRE(proto::msg_info(static_cast<std::uint16_t>(proto::MsgId::TlsLogin), d, ch));
  CHECK(ch == proto::kChannelTls);
  CHECK(ch >= proto::kChannelCount);
}

TEST_CASE("msg_info classifies directions and channels", "[protocol]") {
  proto::Direction d{};
  std::uint8_t ch = 9;
  REQUIRE(proto::msg_info(static_cast<std::uint16_t>(proto::MsgId::MoveInput), d, ch));
  CHECK(d == proto::Direction::ClientToServer);
  CHECK(ch == 1);
  REQUIRE(proto::msg_info(static_cast<std::uint16_t>(proto::MsgId::Snapshot), d, ch));
  CHECK(d == proto::Direction::ServerToClient);
  CHECK_FALSE(proto::msg_info(9999, d, ch));
}
