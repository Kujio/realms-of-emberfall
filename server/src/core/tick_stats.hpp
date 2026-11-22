// Rolling tick-time statistics for the health endpoint and the load test: the mean and maximum of the
// last second, and the 99th percentile of the last `window` ticks. Allocation-free after construction.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ef {

class TickStats {
 public:
  explicit TickStats(std::size_t window) : ring_(std::max<std::size_t>(1, window), 0), scratch_(ring_.size(), 0) {}

  void add(std::uint32_t us) {
    ring_[next_] = us;
    next_ = (next_ + 1) % ring_.size();
    filled_ = std::min(filled_ + 1, ring_.size());
  }

  // The p-th percentile (0 < p <= 100) of what the window holds, 0 when empty. Nearest-rank.
  std::uint32_t percentile(double p) {
    if (filled_ == 0) return 0;
    std::copy_n(ring_.begin(), filled_, scratch_.begin());
    const double clamped = std::clamp(p, 0.0, 100.0);
    auto rank = static_cast<std::size_t>(clamped / 100.0 * static_cast<double>(filled_) + 0.999999);
    rank = std::clamp<std::size_t>(rank, 1, filled_);
    auto nth = scratch_.begin() + static_cast<std::ptrdiff_t>(rank - 1);
    std::nth_element(scratch_.begin(), nth, scratch_.begin() + static_cast<std::ptrdiff_t>(filled_));
    return *nth;
  }

  std::size_t size() const { return filled_; }

 private:
  std::vector<std::uint32_t> ring_;
  std::vector<std::uint32_t> scratch_;
  std::size_t next_ = 0;
  std::size_t filled_ = 0;
};

}  // namespace ef
