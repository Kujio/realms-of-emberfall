// Pure game rules: derived stats, consider colors, the XP curve, death penalty, combat math,
// and a Monte Carlo sanity check that a Warrior beats an even-con mob across the level range.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "game/rules.hpp"

using namespace ef::game;
using Catch::Approx;

namespace {

const Content& content() {
  static const Content c = load_content(EF_CONTENT_DIR);
  return c;
}

const Rules& R() { return content().rules; }

}  // namespace

TEST_CASE("rng is deterministic per seed and stays in range", "[rules]") {
  Rng a(42), b(42), c(43);
  bool differs = false;
  for (int i = 0; i < 1000; ++i) {
    const auto va = a.next();
    CHECK(va == b.next());
    differs |= va != c.next();
    const int r = a.range(-3, 7);
    CHECK(r >= -3);
    CHECK(r <= 7);
    const double u = a.uniform();
    CHECK(u >= 0.0);
    CHECK(u < 1.0);
    (void)b.range(-3, 7);
    (void)b.uniform();
  }
  CHECK(differs);
  CHECK(a.range(5, 5) == 5);
  CHECK(a.range(9, 2) == 9);
}

TEST_CASE("derived stats grow with level and respect race and class", "[rules]") {
  const RaceDef& highlander = *content().find_race("highlander");
  const RaceDef& deepkin = *content().find_race("deepkin");
  const ClassDef& warrior = *content().find_class("warrior");
  const ClassDef& wizard = *content().find_class("wizard");
  const auto w1 = derive_stats(highlander, warrior, 1);
  const auto w60 = derive_stats(highlander, warrior, 60);
  CHECK(w1.max_hp > 0);
  CHECK(w60.max_hp > w1.max_hp * 20);
  CHECK(w60.ac > w1.ac);
  CHECK(w1.max_mana == 0);  // warriors have no mana
  CHECK(w1.stats.str == highlander.stats.str + warrior.combat.stats.str);
  const auto z60 = derive_stats(highlander, wizard, 60);
  CHECK(z60.max_mana > 0);
  CHECK(z60.max_hp < w60.max_hp);  // casters are squishier
  // Stout races get a little more health from stamina.
  CHECK(derive_stats(deepkin, warrior, 30).max_hp > derive_stats(highlander, warrior, 30).max_hp);
}

TEST_CASE("skill caps unlock by level and stop at the class maximum", "[rules]") {
  const SkillCap dodge{6, 4.0f, 140};
  CHECK(skill_cap(dodge, 5) == 0);
  CHECK(skill_cap(dodge, 6) == 29);
  CHECK(skill_cap(dodge, 60) == 140);
  const auto& cr = R().combat;
  CHECK(skill_up_chance(10, 10, cr) == 0.0f);
  CHECK(skill_up_chance(10, 0, cr) == 0.0f);
  CHECK(skill_up_chance(1, 100, cr) > skill_up_chance(90, 100, cr));
}

TEST_CASE("consider colors follow the level difference", "[rules][con]") {
  const auto& cr = R().con;
  CHECK(con_color(10, 13, cr) == Con::Red);
  CHECK(con_color(10, 11, cr) == Con::Yellow);
  CHECK(con_color(10, 12, cr) == Con::Yellow);
  CHECK(con_color(10, 10, cr) == Con::White);
  CHECK(con_color(10, 9, cr) == Con::Blue);
  CHECK(con_color(10, 8, cr) == Con::Blue);
  CHECK(con_color(10, 7, cr) == Con::Green);
  CHECK(con_color(10, 6, cr) == Con::Green);
  CHECK(con_color(10, 5, cr) == Con::Grey);
  CHECK(con_color(1, 1, cr) == Con::White);
  CHECK(con_color(1, 60, cr) == Con::Red);
  // The blue and green bands widen at high level, so more of the world stays worth XP.
  CHECK(con_color(60, 51, cr) == Con::Blue);
  CHECK(con_color(60, 50, cr) == Con::Green);
  CHECK(con_color(60, 42, cr) == Con::Green);
  CHECK(con_color(60, 35, cr) == Con::Grey);
  // Monotonic: a higher mob never cons lower.
  for (int p = 1; p <= 60; ++p)
    for (int m = 1; m < 60; ++m) CHECK(static_cast<int>(con_color(p, m, cr)) <= static_cast<int>(con_color(p, m + 1, cr)));
}

