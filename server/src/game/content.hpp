// Data-driven game content loaded from /content JSON. Validated at startup (fail fast).
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace ef::game {

struct SpawnPoint {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
};

struct RaceDef {
  std::string id;
  std::string name;
  std::string description;
  std::vector<std::string> allowed_classes;
  std::vector<std::string> cities;
};

struct ClassDef {
  std::string id;
  std::string name;
  std::string archetype;
  std::string description;
};

struct CityDef {
  std::string id;
  std::string name;
  std::string description;
  std::string zone;
  SpawnPoint spawn;
};

struct ZoneDef {
  std::string id;
  std::string name;
  std::string description;
  float min_x = 0.0f;
  float min_z = 0.0f;
  float max_x = 0.0f;
  float max_z = 0.0f;
  float ground_y = 0.0f;
  SpawnPoint safe_spawn;
  float interest_radius = 80.0f;
  float say_radius = 60.0f;
};

struct MovementRules {
  float run_speed = 7.0f;            // metres/second
  std::uint32_t input_budget_max = 4;  // max banked movement inputs (jitter absorption)
  float desync_tolerance = 2.0f;     // metres between client prediction and server before a hard correction
  float teleport_tolerance = 1.5f;   // multiplier on max legal per-input displacement
};

struct Rules {
  int max_level = 60;
  int max_characters_per_account = 4;
  MovementRules movement;
};

struct Content {
  Rules rules;
  std::vector<RaceDef> races;
  std::vector<ClassDef> classes;
  std::vector<CityDef> cities;
  std::vector<ZoneDef> zones;

  const RaceDef* find_race(std::string_view id) const;
  const ClassDef* find_class(std::string_view id) const;
  const CityDef* find_city(std::string_view id) const;
  const ZoneDef* find_zone(std::string_view id) const;
};

class ContentError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Parses the five JSON documents. Throws ContentError listing every problem found.
Content parse_content(const nlohmann::json& rules, const nlohmann::json& races, const nlohmann::json& classes,
                      const nlohmann::json& cities, const nlohmann::json& zones);

// Cross-reference and range checks. Returns all problems (empty = valid).
std::vector<std::string> validate_content(const Content& c);

// Loads <dir>/{rules,races,classes,cities,zones}.json, parses, and validates. Throws ContentError.
Content load_content(const std::filesystem::path& dir);

}  // namespace ef::game
