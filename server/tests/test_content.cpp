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

namespace {
// Every shipped content document, for tests that break one rule at a time.
ContentDocs shipped() {
  auto read = [](const char* name) {
    std::ifstream in(std::filesystem::path(EF_CONTENT_DIR) / name);
    return json::parse(in);
  };
  ContentDocs d;
  d.rules = read("rules.json");
  d.races = read("races.json");
  d.classes = read("classes.json");
  d.cities = read("cities.json");
  d.zones = read("zones.json");
  d.skills = read("skills.json");
  d.abilities = read("abilities.json");
  d.items = read("items.json");
  d.loot = read("loot.json");
  d.mobs = read("mobs.json");
  d.spells = read("spells.json");
  d.npcs = read("npcs.json");
  d.pets = read("pets.json");
  d.models = read("models.json");
  d.appearance = read("appearance.json");
  d.item_effects = read("item_effects.json");
  return d;
}

json& spell_doc(ContentDocs& d, const std::string& id) {
  for (auto& s : d.spells["spells"])
    if (s["id"] == id) return s;
  throw std::runtime_error("no spell " + id);
}
}  // namespace

TEST_CASE("shipped spells: 25+ per caster, every one learnable, every level band covered", "[content][spells]") {
  const Content c = load_content(EF_CONTENT_DIR);
  for (const char* cls : {"cleric", "wizard"}) {
    const auto spells = c.spells_for(cls);
    REQUIRE(spells.size() >= 25);
    CHECK(spells.size() <= 40);
    CHECK(spells.front()->level == 1);
    for (const auto& [lo, hi] : kLevelBands)
      CHECK(std::any_of(spells.begin(), spells.end(), [&](const AbilityDef* a) { return a->level >= lo && a->level <= hi; }));
    // A new spell at least every three levels on average, with tiered ranks.
    CHECK(spells.back()->level >= 55);
    for (const AbilityDef* a : spells) {
      const ItemDef* scroll = c.scroll_for(a->id);
      REQUIRE(scroll != nullptr);
      CHECK(scroll->value > 0);
    }
    const ClassDef* cl = c.find_class(cls);
    REQUIRE(cl);
    CHECK(cl->starter_spells.size() == 2);
    CHECK(cl->skill("channeling") != nullptr);
  }
  CHECK(c.spells_for("warrior").empty());
  // One trainer per spell-using class, plus the practice lockbox.
  CHECK(std::count_if(c.npcs.begin(), c.npcs.end(), [](const NpcDef& n) { return n.role == "trainer"; }) == 11);
  // Some spells have a higher rank in the same stack group.
  CHECK(std::count_if(c.spells.begin(), c.spells.end(), [](const AbilityDef& a) { return a.rank > 1; }) > 20);
}

