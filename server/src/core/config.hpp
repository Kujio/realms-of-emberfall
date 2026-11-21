#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace ef {

struct Config {
  // Network
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 7777;
  std::uint32_t max_players = 200;
  std::uint32_t max_packet_size = 4096;
  std::uint32_t max_connections_per_ip = 8;

  // TLS login service (TCP). Passwords only ever travel over this.
  std::string login_bind = "0.0.0.0";
  std::uint16_t login_port = 7779;
  std::string tls_cert = "data/tls/server.crt";  // generated on first start if missing
  std::string tls_key = "data/tls/server.key";

  // Health/admin HTTP endpoint (0 disables).
  std::string health_bind = "0.0.0.0";
  std::uint16_t health_port = 7778;

  // Storage / content
  std::string db_path = "data/emberfall.db";
  std::string content_path = "content";
  std::uint32_t autosave_seconds = 60;

  // Simulation
  std::uint32_t tick_rate = 20;
  std::uint64_t rng_seed = 0;  // 0 = seed from std::random_device; tests pin it for determinism

  // Logging
  std::string log_dir = "logs";
  std::string log_level = "info";

  // Admin (reserved for GM tooling; never logged).
  std::string admin_password;

  // Per-client abuse limits
  double packets_per_second = 60.0;
  double packet_burst = 120.0;
  double bytes_per_second = 32768.0;
  double byte_burst = 65536.0;
  std::uint32_t max_violations = 10;
  std::uint32_t max_failed_logins = 5;
};

class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using EnvLookup = std::function<std::optional<std::string>(const char* name)>;

// Reads real process environment variables.
EnvLookup process_env();

// Loads `path` (JSON; empty path = defaults only), then applies EMBERFALL_* env overrides,
// then validates. Throws ConfigError listing every problem.
Config load_config(const std::string& path, const EnvLookup& env);

// Applies overrides/validation to an already-populated config (exposed for tests).
void apply_env_overrides(Config& cfg, const EnvLookup& env);
void validate_config(const Config& cfg);

}  // namespace ef
