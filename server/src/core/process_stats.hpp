#pragma once

#include <cstdint>

namespace ef {

// Resident set size of this process in KiB (Linux /proc), or 0 if unavailable.
std::uint64_t process_rss_kib();

}  // namespace ef
