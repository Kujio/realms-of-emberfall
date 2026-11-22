#include "core/logging.hpp"

#include <filesystem>
#include <memory>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace ef {

void init_logging(const Config& cfg) {
  std::vector<spdlog::sink_ptr> sinks;
  sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  if (!cfg.log_dir.empty()) {
    std::filesystem::create_directories(cfg.log_dir);
    const auto file = (std::filesystem::path(cfg.log_dir) / "emberfall-server.log").string();
    constexpr std::size_t kMaxSize = std::size_t{10} * 1024 * 1024;
    constexpr std::size_t kMaxFiles = 5;
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, kMaxSize, kMaxFiles));
  }
  auto logger = std::make_shared<spdlog::logger>("emberfall", sinks.begin(), sinks.end());
  logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
  logger->set_level(spdlog::level::from_str(cfg.log_level));
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(logger);
  // Game master actions (Phase 7) also go to their own file, kept longer than the main log.
  if (!cfg.log_dir.empty() && !spdlog::get("audit")) {
    const auto file = (std::filesystem::path(cfg.log_dir) / "audit.log").string();
    std::vector<spdlog::sink_ptr> audit_sinks = sinks;
    audit_sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(file, std::size_t{10} * 1024 * 1024, 20));
    auto audit = std::make_shared<spdlog::logger>("audit", audit_sinks.begin(), audit_sinks.end());
    audit->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
    audit->flush_on(spdlog::level::info);
    spdlog::register_logger(audit);
  }
  spdlog::flush_every(std::chrono::seconds(2));
}

}  // namespace ef