TEST_CASE("spell rules are validated: polarity, reachability, bands, gems, destinations", "[content][spells]") {
  SECTION("a beneficial spell cannot harm") {
    auto d = shipped();
    spell_doc(d, "blessed_ward")["effects"].push_back(json::parse(R"({"type":"damage","base":5})"));
    CHECK(error_of(d).find("a beneficial spell cannot carry harmful effects") != std::string::npos);
  }
  SECTION("a detrimental spell cannot heal") {
    auto d = shipped();
    spell_doc(d, "ember_dart")["effects"].push_back(json::parse(R"({"type":"heal","base":5})"));
    CHECK(error_of(d).find("a detrimental spell cannot heal") != std::string::npos);
    auto d2 = shipped();
    spell_doc(d2, "ember_dart")["effects"].push_back(json::parse(R"({"type":"heal_pct","value":25})"));
    CHECK(error_of(d2).find("a detrimental spell cannot heal") != std::string::npos);
  }
  SECTION("a spell no trainer, drop, or starter grant teaches is unreachable") {
    auto d = shipped();
    for (auto& n : d.npcs["npcs"]) {
      auto& sells = n["sells"];
      sells.erase(std::remove(sells.begin(), sells.end(), json("scroll_sunflare")), sells.end());
    }
    // Scrolls also drop from loot pools (Phase 5): take those away too.
    for (auto& t : d.loot["loot_tables"])
      for (auto& pool : t["pools"]) {
        auto& es = pool["entries"];
        es.erase(std::remove_if(es.begin(), es.end(), [](const json& e) { return e.value("item", "") == "scroll_sunflare"; }), es.end());
      }
    CHECK(error_of(d).find("spell 'sunflare': not reachable") != std::string::npos);
  }
  SECTION("a class with abilities must cover every level band") {
    auto d = shipped();
    auto& list = d.spells["spells"];
    list.erase(std::remove_if(list.begin(), list.end(), [](const json& s) { return s["class"] == "wizard" && s["level"].get<int>() >= 45; }),
               list.end());
    auto& items = d.items["items"];
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const json& it) {
                                 if (!it.contains("spell")) return false;
                                 return std::none_of(list.begin(), list.end(), [&](const json& s) { return s["id"] == it["spell"]; });
                               }),
                items.end());
    for (auto& n : d.npcs["npcs"]) {
      auto& sells = n["sells"];
      sells.erase(std::remove_if(sells.begin(), sells.end(),
                                 [&](const json& id) { return std::none_of(items.begin(), items.end(), [&](const json& it) { return it["id"] == id; }); }),
                  sells.end());
    }
    CHECK(error_of(d).find("class 'wizard': no abilities in levels 45-60") != std::string::npos);
  }
  SECTION("gem unlocks must be a non-decreasing list of up to 8 levels") {
    auto d = shipped();
    d.rules["spells"]["gem_unlock_levels"] = json::parse("[1, 5, 3]");
    CHECK(error_of(d).find("gem_unlock_levels must be non-decreasing") != std::string::npos);
    d.rules["spells"]["gem_unlock_levels"] = json::parse("[1,1,1,1,1,1,1,1,1]");
    CHECK(error_of(d).find("needs 1..8 entries") != std::string::npos);
  }
  SECTION("teleports name a real destination; corpse targets resurrect") {
    auto d = shipped();
    spell_doc(d, "homeward_step")["effects"][0]["destination"] = "atlantis";
    spell_doc(d, "rite_of_return")["target"] = "friendly";
    const auto msg = error_of(d);
    CHECK(msg.find("unknown teleport destination 'atlantis'") != std::string::npos);
    CHECK(msg.find("corpse targets go with a resurrect effect") != std::string::npos);
  }
  SECTION("scrolls must teach a real spell; trainers only sell to classes they teach") {
    auto d = shipped();
    d.items["items"].push_back(json::parse(R"({"id":"scroll_bogus","name":"Spell: Bogus","kind":"scroll","stack":1,"value":5,"spell":"no_such_spell"})"));
    d.npcs["npcs"][0]["sells"].push_back("scroll_ember_dart");
    const auto msg = error_of(d);
    CHECK(msg.find("unknown spell 'no_such_spell'") != std::string::npos);
    CHECK(msg.find("to a class it does not teach") != std::string::npos);
  }
  SECTION("effects need sane numbers") {
    auto d = shipped();
    spell_doc(d, "frost_needle")["effects"][1]["value"] = 150;  // a 150% snare
    spell_doc(d, "smoldering_brand")["effects"][0]["duration"] = 0;
    const auto msg = error_of(d);
    CHECK(msg.find("slow/snare value is a percentage") != std::string::npos);
    CHECK(msg.find("lasting effects need a duration") != std::string::npos);
  }
  SECTION("corpse spells only resurrect; starters fit the level-1 gems and the book") {
    auto d = shipped();
    spell_doc(d, "rite_of_return")["effects"].push_back(json::parse(R"({"type":"teleport","destination":"safe_spot"})"));
    d.rules["spells"]["gem_unlock_levels"] = json::parse("[1, 4]");
    d.rules["spells"]["spellbook_size"] = 1;
    const auto msg = error_of(d);
    CHECK(msg.find("a corpse-target spell can only resurrect") != std::string::npos);
    CHECK(msg.find("more starter spells than gems open at level 1") != std::string::npos);
    CHECK(msg.find("starter spells do not fit the spellbook") != std::string::npos);
  }
  SECTION("a second scroll for the same spell is a valid route") {
    auto d = shipped();
    d.items["items"].push_back(json::parse(R"({"id":"scroll_sunflare_b","name":"Spell: Sunflare","kind":"scroll","stack":1,"value":5,"spell":"sunflare"})"));
    for (auto& n : d.npcs["npcs"])
      for (auto& x : n["sells"])
        if (x == "scroll_sunflare") x = "scroll_sunflare_b";
    CHECK(error_of(d).empty());
  }
  SECTION("default gem levels fit a lower level cap") {
    auto d = docs(good_races());
    d.rules["max_level"] = 10;
    CHECK(error_of(d).empty());
    d.rules["spells"] = json::parse(R"({"gem_unlock_levels": [1, 1, 20]})");
    CHECK(error_of(d).find("gem_unlock_levels must be non-decreasing levels within 1..max_level") != std::string::npos);
  }
  SECTION("the unmodified documents are valid") { CHECK(error_of(shipped()).empty()); }
}

