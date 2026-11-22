// emberfall-balance-sim: every class solo-kills even-level mobs across the level range; prints
// time-to-kill, survivability, and mana efficiency, then flags outliers. With --progression it
// instead measures the time to level from 1 to the cap, solo and in a standard six-person group,
// and reports hours per level band.
//
//   emberfall-balance-sim [--content DIR] [--fights N] [--levels 5,10,...] [--classes a,b] [--seed N]
//                         [--ttk-factor X] [--min-win R] [--strict] [--gearless]
//   emberfall-balance-sim --progression [--mode solo|group|both] [--step N] [--fights N] [--classes a,b]
//                         [--factor X] [--max-death R] [--detail] [--strict] [--gearless]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

#include "balance_sim.hpp"

namespace {

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, ','))
    if (!part.empty()) out.push_back(part);
  return out;
}

int run_progression(const ef::game::Content& content, const std::vector<std::string>& classes, const ef::sim::ProgressionOptions& opt,
                    const std::string& mode, double factor, double max_death, bool detail, bool strict) {
  std::vector<ef::sim::Progression> rows;
  for (const bool group : {false, true}) {
    if ((group && mode == "solo") || (!group && mode == "group")) continue;
    for (const auto& c : classes) {
      rows.push_back(ef::sim::simulate_progression(content, c, group, opt));
      std::fprintf(stderr, "simulated %s %s\n", c.c_str(), group ? "grouped" : "solo");
    }
  }
  std::printf("Progression: hours from level 1 to %d, %d pulls per sampled level, every %d level(s), %s.\n\n", content.rules.max_level, opt.fights,
              opt.step, opt.gear ? "recommended gear" : "no gear");
  std::printf("%s", ef::sim::format_progression(rows, detail).c_str());
  const auto outliers = ef::sim::progression_outliers(rows, factor, max_death);
  if (outliers.empty()) {
    std::printf("No outliers (time to the cap within %.2fx of the median, deaths in at most %.0f%% of pulls).\n", factor, max_death * 100.0);
  } else {
    std::printf("Outliers (time to the cap beyond %.2fx of the median, or deaths in more than %.0f%% of pulls):\n", factor, max_death * 100.0);
    for (const auto& o : outliers) std::printf("  %-14s %s\n", o.class_id.c_str(), o.why.c_str());
  }
  return strict && !outliers.empty() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = EF_DEFAULT_CONTENT_DIR;
  ef::sim::SimOptions opt;
  ef::sim::ProgressionOptions popt;
  std::vector<int> levels{1, 5, 10, 15, 20, 30, 40, 50, 60};
  std::vector<std::string> classes;
  double ttk_factor = 3.0;
  double min_win = 0.5;
  double factor = 1.5;
  double max_death = 0.15;
  bool strict = false;
  bool progression = false;
  bool detail = false;
  bool fights_set = false;
  std::string mode = "both";
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
        return argv[++i];
      };
      if (a == "--content") dir = next();
      else if (a == "--fights") {
        opt.fights = popt.fights = std::stoi(next());
        fights_set = true;
      } else if (a == "--seed") opt.seed = popt.seed = std::stoull(next());
      else if (a == "--ttk-factor") ttk_factor = std::stod(next());
      else if (a == "--min-win") min_win = std::stod(next());
      else if (a == "--factor") factor = std::stod(next());
      else if (a == "--max-death") max_death = std::stod(next());
      else if (a == "--strict") strict = true;
      else if (a == "--gearless") opt.gear = popt.gear = false;
      else if (a == "--progression") progression = true;
      else if (a == "--detail") detail = true;
      else if (a == "--step") popt.step = std::stoi(next());
      else if (a == "--mode") mode = next();
      else if (a == "--levels") {
        levels.clear();
        for (const auto& l : split(next())) levels.push_back(std::stoi(l));
      } else if (a == "--classes") classes = split(next());
      else if (a == "--help" || a == "-h") {
        std::printf("usage: emberfall-balance-sim [--content DIR] [--fights N] [--levels 5,10] [--classes a,b] [--seed N] "
                    "[--ttk-factor X] [--min-win R] [--strict] [--gearless]\n"
                    "       emberfall-balance-sim --progression [--mode solo|group|both] [--step N] [--fights N] [--classes a,b] "
                    "[--factor X] [--max-death R] [--detail] [--strict] [--gearless]\n");
        return 0;
      } else {
        throw std::runtime_error("unknown option " + a);
      }
    }
    if (opt.fights < 1 || opt.fights > 1000) throw std::runtime_error("--fights must be 1..1000");
    if (popt.step < 1 || popt.step > 59) throw std::runtime_error("--step must be 1..59");
    if (mode != "solo" && mode != "group" && mode != "both") throw std::runtime_error("--mode must be solo, group or both");
    if (!std::isfinite(factor) || factor <= 1.0) throw std::runtime_error("--factor must be a number above 1");
    if (!std::isfinite(max_death) || max_death < 0.0 || max_death > 1.0) throw std::runtime_error("--max-death must be 0..1");
    if (levels.empty()) throw std::runtime_error("--levels needs at least one level");
    const ef::game::Content content = ef::game::load_content(dir);
    if (classes.empty())
      for (const auto& c : content.classes) classes.push_back(c.id);
    for (const auto& c : classes)
      if (!content.find_class(c)) throw std::runtime_error("unknown class " + c);
    if (progression) {
      if (!fights_set) popt.fights = 4;
      return run_progression(content, classes, popt, mode, factor, max_death, detail, strict);
    }
    std::vector<ef::sim::FightStats> rows;
    for (const auto& c : classes) {
      for (int l : levels) {
        if (l < 1 || l > content.rules.max_level) throw std::runtime_error("level out of range");
        rows.push_back(ef::sim::simulate(content, c, l, opt));
      }
      std::fprintf(stderr, "simulated %s\n", c.c_str());
    }
    std::printf("Balance simulator: each class alone against even-level mobs, %d fights per cell, %s.\n\n", opt.fights,
                opt.gear ? "recommended gear" : "no gear");
    std::printf("%s", ef::sim::format_table(rows, levels).c_str());
    const auto outliers = ef::sim::find_outliers(rows, ttk_factor, min_win);
    if (outliers.empty()) {
      std::printf("No outliers (time to kill within %.1fx of the median, win rate at least %.0f%%).\n", ttk_factor, min_win * 100.0);
    } else {
      std::printf("Outliers (time to kill beyond %.1fx of the median, or win rate below %.0f%%):\n", ttk_factor, min_win * 100.0);
      for (const auto& o : outliers) std::printf("  %-14s L%-3d %s\n", o.class_id.c_str(), o.level, o.why.c_str());
    }
    return strict && !outliers.empty() ? 1 : 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "emberfall-balance-sim: %s\n", e.what());
    return 2;
  }
}
