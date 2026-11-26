// Phase 13: tradeskill content (content/crafts.json): the combine rules, the tradeskills and their
// stations, and every recipe. Parsed and checked in content_crafts.cpp; the rules themselves are pure
// functions in crafting.hpp.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ef::game {

struct CraftRules {
  int max_skill = 300;               // every tradeskill's ceiling (no level cap)
  float combine_seconds = 1.5f;      // the least time between two combines
  float success_max = 0.95f;         // at or above a recipe's trivial
  float success_min = 0.05f;         // the floor, however far below
  float success_per_point = 0.012f;  // lost per point of skill below the trivial
  float skill_up_base = 0.2f;        // skill-up chance one point below the trivial
  float skill_up_per_point = 0.01f;  // added per further point below
  float skill_up_max = 0.6f;
  float fail_loss_chance = 0.75f;    // a failed combine loses each unit of material with this chance
};

struct TradeskillDef {
  std::string id;
  std::string name;
  std::string station;       // the station kind it works at (station NPCs carry it)
  std::string station_name;  // what that station is called
  std::string description;
};

struct ItemCount {
  std::string item;
  int count = 1;
};

struct RecipeDef {
  std::string id;
  std::string tradeskill;
  int trivial = 0;  // past this skill the recipe teaches nothing and nearly always succeeds
  ItemCount result;
  std::vector<ItemCount> ingredients;  // used up (a failure loses some)
  std::vector<std::string> tools;      // must be in the pack; never used up
  std::string book;  // Phase 16: the recipe book that teaches it ("" = known from the start)
};

struct CraftContent {
  CraftRules rules;
  std::vector<TradeskillDef> tradeskills;
  std::vector<RecipeDef> recipes;

  const TradeskillDef* find_tradeskill(std::string_view id) const;
  // The tradeskill worked at a station kind (nullptr if none).
  const TradeskillDef* for_station(std::string_view station) const;
  const RecipeDef* find_recipe(std::string_view id) const;
  // Rebuilds the id index find_recipe uses (parsing calls it; tests that edit `recipes` may too).
  void index();

 private:
  std::unordered_map<std::string, std::size_t> recipe_index_;  // id -> position (verified on use)
};

}  // namespace ef::game
