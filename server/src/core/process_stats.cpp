#include "core/process_stats.hpp"

#include <fstream>

#include <unistd.h>

namespace ef {

std::uint64_t process_rss_kib() {
  std::ifstream in("/proc/self/statm");
  std::uint64_t size = 0;
  std::uint64_t resident = 0;
  if (!(in >> size >> resident)) return 0;
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) return 0;
  return resident * static_cast<std::uint64_t>(page) / 1024u;
}

}  // namespace ef
