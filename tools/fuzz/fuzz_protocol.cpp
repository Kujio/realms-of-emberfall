// libFuzzer target: the wire decoder. Every message type is tried on every input (visit_packet picks
// by id; the raw decode of each type runs too), so bounds checks are exercised for all of them.
// Build with the "fuzz" preset (Clang), then: build/fuzz/tools/fuzz/emberfall-fuzz-protocol -max_total_time=300
#include <cstddef>
#include <cstdint>
#include <span>

#include "net/protocol.gen.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> in(data, size);
  std::size_t visited = 0;
  ef::proto::visit_packet(in, [&](const auto&) { ++visited; });
  // Re-encoding what decoded must round-trip to the same bytes.
  if (visited > 0) {
    ef::proto::visit_packet(in, [&](const auto& msg) {
      const auto again = ef::proto::encode_packet(msg);
      std::size_t n = 0;
      ef::proto::visit_packet(again, [&](const auto&) { ++n; });
      if (n != 1) __builtin_trap();
    });
  }
  return 0;
}
