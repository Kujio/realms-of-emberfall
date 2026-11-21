#pragma once

#include <algorithm>
#include <cstddef>

namespace ef::net {

// Classic token bucket. Time is in seconds from any monotonic origin.
class TokenBucket {
 public:
  TokenBucket(double rate_per_sec, double burst) : rate_(rate_per_sec), burst_(burst), tokens_(burst) {}

  bool consume(double amount, double now) {
    refill(now);
    if (tokens_ < amount) return false;
    tokens_ -= amount;
    return true;
  }
  double tokens(double now) {
    refill(now);
    return tokens_;
  }

 private:
  void refill(double now) {
    if (!started_) {
      started_ = true;
      last_ = now;
      return;
    }
    if (now > last_) {
      tokens_ = std::min(burst_, tokens_ + (now - last_) * rate_);
      last_ = now;
    }
  }
  double rate_;
  double burst_;
  double tokens_;
  double last_ = 0.0;
  bool started_ = false;
};

// Per-peer packet-count and byte-volume limits.
class PeerRateLimiter {
 public:
  PeerRateLimiter(double packets_per_sec, double packet_burst, double bytes_per_sec, double byte_burst)
      : packets_(packets_per_sec, packet_burst), bytes_(bytes_per_sec, byte_burst) {}

  // Both buckets must have room; a rejected packet consumes nothing.
  bool allow(std::size_t packet_bytes, double now) {
    const double b = static_cast<double>(packet_bytes);
    if (packets_.tokens(now) < 1.0 || bytes_.tokens(now) < b) return false;
    packets_.consume(1.0, now);
    bytes_.consume(b, now);
    return true;
  }

 private:
  TokenBucket packets_;
  TokenBucket bytes_;
};

}  // namespace ef::net
