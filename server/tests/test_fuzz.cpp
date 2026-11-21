// Randomized parser fuzzing. Run under the asan preset to catch out-of-bounds reads.
#include <catch2/catch_test_macros.hpp>

#include <random>

#include "net/protocol.gen.hpp"

using namespace ef;

namespace {

std::vector<std::vector<std::uint8_t>> seed_corpus() {
  std::vector<std::vector<std::uint8_t>> c;
  c.push_back(proto::encode_packet(proto::Hello{1, "client"}));
  c.push_back(proto::encode_packet(proto::TlsLogin{"user", "password123", true, "abcd"}));
  c.push_back(proto::encode_packet(proto::TicketLogin{"0123456789abcdef"}));
  c.push_back(proto::encode_packet(proto::HelloResult{true, 2, "hi", 7779, "-----BEGIN CERTIFICATE-----", "nonce"}));
  c.push_back(proto::encode_packet(proto::CharCreate{"Aldric", "highlander", "warrior", "emberhold", 0}));
  c.push_back(proto::encode_packet(proto::MoveInput{5, 0.5f, 0.5f, 1.0f, 1.0f, 0.0f, 2.0f}));
  c.push_back(proto::encode_packet(proto::ChatSay{"hello there"}));
  proto::CharList list;
  list.characters.push_back({1, "Aldric", "highlander", "warrior", "emberhold", 1, "proving_flats"});
  c.push_back(proto::encode_packet(list));
  proto::CreateOptions opts;
  opts.races.push_back({"a", "A", "d", {"x", "y"}, {"z"}});
  opts.classes.push_back({"x", "X", "d"});
  c.push_back(proto::encode_packet(opts));
  proto::Snapshot s;
  s.entities.push_back({1, 1, 2, 3, 4});
  s.entities.push_back({2, 1, 2, 3, 4});
  c.push_back(proto::encode_packet(s));
  return c;
}

// Decodes with every path; must never crash, read out of bounds, or loop forever.
std::size_t decode_all(std::span<const std::uint8_t> data) {
  std::size_t visited = 0;
  proto::visit_packet(data, [&](const auto&) { ++visited; });
  return visited;
}

}  // namespace

TEST_CASE("random bytes never crash the decoder", "[fuzz]") {
  std::mt19937 rng(12345);
  std::size_t accepted = 0;
  for (int i = 0; i < 200000; ++i) {
    std::vector<std::uint8_t> buf(rng() % 96);
    for (auto& b : buf) b = static_cast<std::uint8_t>(rng());
    // Bias half the inputs toward known message ids so payload parsers get exercised.
    if (buf.size() >= 2 && (i & 1)) {
      static const std::uint16_t ids[] = {1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 40, 41, 42, 43, 44, 50, 51};
      const auto id = ids[rng() % std::size(ids)];
      buf[0] = static_cast<std::uint8_t>(id & 0xFF);
      buf[1] = static_cast<std::uint8_t>(id >> 8);
    }
    accepted += decode_all(buf);
  }
  SUCCEED("accepted " << accepted << " random packets as valid");
}

TEST_CASE("mutated valid packets never crash the decoder", "[fuzz]") {
  std::mt19937 rng(777);
  const auto corpus = seed_corpus();
  for (int i = 0; i < 200000; ++i) {
    auto buf = corpus[rng() % corpus.size()];
    const int mutations = 1 + static_cast<int>(rng() % 4);
    for (int m = 0; m < mutations; ++m) {
      switch (rng() % 5) {
        case 0:  // flip a bit
          if (!buf.empty()) buf[rng() % buf.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
          break;
        case 1:  // truncate
          if (!buf.empty()) buf.resize(rng() % buf.size());
          break;
        case 2:  // extend with junk
          for (unsigned k = rng() % 16; k > 0; --k) buf.push_back(static_cast<std::uint8_t>(rng()));
          break;
        case 3:  // overwrite a length/count prefix with a huge value
          if (buf.size() >= 4) {
            const std::size_t at = 2 + rng() % (buf.size() - 3);
            buf[at] = 0xFF;
            buf[at + 1] = 0xFF;
          }
          break;
        default:  // random byte
          if (!buf.empty()) buf[rng() % buf.size()] = static_cast<std::uint8_t>(rng());
          break;
      }
    }
    decode_all(buf);
  }
  SUCCEED();
}

TEST_CASE("valid corpus still decodes", "[fuzz]") {
  for (const auto& pkt : seed_corpus()) CHECK(decode_all(pkt) == 1);
}
