#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

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
json skills() { return json::parse(R"({"skills":[{"id":"offense","name":"Offense","kind":"weapon"},{"id":"defense","name":"Defense","kind":"defense"},{"id":"kick","name":"Kick","kind":"ability"}],"defaults":{"offense":{"level":1,"per_level":4,"max":200},"defense":{"level":1,"per_level":4,"max":200}}})"); }

ContentDocs docs(json races) {
  ContentDocs d;
  d.rules = rules();
  d.races = std::move(races);
  d.classes = classes();
  d.cities = cities();
  d.zones = zones();
  d.skills = skills();
  return d;
}
json good_races() { return json::parse(R"({"races":[{"id":"r","name":"R","allowed_classes":["warrior"],"cities":["town"]}]})"); }

std::string error_of(const ContentDocs& d) {
  try {
    parse_content(d);
  } catch (const ContentError& e) {
    return e.what();
  }
  return {};
}
}  // namespace

TEST_CASE("bad references fail fast", "[content]") {
  auto races = json::parse(R"({"races":[{"id":"r","name":"R","allowed_classes":["wizard"],"cities":["nowhere"]}]})");
  try {
    parse_content(docs(races));
    FAIL("expected ContentError");
  } catch (const ContentError& e) {
    const std::string msg = e.what();
    CHECK(msg.find("unknown class 'wizard'") != std::string::npos);
    CHECK(msg.find("unknown city 'nowhere'") != std::string::npos);
  }
}

TEST_CASE("valid minimal content parses", "[content]") {
  auto c = parse_content(docs(good_races()));
  CHECK(c.find_race("r") != nullptr);
  REQUIRE(c.find_class("warrior") != nullptr);
  CHECK(c.find_class("warrior")->skill("offense") != nullptr);  // defaults merged in
  CHECK(c.rules.xp.growth > 1.0);                                // rules sections default
}

TEST_CASE("structural errors are reported", "[content]") {
  auto races = json::parse(R"({"races":[{"id":"Bad Id","allowed_classes":"warrior","cities":["town"]}]})");
  CHECK_THROWS_AS(parse_content(docs(races)), ContentError);
  CHECK_THROWS_AS(parse_content(docs(json::object())), ContentError);
}

TEST_CASE("shipped combat content is consistent", "[content]") {
  const Content c = load_content(EF_CONTENT_DIR);
  CHECK(c.mobs.size() >= 25);
  int lo = 99, hi = 0;
  for (const auto& m : c.mobs) {
    lo = std::min(lo, m.level);
    hi = std::max(hi, m.level);
    CHECK(c.find_loot(m.loot) != nullptr);
  }
  CHECK(lo == 1);
  CHECK(hi == 60);
  const auto warrior = c.abilities_for("warrior");
  REQUIRE(warrior.size() >= 10);
  CHECK(warrior.front()->level == 1);
  for (std::size_t i = 1; i < warrior.size(); ++i) CHECK(warrior[i - 1]->level <= warrior[i]->level);
  const auto* z = c.find_zone("proving_flats");
  REQUIRE(z != nullptr);
  CHECK(z->spawns.size() >= 25);
}

TEST_CASE("ability, mob, loot, and spawn references are checked", "[content]") {
  auto d = docs(good_races());
  d.abilities = json::parse(R"({"abilities":[
    {"id":"zap","name":"Zap","class":"wizard","level":1,"type":"nuke","resource":"mana","cost":5,"cast_time":0,"cooldown":1,"gcd":true,"range":10,"target":"enemy","effects":[{"type":"explode"}]},
    {"id":"kick2","name":"Kick","class":"warrior","level":1,"type":"passive","skill":"kick"}]})");
  d.items = json::parse(R"({"items":[{"id":"gem","name":"Gem","kind":"loot","stack":5,"value":1}]})");
  d.loot = json::parse(R"({"loot_tables":[{"id":"t","coin_min":5,"coin_max":1,"max_drops":1,"drops":[{"item":"nope","chance":2,"min":1,"max":1}]}]})");
  d.mobs = json::parse(R"({"mobs":[{"id":"m","name":"a m","level":99,"model":"rat","scale":1,"hp_mult":1,"damage_mult":1,"attack_delay":2,"run_speed":5,"aggro_radius":0,"assist":false,"assist_radius":0,"faction":"x","flee_hp_pct":0,"wander_radius":0,"leash_radius":50,"respawn_seconds":10,"stun_immune":false,"loot":"missing"}]})");
  d.zones["zones"][0]["spawns"] = json::parse(R"([{"mob":"ghost","x":0,"z":0}])");
  const std::string msg = error_of(d);
  CHECK(msg.find("unknown effect type") != std::string::npos);
  // The structural error stops parsing before cross-reference checks; fix it and look again.
  d.abilities["abilities"][0]["effects"][0]["type"] = "damage";
  const std::string msg2 = error_of(d);
  CHECK(msg2.find("unknown class 'wizard'") != std::string::npos);
  CHECK(msg2.find("lacks skill 'kick'") != std::string::npos);
  CHECK(msg2.find("coin_min <= coin_max") != std::string::npos);
  CHECK(msg2.find("unknown item 'nope'") != std::string::npos);
  CHECK(msg2.find("chance must be 0..1") != std::string::npos);
  CHECK(msg2.find("level must be 1..max_level") != std::string::npos);
  CHECK(msg2.find("unknown loot table 'missing'") != std::string::npos);
  CHECK(msg2.find("unknown mob 'ghost'") != std::string::npos);
}

TEST_CASE("a content file on disk must be an object, not null", "[content]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("emberfall_content_null_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::copy(EF_CONTENT_DIR, dir, fs::copy_options::recursive);
  std::ofstream(dir / "abilities.json") << "null";
  try {
    load_content(dir);
    FAIL("expected ContentError");
  } catch (const ContentError& e) {
    CHECK(std::string(e.what()).find("abilities.json: must be a JSON object") != std::string::npos);
  }
  fs::remove_all(dir);
}

TEST_CASE("XP curves whose total would overflow are rejected", "[content]") {
  auto d = docs(good_races());
  d.rules["xp"] = json::parse(R"({"base": 1000000000, "level_exponent": 4, "growth": 1.0})");
  d.rules["max_level"] = 255;
  CHECK(error_of(d).find("total XP to max_level must stay below 1e15") != std::string::npos);
  d.rules["max_level"] = 60;
  d.rules["xp"] = json::parse(R"({"base": 10, "level_exponent": 2, "growth": 1.0718})");
  CHECK(error_of(d).empty());
}

TEST_CASE("a rules section that is present but not an object is an error", "[content]") {
  auto d = docs(good_races());
  d.rules["death"] = json::array();
  CHECK(error_of(d).find("rules: 'death' must be an object") != std::string::npos);
}
