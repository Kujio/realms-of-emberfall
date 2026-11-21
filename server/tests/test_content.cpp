#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "game/content.hpp"

using namespace ef::game;
using nlohmann::json;

TEST_CASE("repository content loads and validates", "[content]") {
  const Content c = load_content(EF_CONTENT_DIR);
  CHECK(c.rules.max_level == 60);
  CHECK(c.classes.size() == 14);
  CHECK(c.races.size() >= 3);
  CHECK(c.find_zone("proving_flats") != nullptr);
  for (const auto& r : c.races) {
    for (const auto& city : r.cities) CHECK(c.find_city(city) != nullptr);
  }
}

namespace {
json rules() { return json::parse(R"({"max_level":60,"max_characters_per_account":4,"movement":{"run_speed":7,"input_budget_max":4,"desync_tolerance":2,"teleport_tolerance":1.5}})"); }
json classes() { return json::parse(R"({"classes":[{"id":"warrior","name":"Warrior","archetype":"tank"}]})"); }
json cities() { return json::parse(R"({"cities":[{"id":"town","name":"Town","zone":"z","spawn":{"x":0,"y":0,"z":0,"yaw":0}}]})"); }
json zones() { return json::parse(R"({"zones":[{"id":"z","name":"Z","bounds":{"min_x":-10,"min_z":-10,"max_x":10,"max_z":10},"ground_y":0,"safe_spawn":{"x":0,"y":0,"z":0,"yaw":0},"interest_radius":50,"say_radius":30}]})"); }
}  // namespace

TEST_CASE("bad references fail fast", "[content]") {
  auto races = json::parse(R"({"races":[{"id":"r","name":"R","allowed_classes":["wizard"],"cities":["nowhere"]}]})");
  try {
    parse_content(rules(), races, classes(), cities(), zones());
    FAIL("expected ContentError");
  } catch (const ContentError& e) {
    const std::string msg = e.what();
    CHECK(msg.find("unknown class 'wizard'") != std::string::npos);
    CHECK(msg.find("unknown city 'nowhere'") != std::string::npos);
  }
}

TEST_CASE("valid minimal content parses", "[content]") {
  auto races = json::parse(R"({"races":[{"id":"r","name":"R","allowed_classes":["warrior"],"cities":["town"]}]})");
  auto c = parse_content(rules(), races, classes(), cities(), zones());
  CHECK(c.find_race("r") != nullptr);
}

TEST_CASE("structural errors are reported", "[content]") {
  auto races = json::parse(R"({"races":[{"id":"Bad Id","allowed_classes":"warrior","cities":["town"]}]})");
  CHECK_THROWS_AS(parse_content(rules(), races, classes(), cities(), zones()), ContentError);
  CHECK_THROWS_AS(parse_content(rules(), json::object(), classes(), cities(), zones()), ContentError);
}