TEST_CASE("XP curve: monotonic, exponential-ish, and 50-60 takes about as long as 1-50", "[rules][xp]") {
  const auto& x = R().xp;
  for (int l = 1; l < 60; ++l) CHECK(xp_to_next(l + 1, x) > xp_to_next(l, x));
  CHECK(total_xp_for_level(1, x) == 0);
  CHECK(total_xp_for_level(2, x) == xp_to_next(1, x));
  for (int l = 1; l <= 60; ++l) {
    CHECK(level_for_xp(total_xp_for_level(l, x), 60, x) == l);
    if (l > 1) CHECK(level_for_xp(total_xp_for_level(l, x) - 1, 60, x) == l - 1);
  }
  CHECK(level_for_xp(UINT64_MAX / 2, 60, x) == 60);  // capped at max level

  // Time per level ~ kills per level (mob HP scales with player damage, so a kill takes roughly
  // as long at every level). Compare the time for 50->60 with 1->50.
  auto kills = [&](int level) { return static_cast<double>(xp_to_next(level, x)) / static_cast<double>(kill_xp(level, level, x, R().con)); };
  double early = 0.0, late = 0.0;
  for (int l = 1; l < 50; ++l) early += kills(l);
  for (int l = 50; l < 60; ++l) late += kills(l);
  INFO("even-con kills 1-50: " << early << ", 50-60: " << late);
  CHECK(late / early > 0.8);
  CHECK(late / early < 1.25);
  CHECK(kills(1) >= 3.0);   // level 2 is not instant...
  CHECK(kills(1) <= 15.0);  // ...but comes quickly
}

