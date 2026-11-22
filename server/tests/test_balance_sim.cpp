// Balance simulator v1 smoke test: every class solo-kills even-level mobs at a few levels with its
// content-only kit, and no class is off by an extreme factor. The full table (and the outliers it
// flags) comes from tools/balance_sim; see docs/DECISIONS.md.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>

#include "balance_sim.hpp"
#include "zone_support.hpp"

TEST_CASE("every class can solo even-level mobs, and none is off by an extreme factor", "[classes][balance]") {
  const auto& c = zt::content();
  ef::sim::SimOptions opt;
  opt.fights = 4;
  opt.seed = 7;
  std::vector<ef::sim::FightStats> rows;
  for (const auto& cl : c.classes)
    for (int level : {10, 30, 50}) rows.push_back(ef::sim::simulate(c, cl.id, level, opt));
  std::printf("%s", ef::sim::format_table(rows, {10, 30, 50}).c_str());
  for (const auto& r : rows) {
    INFO(r.class_id << " at level " << r.level << ": won " << r.wins << "/" << r.fights);
    CHECK(r.wins >= 1);  // every class can kill an even-level mob with nothing but its kit
  }
  // "Extreme" is a factor of five from the median time to kill, or losing most fights. Smaller
  // gaps are tuning work and are flagged by the tool, not failed here.
  const auto extreme = ef::sim::find_outliers(rows, 5.0, 0.25);
  for (const auto& o : extreme) UNSCOPED_INFO(o.class_id << " L" << o.level << ": " << o.why);
  CHECK(extreme.empty());
  // The kit is built from content alone.
  const auto kit = ef::sim::kit_summary(c, "magician", 30);
  REQUIRE_FALSE(kit.empty());
  CHECK(kit.front().find("servant") != std::string::npos);
}

TEST_CASE("progression: every band takes time, groups level no slower than solo, and no class is far off", "[classes][balance]") {
  const auto& c = zt::content();
  ef::sim::ProgressionOptions opt;
  opt.fights = 2;
  opt.step = 15;
  opt.seed = 3;
  std::vector<ef::sim::Progression> rows;
  for (const char* cls : {"warrior", "cleric", "wizard", "rogue"})
    for (const bool group : {false, true}) rows.push_back(ef::sim::simulate_progression(c, cls, group, opt));
  std::printf("%s", ef::sim::format_progression(rows, false).c_str());
  for (const auto& r : rows) {
    INFO(r.class_id << (r.group ? " grouped" : " solo") << ": " << r.total_hours << " h");
    REQUIRE(r.levels.size() == static_cast<std::size_t>(c.rules.max_level - 1));
    double bands = 0.0;
    for (double h : r.band_hours) {
      CHECK(h > 0.0);
      bands += h;
    }
    CHECK(bands == Catch::Approx(r.total_hours));
    CHECK(r.total_hours < 1000.0);  // nobody stalls
    CHECK(r.deaths * 5 <= r.fights);
  }
  // Grouping pays: the grouped median is no slower than the solo median (give or take sampling noise).
  std::vector<double> solo, grouped;
  for (const auto& r : rows) (r.group ? grouped : solo).push_back(r.total_hours);
  std::sort(solo.begin(), solo.end());
  std::sort(grouped.begin(), grouped.end());
  CHECK(grouped[grouped.size() / 2] <= solo[solo.size() / 2] * 1.1);
  CHECK(ef::sim::progression_outliers(rows, 2.0, 0.2).empty());
  CHECK(ef::sim::standard_group(c, "warrior").size() == 6);
  CHECK(ef::sim::standard_group(c, "enchanter").front() == "enchanter");
}