TEST_CASE("shipped models: every mob, npc, race and gender resolves; reach follows the model", "[content][models]") {
  const Content c = load_content(EF_CONTENT_DIR);
  REQUIRE(c.find_model("humanoid") != nullptr);
  for (const auto& m : c.mobs) {
    const ModelDef* md = c.find_model(m.model);
    REQUIRE(md != nullptr);
    CHECK(m.collision_radius == md->collision_radius);
  }
  for (const auto& r : c.races) {
    auto it = std::find_if(c.race_appearance.begin(), c.race_appearance.end(), [&](const RaceAppearance& a) { return a.race == r.id; });
    REQUIRE(it != c.race_appearance.end());
    for (const auto& model : it->models) CHECK(c.find_model(model) != nullptr);
  }
  const ItemDef* sword = c.find_item("training_sword");
  REQUIRE(sword != nullptr);
  REQUIRE(c.find_item_visual(sword->visual) != nullptr);
  CHECK(c.find_item_visual(sword->visual)->slot == "main_hand");
}

TEST_CASE("model and appearance references are checked", "[content][models]") {
  auto d = shipped();
  d.mobs["mobs"][0]["model"] = "no_such_model";
  d.npcs["npcs"][0]["model"] = "ghost_model";
  d.models["models"][0]["collision_radius"] = 0;
  d.models["models"][0]["animations"] = "no_map";
  d.appearance["races"].erase(d.appearance["races"].begin());  // highlander loses its look
  d.appearance["races"][0]["male"]["model"] = "nobody";
  d.appearance["classes"][0]["held"]["off_hand"] = "training_sword";  // a main-hand item
  d.appearance["classes"][1]["held"]["main_hand"] = "cracked_carapace";  // no visual
  d.items["items"][0]["visual"] = "nothing";
  const std::string msg = error_of(d);
  CHECK(msg.find("mob 'ember_rat': unknown model 'no_such_model'") != std::string::npos);
  CHECK(msg.find("unknown model 'ghost_model'") != std::string::npos);
  CHECK(msg.find("collision_radius must be 0.05..15") != std::string::npos);
  CHECK(msg.find("unknown animation map 'no_map'") != std::string::npos);
  CHECK(msg.find("race 'highlander': no entry in appearance.json") != std::string::npos);
  CHECK(msg.find("male uses unknown model 'nobody'") != std::string::npos);
  CHECK(msg.find("belongs in main_hand, not off_hand") != std::string::npos);
  CHECK(msg.find("holds item 'cracked_carapace', which has no visual") != std::string::npos);
  CHECK(msg.find("unknown visual 'nothing'") != std::string::npos);
}

TEST_CASE("models.json and appearance.json are required on disk", "[content][models]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("emberfall_content_models_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::copy(EF_CONTENT_DIR, dir, fs::copy_options::recursive);
  fs::remove(dir / "appearance.json");
  try {
    load_content(dir);
    FAIL("expected ContentError");
  } catch (const ContentError& e) {
    CHECK(std::string(e.what()).find("appearance.json") != std::string::npos);
  }
  fs::copy_file(fs::path(EF_CONTENT_DIR) / "appearance.json", dir / "appearance.json");
  std::ofstream(dir / "models.json") << R"({"models": [], "animation_maps": [], "item_visuals": []})";
  try {
    load_content(dir);
    FAIL("expected ContentError for an empty model catalog");
  } catch (const ContentError& e) {
    CHECK(std::string(e.what()).find("at least one model") != std::string::npos);
  }
  fs::remove_all(dir);
}
