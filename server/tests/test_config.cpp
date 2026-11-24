#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>

#include "core/config.hpp"

using namespace ef;

namespace {
EnvLookup fake_env(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](const char* name) -> std::optional<std::string> {
    auto it = vars.find(name);
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}
}  // namespace

TEST_CASE("defaults are valid", "[config]") {
  auto cfg = load_config("", fake_env({}));
  CHECK(cfg.port == 7777);
  CHECK(cfg.tick_rate == 20);
}

TEST_CASE("env overrides file", "[config]") {
  const auto path = std::filesystem::temp_directory_path() / "emberfall_test_config.json";
  {
    std::ofstream f(path);
    f << R"({"port": 9000, "max_players": 50, "db_path": "x.db"})";
  }
  auto cfg = load_config(path.string(), fake_env({{"EMBERFALL_PORT", "9100"}, {"EMBERFALL_TICK_RATE", "30"}, {"EMBERFALL_RNG_SEED", "4242"}}));
  CHECK(cfg.port == 9100);
  CHECK(cfg.rng_seed == 4242);  // Phase 12: the smoke test pins the dice
  CHECK(cfg.max_players == 50);
  CHECK(cfg.tick_rate == 30);
  CHECK(cfg.db_path == "x.db");
  std::filesystem::remove(path);
}

TEST_CASE("bad config fails fast with a clear error", "[config]") {
  CHECK_THROWS_AS(load_config("", fake_env({{"EMBERFALL_PORT", "notanumber"}})), ConfigError);
  CHECK_THROWS_AS(load_config("", fake_env({{"EMBERFALL_PORT", "70000"}})), ConfigError);
  CHECK_THROWS_AS(load_config("", fake_env({{"EMBERFALL_TICK_RATE", "0"}})), ConfigError);
  CHECK_THROWS_AS(load_config("/nonexistent/emberfall.json", fake_env({})), ConfigError);

  const auto path = std::filesystem::temp_directory_path() / "emberfall_test_config_bad.json";
  {
    std::ofstream f(path);
    f << R"({"prot": 9000})";
  }
  CHECK_THROWS_AS(load_config(path.string(), fake_env({})), ConfigError);
  std::filesystem::remove(path);
}
