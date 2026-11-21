#include "game/content.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ef::game {

namespace {

using nlohmann::json;

template <class T>
const T* find_by_id(const std::vector<T>& v, std::string_view id) {
  auto it = std::find_if(v.begin(), v.end(), [&](const T& t) { return t.id == id; });
  return it == v.end() ? nullptr : &*it;
}

// Small helper that records errors with a path prefix instead of throwing on the first one.
class Parser {
 public:
  explicit Parser(std::vector<std::string>& errors) : errors_(errors) {}

  std::string str(const json& j, const char* key, const std::string& where, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return {};
    }
    if (!j.at(key).is_string()) {
      errors_.push_back(where + ": '" + key + "' must be a string");
      return {};
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
    if (!std::isfinite(v)) {
      errors_.push_back(where + ": '" + key + "' must be finite");
      return fallback;
    }
    return static_cast<float>(v);
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
    return j.at(key).get<int>();
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

  const json* list(const json& doc, const char* key, const char* file) {
    if (!doc.is_object() || !doc.contains(key) || !doc.at(key).is_array()) {
      errors_.push_back(std::string(file) + ": top-level '" + key + "' array is required");
      return nullptr;
    }
    return &doc.at(key);
  }

 private:
  std::vector<std::string>& errors_;
};

[[noreturn]] void throw_errors(const std::vector<std::string>& errors) {
  std::ostringstream os;
  os << "content validation failed (" << errors.size() << " problem" << (errors.size() == 1 ? "" : "s") << "):";
  for (const auto& e : errors) os << "\n  - " << e;
  throw ContentError(os.str());
}

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 32) return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

template <class T>
void check_ids(const std::vector<T>& v, const char* kind, std::vector<std::string>& errors) {
  std::set<std::string> seen;
  for (const auto& t : v) {
    if (!valid_id(t.id)) errors.push_back(std::string(kind) + " id '" + t.id + "' must be 1-32 chars of [a-z0-9_]");
    if (!seen.insert(t.id).second) errors.push_back(std::string("duplicate ") + kind + " id '" + t.id + "'");
    if (t.name.empty() || t.name.size() > 48) errors.push_back(std::string(kind) + " '" + t.id + "': name must be 1-48 chars");
  }
}

json read_json(const std::filesystem::path& path, std::vector<std::string>& errors) {
  std::ifstream in(path);
  if (!in) {
    errors.push_back("cannot open " + path.string());
    return json::object();
  }
  try {
    return json::parse(in);
  } catch (const json::exception& e) {
    errors.push_back(path.string() + ": invalid JSON: " + e.what());
    return json::object();
  }
}

}  // namespace

const RaceDef* Content::find_race(std::string_view id) const { return find_by_id(races, id); }
const ClassDef* Content::find_class(std::string_view id) const { return find_by_id(classes, id); }
const CityDef* Content::find_city(std::string_view id) const { return find_by_id(cities, id); }
const ZoneDef* Content::find_zone(std::string_view id) const { return find_by_id(zones, id); }

Content parse_content(const json& rules, const json& races, const json& classes, const json& cities, const json& zones) {
  std::vector<std::string> errors;
  Parser p(errors);
  Content c;

  c.rules.max_level = p.integer(rules, "max_level", "rules", 60);
  c.rules.max_characters_per_account = p.integer(rules, "max_characters_per_account", "rules", 4);
  if (rules.is_object() && rules.contains("movement")) {
    const auto& m = rules.at("movement");
    c.rules.movement.run_speed = p.num(m, "run_speed", "rules.movement", 7.0f);
    c.rules.movement.input_budget_max = static_cast<std::uint32_t>(std::max(1, p.integer(m, "input_budget_max", "rules.movement", 4)));
    c.rules.movement.desync_tolerance = p.num(m, "desync_tolerance", "rules.movement", 2.0f);
    c.rules.movement.teleport_tolerance = p.num(m, "teleport_tolerance", "rules.movement", 1.5f);
  } else {
    errors.emplace_back("rules: missing 'movement'");
  }

  if (const auto* list = p.list(races, "races", "races.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "races[" + std::to_string(i) + "]";
      RaceDef r;
      r.id = p.str(j, "id", w);
      r.name = p.str(j, "name", w);
      r.description = p.str(j, "description", w, false);
      r.allowed_classes = p.str_list(j, "allowed_classes", w);
      r.cities = p.str_list(j, "cities", w);
      c.races.push_back(std::move(r));
    }
  }
  if (const auto* list = p.list(classes, "classes", "classes.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "classes[" + std::to_string(i) + "]";
      ClassDef d;
      d.id = p.str(j, "id", w);
      d.name = p.str(j, "name", w);
      d.archetype = p.str(j, "archetype", w);
      d.description = p.str(j, "description", w, false);
      c.classes.push_back(std::move(d));
    }
  }
  if (const auto* list = p.list(cities, "cities", "cities.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "cities[" + std::to_string(i) + "]";
      CityDef d;
      d.id = p.str(j, "id", w);
      d.name = p.str(j, "name", w);
      d.description = p.str(j, "description", w, false);
      d.zone = p.str(j, "zone", w);
      d.spawn = p.spawn(j, "spawn", w);
      c.cities.push_back(std::move(d));
    }
  }
  if (const auto* list = p.list(zones, "zones", "zones.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "zones[" + std::to_string(i) + "]";
      ZoneDef z;
      z.id = p.str(j, "id", w);
      z.name = p.str(j, "name", w);
      z.description = p.str(j, "description", w, false);
      if (j.is_object() && j.contains("bounds")) {
        const auto& b = j.at("bounds");
        z.min_x = p.num(b, "min_x", w + ".bounds", 0.0f);
        z.min_z = p.num(b, "min_z", w + ".bounds", 0.0f);
        z.max_x = p.num(b, "max_x", w + ".bounds", 0.0f);
        z.max_z = p.num(b, "max_z", w + ".bounds", 0.0f);
      } else {
        errors.push_back(w + ": missing 'bounds'");
      }
      z.ground_y = p.num(j, "ground_y", w, 0.0f);
      z.safe_spawn = p.spawn(j, "safe_spawn", w);
      z.interest_radius = p.num(j, "interest_radius", w, 80.0f);
      z.say_radius = p.num(j, "say_radius", w, 60.0f);
      c.zones.push_back(std::move(z));
    }
  }

  if (!errors.empty()) throw_errors(errors);
  auto problems = validate_content(c);
  if (!problems.empty()) throw_errors(problems);
  return c;
}