TEST_CASE("level 60 is reachable by killing level-appropriate mobs", "[rules][xp]") {
  // Fight the best-XP mob a player would sensibly pick (even con, or yellow when available) and
  // count kills to max level; also checks nothing overflows or stalls along the way.
  const auto& x = R().xp;
  std::uint64_t xp = 0;
  int level = 1;
  long kills = 0;
  while (level < 60 && kills < 10'000'000) {
    const std::uint64_t gain = kill_xp(level, std::min(60, level + 1), x, R().con);
    REQUIRE(gain > 0);
    xp += gain;
    ++kills;
    level = level_for_xp(xp, 60, x);
  }
  INFO("kills to 60: " << kills);
  CHECK(level == 60);
  CHECK(kills > 1000);    // it is a journey
  CHECK(kills < 50'000);  // but a finite one
}

TEST_CASE("kill XP depends on con; grey gives nothing; groups split with a bonus", "[rules][xp]") {
  const auto& x = R().xp;
  const auto& c = R().con;
  CHECK(kill_xp(20, 10, x, c) == 0);
  CHECK(kill_xp(20, 22, x, c) > kill_xp(20, 20, x, c));
  CHECK(kill_xp(20, 20, x, c) > kill_xp(20, 18, x, c));
  const auto solo = split_group_xp(1000, {20}, x);
  REQUIRE(solo.size() == 1);
  CHECK(solo[0] == 1000);
  const auto pair = split_group_xp(1000, {20, 20}, x);
  REQUIRE(pair.size() == 2);
  CHECK(pair[0] + pair[1] == 1100);  // 10% bonus per extra member
  const auto mixed = split_group_xp(1000, {30, 10}, x);
  REQUIRE(mixed.size() == 2);
  CHECK(mixed[0] > mixed[1]);  // shares follow level
  CHECK(split_group_xp(0, {5}, x)[0] == 0);
  CHECK(split_group_xp(10, {}, x).empty());
}

TEST_CASE("death costs a slice of the current level, never below zero", "[rules][xp]") {
  const auto& d = R().death;
  const auto& x = R().xp;
  CHECK(death_xp_loss(500, d.min_level_for_xp_loss - 1, d, x) == 0);
  const int L = 20;
  const std::uint64_t expected = static_cast<std::uint64_t>(std::floor(static_cast<double>(xp_to_next(L, x)) * static_cast<double>(d.xp_loss_fraction)));
  CHECK(death_xp_loss(total_xp_for_level(L, x) + 10, L, d, x) == expected);
  CHECK(death_xp_loss(3, L, d, x) == 3);
}

TEST_CASE("hit chance, mitigation, and avoidance stay bounded and move the right way", "[rules][combat]") {
  const auto& r = R().combat;
  CHECK(hit_chance(0, 10000, 1, 60, r) == Approx(r.min_hit_chance));
  CHECK(hit_chance(10000, 0, 60, 1, r) == Approx(r.max_hit_chance));
  CHECK(hit_chance(100, 50, 20, 20, r) > hit_chance(50, 100, 20, 20, r));
  CHECK(hit_chance(100, 100, 22, 20, r) > hit_chance(100, 100, 20, 22, r));
  CHECK(mitigation(0, 10, r) == 0.0f);
  CHECK(mitigation(100000, 1, r) == Approx(r.max_mitigation));
  CHECK(mitigation(200, 10, r) > mitigation(100, 10, r));
  CHECK(mitigation(200, 50, r) < mitigation(200, 10, r));  // stronger attackers punch through armor
  CHECK(avoid_chance(0, 10, r) == 0.0f);
  CHECK(avoid_chance(100000, 1, r) == Approx(r.max_avoid_chance));
  CHECK(riposte_chance(100, 10, r) < avoid_chance(100, 10, r));
  CHECK(double_attack_chance(0, r) == 0.0f);
  CHECK(double_attack_chance(200, r) > 0.3f);
  CHECK(mitigate(100, 0, 10, 1.0f, r) == 100);
  CHECK(mitigate(100, 0, 10, 0.6f, r) == 60);  // defensive discipline
  CHECK(mitigate(1, 100000, 1, 1.0f, r) == 1);  // a landed hit always does something
}

TEST_CASE("stuns and taunts respect levels", "[rules][combat]") {
  const auto& r = R().combat;
  CHECK(stun_lands(20, 22, false, r));
  CHECK_FALSE(stun_lands(20, 23, false, r));
  CHECK_FALSE(stun_lands(60, 1, true, r));
  CHECK(taunt_chance(100, 100, 20, 20, r) > taunt_chance(1, 100, 20, 20, r));
  CHECK(taunt_chance(100, 100, 20, 30, r) < taunt_chance(100, 100, 20, 20, r));
  CHECK(taunt_chance(0, 0, 1, 60, r) >= 0.05f);
}

TEST_CASE("resolve_swing outcomes match their probabilities", "[rules][combat]") {
  const auto& r = R().combat;
  Rng rng(7);
  AttackerView a{20, 100, {10, 30}, 0.1f, 1.0f};
  DefenderView d{20, 100, 0, 100, 100, 100, 1.0f};
  int miss = 0, hit = 0, crit = 0, avoid = 0;
  long dmg = 0;
  const int n = 200000;
  for (int i = 0; i < n; ++i) {
    const auto o = resolve_swing(a, d, r, rng);
    switch (o.result) {
      case SwingResult::Miss: ++miss; CHECK(o.damage == 0); break;
      case SwingResult::Hit: ++hit; dmg += o.damage; CHECK(o.damage >= 10); CHECK(o.damage <= 30); break;
      case SwingResult::Crit: ++crit; CHECK(o.damage >= 20); break;
      default: ++avoid; CHECK(o.damage == 0); break;
    }
  }
  const double p_hit = static_cast<double>(hit_chance(100, 100, 20, 20, r));
  CHECK(static_cast<double>(miss) / n == Approx(1.0 - p_hit).margin(0.01));
  CHECK(static_cast<double>(crit) / (hit + crit) == Approx(0.1).margin(0.01));
  CHECK(avoid > 0);
  CHECK(static_cast<double>(dmg) / hit == Approx(20.0).margin(0.5));
}

TEST_CASE("mob stats scale with level", "[rules][combat]") {
  const auto& s = R().mob_scaling;
  MobDef weak, strong;
  weak.level = 1;
  strong.level = 60;
  const auto a = mob_stats(weak, s);
  const auto b = mob_stats(strong, s);
  CHECK(a.max_hp >= 1);
  CHECK(b.max_hp > a.max_hp * 20);
  CHECK(b.hit.max > a.hit.max);
  CHECK(a.hit.min <= a.hit.max);
  strong.hp_mult = 2.0f;
  CHECK(mob_stats(strong, s).max_hp == Approx(b.max_hp * 2).margin(2));
}

TEST_CASE("regen is faster out of combat and faster again sitting", "[rules]") {
  const auto& r = R().combat.regen;
  CHECK(hp_regen(10, true, false, r) < hp_regen(10, false, false, r));
  CHECK(hp_regen(10, false, false, r) < hp_regen(10, false, true, r));
  CHECK(hp_regen(10, true, true, r) == hp_regen(10, true, false, r));  // no sitting bonus mid-fight
  CHECK(endurance_regen(60, false, false, r) > endurance_regen(1, false, false, r));
}

// Balance sanity: a fresh Warrior (skills at cap, no gear) against an even-con mob of the generic
// archetype, auto-attack only. Wins most fights at every level, and fights take seconds, not minutes.
TEST_CASE("a Warrior usually beats an even-con mob at every level", "[rules][balance]") {
  const auto& r = R().combat;
  const RaceDef& race = *content().find_race("highlander");
  const ClassDef& cls = *content().find_class("warrior");
  Rng rng(1234);
  for (int level : {1, 5, 10, 20, 30, 40, 50, 60}) {
    const DerivedStats ds = derive_stats(race, cls, level);
    MobDef md;
    md.level = level;
    const MobStats ms = mob_stats(md, R().mob_scaling);
    auto cap = [&](const char* s) {
      const SkillCap* c = cls.skill(s);
      return c ? skill_cap(*c, level) : 0;
    };
    const AttackerView pa{level, cap("offense"), player_hit_range(ds, level), ds.crit_chance, 1.0f};
    const DefenderView md_def{level, ms.defense, ms.ac, ms.avoid_skill, ms.avoid_skill, ms.avoid_skill / 2, 1.0f};
    const AttackerView ma{level, ms.offense, ms.hit, 0.0f, 1.0f};
    const DefenderView pd{level, cap("defense"), ds.ac, cap("dodge"), cap("parry"), cap("riposte"), 1.0f};
    const double da = static_cast<double>(double_attack_chance(cap("double_attack"), r));
    int wins = 0;
    double seconds = 0.0, hp_left = 0.0;
    const int fights = 400;
    for (int f = 0; f < fights; ++f) {
      int php = ds.max_hp, mhp = ms.max_hp;
      double t = 0.0, next_p = 0.0, next_m = 0.5;
      while (php > 0 && mhp > 0 && t < 600.0) {
        if (next_p <= next_m) {
          t = next_p;
          const int swings = rng.chance(da) ? 2 : 1;
          for (int s = 0; s < swings && mhp > 0; ++s) mhp -= resolve_swing(pa, md_def, r, rng).damage;
          next_p += static_cast<double>(ds.weapon_delay);
        } else {
          t = next_m;
          php -= resolve_swing(ma, pd, r, rng).damage;
          next_m += 2.5;
        }
      }
      if (mhp <= 0 && php > 0) {
        ++wins;
        hp_left += static_cast<double>(php) / ds.max_hp;
      }
      seconds += t;
    }
    const double win_rate = static_cast<double>(wins) / fights;
    const double avg_s = seconds / fights;
    INFO("level " << level << ": win " << win_rate << ", " << avg_s << " s, hp left " << (wins ? hp_left / wins : 0.0));
    CHECK(win_rate >= 0.8);
    CHECK(avg_s >= 5.0);
    CHECK(avg_s <= 90.0);
  }
}
