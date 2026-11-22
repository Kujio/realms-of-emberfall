// Internal: the JSON reader shared by the content translation units (content.cpp, content_world.cpp).
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/content.hpp"

namespace ef::game::content_detail {

using nlohmann::json;

// Small helper that records errors with a path prefix instead of throwing on the first one.
class Parser {
 public:
  explicit Parser(std::vector<std::string>& errors) : errors_(errors) {}

  std::string str(const json& j, const char* key, const std::string& where, bool required = true, std::string fallback = {}) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_string()) {
      errors_.push_back(where + ": '" + key + "' must be a string");
      return fallback;
    }
    return j.at(key).get<std::string>();
  }

  float num(const json& j, const char* key, const std::string& where, float fallback, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_number()) {
      errors_.push_back(where + ": '" + key + "' must be a number");
      return fallback;
    }
    const double v = j.at(key).get<double>();
    if (!std::isfinite(v) || std::fabs(v) > 1e9) {
      errors_.push_back(where + ": '" + key + "' must be a finite number");
      return fallback;
    }
    return static_cast<float>(v);
  }

  // Optional number with a default (no error when absent).
  float opt(const json& j, const char* key, const std::string& where, float fallback) { return num(j, key, where, fallback, false); }

  double dbl(const json& j, const char* key, const std::string& where, double fallback) {
    return static_cast<double>(num(j, key, where, static_cast<float>(fallback), false));
  }

  int integer(const json& j, const char* key, const std::string& where, int fallback, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_number_integer()) {
      errors_.push_back(where + ": '" + key + "' must be an integer");
      return fallback;
    }
    const auto v = j.at(key).get<long long>();
    if (v < -1000000000LL || v > 1000000000LL) {
      errors_.push_back(where + ": '" + key + "' is out of range");
      return fallback;
    }
    return static_cast<int>(v);
  }

  bool boolean(const json& j, const char* key, const std::string& where, bool fallback, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_boolean()) {
      errors_.push_back(where + ": '" + key + "' must be true or false");
      return fallback;
    }
    return j.at(key).get<bool>();
  }

  std::vector<std::string> str_list(const json& j, const char* key, const std::string& where) {
    std::vector<std::string> out;
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
      errors_.push_back(where + ": '" + key + "' must be an array of strings");
      return out;
    }
    for (const auto& v : j.at(key)) {
      if (!v.is_string()) {
        errors_.push_back(where + ": '" + key + "' must contain only strings");
        continue;
      }
      out.push_back(v.get<std::string>());
    }
    return out;
  }

  SpawnPoint spawn(const json& j, const char* key, const std::string& where) {
    SpawnPoint s;
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_object()) {
      errors_.push_back(where + ": missing object '" + key + "'");
      return s;
    }
    const auto& o = j.at(key);
    const std::string w = where + "." + key;
    s.x = num(o, "x", w, 0.0f);
    s.y = num(o, "y", w, 0.0f);
    s.z = num(o, "z", w, 0.0f);
    s.yaw = num(o, "yaw", w, 0.0f);
    return s;
  }

  StatBlock stats(const json& j, const std::string& where, StatBlock fallback, bool all_required) {
    StatBlock s = fallback;
    if (!j.is_object()) {
      if (all_required) errors_.push_back(where + ": stats must be an object");
      return s;
    }
    s.str = integer(j, "str", where, s.str, all_required);
    s.sta = integer(j, "sta", where, s.sta, all_required);
    s.agi = integer(j, "agi", where, s.agi, all_required);
    s.dex = integer(j, "dex", where, s.dex, all_required);
    s.wis = integer(j, "wis", where, s.wis, all_required);
    s.intel = integer(j, "int", where, s.intel, all_required);
    s.cha = integer(j, "cha", where, s.cha, all_required);
    return s;
  }

  SkillCap skill_cap(const json& j, const std::string& where) {
    SkillCap c;
    if (!j.is_object()) {
      errors_.push_back(where + ": skill cap must be an object");
      return c;
    }
    c.level = integer(j, "level", where, 1);
    c.per_level = num(j, "per_level", where, 5.0f);
    c.max = integer(j, "max", where, 250);
    return c;
  }

  // A top-level array. A null document is an empty list (tests pass only what they need).
  const json* list(const json& doc, const char* key, const char* file) {
    if (doc.is_null()) return nullptr;
    if (!doc.is_object() || !doc.contains(key) || !doc.at(key).is_array()) {
      errors_.push_back(std::string(file) + ": top-level '" + key + "' array is required");
      return nullptr;
    }
    return &doc.at(key);
  }

  std::vector<std::string>& errors() { return errors_; }
  void error(std::string msg) { errors_.push_back(std::move(msg)); }

 private:
  std::vector<std::string>& errors_;
};


// content_world.cpp
void parse_zones(Parser& p, const ContentDocs& docs, Content& c);
void parse_travel(Parser& p, const ContentDocs& docs, Content& c);
// After everything is parsed: dungeon layouts, rooms to positions, scene NPC placements, layout hashes.
void finalize_world(Content& c, std::vector<std::string>& errors);
void validate_world(const Content& c, std::vector<std::string>& e);
// content_quests.cpp
void parse_quests(Parser& p, const ContentDocs& docs, Content& c);
void validate_quests(const Content& c, std::vector<std::string>& e);

}  // namespace ef::game::content_detail
