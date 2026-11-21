#include "core/config.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace ef {

namespace {

template <class T>
bool parse_uint(const std::string& s, T& out) {
  std::uint64_t v = 0;
  const auto* first = s.data();
  const auto* last = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(first, last, v);
  if (ec != std::errc{} || ptr != last || s.empty()) return false;
  if (v > std::numeric_limits<T>::max()) return false;
  out = static_cast<T>(v);
  return true;
}

template <class T>
void read_uint(const nlohmann::json& j, const char* key, T& out, std::vector<std::string>& errors) {
  if (!j.contains(key)) return;
  const auto& v = j.at(key);
  if (!v.is_number_unsigned() || v.get<std::uint64_t>() > std::numeric_limits<T>::max()) {
    errors.push_back(std::string("config key '") + key + "' must be an unsigned integer in range");
    return;
  }
  out = static_cast<T>(v.get<std::uint64_t>());
}

void read_string(const nlohmann::json& j, const char* key, std::string& out, std::vector<std::string>& errors) {
  if (!j.contains(key)) return;
  if (!j.at(key).is_string()) {
    errors.push_back(std::string("config key '") + key + "' must be a string");
    return;
  }
  out = j.at(key).get<std::string>();
}

void read_double(const nlohmann::json& j, const char* key, double& out, std::vector<std::string>& errors) {
  if (!j.contains(key)) return;
  if (!j.at(key).is_number()) {
    errors.push_back(std::string("config key '") + key + "' must be a number");
    return;
  }
  out = j.at(key).get<double>();
}

[[noreturn]] void throw_errors(const std::vector<std::string>& errors) {
  std::ostringstream os;
  os << "invalid configuration:";
  for (const auto& e : errors) os << "\n  - " << e;
  throw ConfigError(os.str());
}

}  // namespace

EnvLookup process_env() {
  return [](const char* name) -> std::optional<std::string> {
    const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read once at startup
    if (v == nullptr) return std::nullopt;
    return std::string(v);
  };
}

void apply_env_overrides(Config& cfg, const EnvLookup& env) {
  std::vector<std::string> errors;
  auto str = [&](const char* name, std::string& out) {
    if (auto v = env(name)) out = *v;
  };
  auto num = [&](const char* name, auto& out) {
    if (auto v = env(name)) {
      if (!parse_uint(*v, out)) errors.push_back(std::string("environment variable ") + name + " must be an unsigned integer in range");
    }
  };
  str("EMBERFALL_BIND", cfg.bind_address);
  num("EMBERFALL_PORT", cfg.port);
  num("EMBERFALL_MAX_PLAYERS", cfg.max_players);
  str("EMBERFALL_LOGIN_BIND", cfg.login_bind);
  num("EMBERFALL_LOGIN_PORT", cfg.login_port);
  str("EMBERFALL_TLS_CERT", cfg.tls_cert);
  str("EMBERFALL_TLS_KEY", cfg.tls_key);
  str("EMBERFALL_HEALTH_BIND", cfg.health_bind);
  num("EMBERFALL_HEALTH_PORT", cfg.health_port);
  str("EMBERFALL_DB_PATH", cfg.db_path);
  str("EMBERFALL_CONTENT_PATH", cfg.content_path);
  num("EMBERFALL_AUTOSAVE_SECONDS", cfg.autosave_seconds);
  num("EMBERFALL_TICK_RATE", cfg.tick_rate);
  str("EMBERFALL_LOG_DIR", cfg.log_dir);
  str("EMBERFALL_LOG_LEVEL", cfg.log_level);
  str("EMBERFALL_ADMIN_PASSWORD", cfg.admin_password);
  if (!errors.empty()) throw_errors(errors);
}

