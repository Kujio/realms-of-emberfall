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
  // Login attempts per IP: a sustained rate and a burst (a load test from one host raises both).
  std::uint32_t login_attempts_per_minute = 12;
  std::uint32_t login_attempt_burst = 10;

  // Health/admin HTTP endpoint (0 disables).
  std::string health_bind = "0.0.0.0";
  std::uint16_t health_port = 7778;

  // Storage / content
  std::string db_path = "data/emberfall.db";
  std::string content_path = "content";
  std::uint32_t autosave_seconds = 60;
  // Startup integrity check of the database: "quick" (default), "full" or "off".
  std::string db_check = "quick";

  // Simulation
  std::uint32_t tick_rate = 20;
  std::uint64_t rng_seed = 0;  // 0 = seed from std::random_device; tests pin it for determinism
  // Zones (Phase 6): an empty, non-persistent zone is unloaded after this long (0 = never).
  std::uint32_t zone_idle_seconds = 300;
  // Test/dev: new characters start (and are bound) in this zone instead of their city ("" = off).
  // A comma-separated list spreads them over several zones (load tests), chosen by character name.
  std::string start_zone;
  // Phase 8: a character whose connection drops while in combat stays in the world this long, so
  // pulling the plug cannot save it from a fight (0 = leave at once). Logging back in reclaims it.
  std::uint32_t linger_seconds = 30;

  // Shown on the login screen (with the number of players online) before anyone logs in.
  std::string motd = "Welcome to Realms of Emberfall.";

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
  // Phase 8: a connection that has not logged in this long after connecting is dropped, so idle
  // half-open sessions cannot hold every slot.
  std::uint32_t login_timeout_seconds = 120;  // long enough to read a first-time "trust this server?" prompt
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
