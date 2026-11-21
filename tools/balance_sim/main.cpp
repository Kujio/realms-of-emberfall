// emberfall-balance-sim: every class solo-kills even-level mobs across the level range; prints
// time-to-kill, survivability, and mana efficiency, then flags outliers.
//
//   emberfall-balance-sim [--content DIR] [--fights N] [--levels 5,10,...] [--classes a,b] [--seed N]
//                         [--ttk-factor X] [--min-win R] [--strict]
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

}  // namespace

int main(int argc, char** argv) {
  std::string dir = EF_DEFAULT_CONTENT_DIR;
  ef::sim::SimOptions opt;
  std::vector<int> levels{1, 5, 10, 15, 20, 30, 40, 50, 60};
  std::vector<std::string> classes;
  double ttk_factor = 3.0;
  double min_win = 0.5;
  bool strict = false;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
        return argv[++i];
      };
      if (a == "--content") dir = next();
      else if (a == "--fights") opt.fights = std::stoi(next());
      else if (a == "--seed") opt.seed = std::stoull(next());
      else if (a == "--ttk-factor") ttk_factor = std::stod(next());
      else if (a == "--min-win") min_win = std::stod(next());
      else if (a == "--strict") strict = true;
      else if (a == "--levels") {
        levels.clear();
        for (const auto& l : split(next())) levels.push_back(std::stoi(l));
      } else if (a == "--classes") classes = split(next());
      else if (a == "--help" || a == "-h") {
        std::printf("usage: emberfall-balance-sim [--content DIR] [--fights N] [--levels 5,10] [--classes a,b] [--seed N] "
                    "[--ttk-factor X] [--min-win R] [--strict]\n");
        return 0;
      } else {
        throw std::runtime_error("unknown option " + a);
      }
    }
    if (opt.fights < 1 || opt.fights > 1000) throw std::runtime_error("--fights must be 1..1000");
    if (levels.empty()) throw std::runtime_error("--levels needs at least one level");
    const ef::game::Content content = ef::game::load_content(dir);
    if (classes.empty())
      for (const auto& c : content.classes) classes.push_back(c.id);
    std::vector<ef::sim::FightStats> rows;
    for (const auto& c : classes) {
      if (!content.find_class(c)) throw std::runtime_error("unknown class " + c);
      for (int l : levels) {
        if (l < 1 || l > content.rules.max_level) throw std::runtime_error("level out of range");
        rows.push_back(ef::sim::simulate(content, c, l, opt));
      }
      std::fprintf(stderr, "simulated %s\n", c.c_str());
    }
    std::printf("Balance simulator v1: each class alone against even-level mobs, %d fights per cell, no gear.\n\n", opt.fights);
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
