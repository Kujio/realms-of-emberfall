#pragma once

#include "core/config.hpp"

namespace ef {

// Console + rotating file logger (log_dir/emberfall-server.log, 10 MiB x 5). Empty log_dir = console only.
void init_logging(const Config& cfg);

}  // namespace ef