void validate_config(const Config& cfg) {
  std::vector<std::string> errors;
  if (cfg.port == 0) errors.emplace_back("port must be 1..65535");
  if (cfg.login_port == 0) errors.emplace_back("login_port must be 1..65535");
  if (cfg.tls_cert.empty() || cfg.tls_key.empty()) errors.emplace_back("tls_cert and tls_key must be set");
  if (cfg.max_players == 0 || cfg.max_players > 4000) errors.emplace_back("max_players must be 1..4000");
  if (cfg.tick_rate < 1 || cfg.tick_rate > 128) errors.emplace_back("tick_rate must be 1..128");
  if (cfg.max_packet_size < 256 || cfg.max_packet_size > 65536) errors.emplace_back("max_packet_size must be 256..65536");
  if (cfg.max_connections_per_ip == 0) errors.emplace_back("max_connections_per_ip must be >= 1");
  if (cfg.autosave_seconds < 5) errors.emplace_back("autosave_seconds must be >= 5");
  if (cfg.db_path.empty()) errors.emplace_back("db_path must not be empty");
  if (cfg.content_path.empty()) errors.emplace_back("content_path must not be empty");
  if (cfg.packets_per_second <= 0 || cfg.packet_burst < 1) errors.emplace_back("packet rate limits must be positive");
  if (cfg.bytes_per_second <= 0 || cfg.byte_burst < cfg.max_packet_size) errors.emplace_back("byte_burst must be >= max_packet_size and bytes_per_second > 0");
  if (cfg.max_violations == 0) errors.emplace_back("max_violations must be >= 1");
  static const std::set<std::string> levels{"trace", "debug", "info", "warn", "error", "critical", "off"};
  if (!levels.contains(cfg.log_level)) errors.emplace_back("log_level must be one of trace/debug/info/warn/error/critical/off");
  if (!errors.empty()) throw_errors(errors);
}

Config load_config(const std::string& path, const EnvLookup& env) {
  Config cfg;
  if (!path.empty()) {
    std::ifstream in(path);
    if (!in) throw ConfigError("cannot open config file: " + path);
    nlohmann::json j;
    try {
      in >> j;
    } catch (const nlohmann::json::exception& e) {
      throw ConfigError("config file " + path + " is not valid JSON: " + e.what());
    }
    if (!j.is_object()) throw ConfigError("config file " + path + " must contain a JSON object");

    static const std::set<std::string> known{
        "bind_address", "port", "max_players", "max_packet_size", "max_connections_per_ip",
        "login_bind", "login_port", "tls_cert", "tls_key",
        "health_bind", "health_port", "db_path", "content_path", "autosave_seconds", "tick_rate",
        "log_dir", "log_level", "admin_password", "packets_per_second", "packet_burst",
        "bytes_per_second", "byte_burst", "max_violations", "max_failed_logins"};
    std::vector<std::string> errors;
    for (const auto& [key, _] : j.items()) {
      if (!key.starts_with('_') && !known.contains(key)) errors.push_back("unknown config key '" + key + "'");
    }
    read_string(j, "bind_address", cfg.bind_address, errors);
    read_uint(j, "port", cfg.port, errors);
    read_uint(j, "max_players", cfg.max_players, errors);
    read_uint(j, "max_packet_size", cfg.max_packet_size, errors);
    read_uint(j, "max_connections_per_ip", cfg.max_connections_per_ip, errors);
    read_string(j, "login_bind", cfg.login_bind, errors);
    read_uint(j, "login_port", cfg.login_port, errors);
    read_string(j, "tls_cert", cfg.tls_cert, errors);
    read_string(j, "tls_key", cfg.tls_key, errors);
    read_string(j, "health_bind", cfg.health_bind, errors);
    read_uint(j, "health_port", cfg.health_port, errors);
    read_string(j, "db_path", cfg.db_path, errors);
    read_string(j, "content_path", cfg.content_path, errors);
    read_uint(j, "autosave_seconds", cfg.autosave_seconds, errors);
    read_uint(j, "tick_rate", cfg.tick_rate, errors);
    read_string(j, "log_dir", cfg.log_dir, errors);
    read_string(j, "log_level", cfg.log_level, errors);
    read_string(j, "admin_password", cfg.admin_password, errors);
    read_double(j, "packets_per_second", cfg.packets_per_second, errors);
    read_double(j, "packet_burst", cfg.packet_burst, errors);
    read_double(j, "bytes_per_second", cfg.bytes_per_second, errors);
    read_double(j, "byte_burst", cfg.byte_burst, errors);
    read_uint(j, "max_violations", cfg.max_violations, errors);
    read_uint(j, "max_failed_logins", cfg.max_failed_logins, errors);
    if (!errors.empty()) throw_errors(errors);
  }
  apply_env_overrides(cfg, env);
  validate_config(cfg);
  return cfg;
}

}  // namespace ef
