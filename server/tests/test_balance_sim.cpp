// Balance simulator v1 smoke test: every class solo-kills even-level mobs at a few levels with its
// content-only kit, and no class is off by an extreme factor. The full table (and the outliers it
// flags) comes from tools/balance_sim; see docs/DECISIONS.md.
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