std::vector<std::string> validate_content(const Content& c) {
  std::vector<std::string> e;
  if (c.rules.max_level < 1 || c.rules.max_level > 255) e.emplace_back("rules.max_level must be 1..255");
  if (c.rules.max_characters_per_account < 1 || c.rules.max_characters_per_account > 16) e.emplace_back("rules.max_characters_per_account must be 1..16");
  if (c.rules.movement.run_speed <= 0.0f || c.rules.movement.run_speed > 50.0f) e.emplace_back("rules.movement.run_speed must be in (0, 50]");
  if (c.rules.movement.desync_tolerance <= 0.0f) e.emplace_back("rules.movement.desync_tolerance must be > 0");
  if (c.rules.movement.teleport_tolerance < 1.0f) e.emplace_back("rules.movement.teleport_tolerance must be >= 1");

  if (c.races.empty()) e.emplace_back("at least one race is required");
  if (c.classes.empty()) e.emplace_back("at least one class is required");
  if (c.cities.empty()) e.emplace_back("at least one city is required");
  if (c.zones.empty()) e.emplace_back("at least one zone is required");
  check_ids(c.races, "race", e);
  check_ids(c.classes, "class", e);
  check_ids(c.cities, "city", e);
  check_ids(c.zones, "zone", e);

  for (const auto& z : c.zones) {
    if (!(z.min_x < z.max_x && z.min_z < z.max_z)) e.push_back("zone '" + z.id + "': bounds min must be < max");
    if (z.interest_radius < 10.0f || z.interest_radius > 500.0f) e.push_back("zone '" + z.id + "': interest_radius must be 10..500");
    if (z.say_radius <= 0.0f) e.push_back("zone '" + z.id + "': say_radius must be > 0");
    const auto& s = z.safe_spawn;
    if (s.x < z.min_x || s.x > z.max_x || s.z < z.min_z || s.z > z.max_z) e.push_back("zone '" + z.id + "': safe_spawn is outside bounds");
  }
  for (const auto& city : c.cities) {
    const auto* z = c.find_zone(city.zone);
    if (!z) {
      e.push_back("city '" + city.id + "': unknown zone '" + city.zone + "'");
      continue;
    }
    const auto& s = city.spawn;
    if (s.x < z->min_x || s.x > z->max_x || s.z < z->min_z || s.z > z->max_z) e.push_back("city '" + city.id + "': spawn is outside zone bounds");
  }
  for (const auto& r : c.races) {
    if (r.allowed_classes.empty()) e.push_back("race '" + r.id + "': allowed_classes must not be empty");
    if (r.cities.empty()) e.push_back("race '" + r.id + "': cities must not be empty");
    if (r.allowed_classes.size() > 32 || r.cities.size() > 8) e.push_back("race '" + r.id + "': too many classes (max 32) or cities (max 8)");
    for (const auto& cl : r.allowed_classes)
      if (!c.find_class(cl)) e.push_back("race '" + r.id + "': unknown class '" + cl + "'");
    for (const auto& ci : r.cities)
      if (!c.find_city(ci)) e.push_back("race '" + r.id + "': unknown city '" + ci + "'");
  }
  return e;
}

Content load_content(const std::filesystem::path& dir) {
  std::vector<std::string> errors;
  auto rules = read_json(dir / "rules.json", errors);
  auto races = read_json(dir / "races.json", errors);
  auto classes = read_json(dir / "classes.json", errors);
  auto cities = read_json(dir / "cities.json", errors);
  auto zones = read_json(dir / "zones.json", errors);
  if (!errors.empty()) throw_errors(errors);
  return parse_content(rules, races, classes, cities, zones);
}

}  // namespace ef::game
