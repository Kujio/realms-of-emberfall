#include <catch2/catch_test_macros.hpp>

#include "net/rate_limiter.hpp"

using namespace ef::net;

TEST_CASE("token bucket allows burst then the sustained rate", "[ratelimit]") {
  TokenBucket b(10.0, 5.0);
  int ok = 0;
  for (int i = 0; i < 20; ++i) ok += b.consume(1.0, 0.0) ? 1 : 0;
  CHECK(ok == 5);
  CHECK_FALSE(b.consume(1.0, 0.05));
  CHECK(b.consume(1.0, 0.2));  // 0.2s * 10/s = 2 tokens refilled
}

TEST_CASE("peer limiter enforces packet count and bytes", "[ratelimit]") {
  PeerRateLimiter lim(60.0, 120.0, 1000.0, 2000.0);
  int ok = 0;
  for (int i = 0; i < 500; ++i) ok += lim.allow(10, 0.0) ? 1 : 0;
  CHECK(ok == 120);

  PeerRateLimiter bytes(1000.0, 1000.0, 100.0, 500.0);
  CHECK(bytes.allow(400, 0.0));
  CHECK_FALSE(bytes.allow(400, 0.0));  // only 100 bytes left
  CHECK(bytes.allow(100, 0.0));
  CHECK(bytes.allow(300, 3.0));  // refilled
}

TEST_CASE("a rejected packet consumes nothing", "[ratelimit]") {
  PeerRateLimiter lim(10.0, 2.0, 1000.0, 100.0);
  CHECK_FALSE(lim.allow(200, 0.0));  // too many bytes: packet bucket untouched
  CHECK(lim.allow(10, 0.0));
  CHECK(lim.allow(10, 0.0));
  CHECK_FALSE(lim.allow(10, 0.0));
}
