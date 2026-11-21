// Pure spell-engine rules: resists, concentration, the cast state machine, stacking, dispels,
// line of sight, gems, and effect magnitudes. No zone or entities.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/spells.hpp"

using namespace ef::game;
using Catch::Approx;

namespace {

SpellRules rules() { return SpellRules{}; }

ActiveBuff buff(const char* group, int rank, bool beneficial, EntityId caster = 1, int periodic = 0) {
  ActiveBuff b;
  b.ability_id = group;
  b.name = group;
  b.stack_group = group;
  b.rank = rank;
  b.beneficial = beneficial;
  b.caster = caster;
  if (periodic != 0) b.periodics.push_back({periodic, 60, 60, kNever});
  b.mods.push_back({BuffStat::Ac, 1.0f});
  return b;
}

AbilityDef dot_spell() {
  AbilityDef a;
  a.id = "test_dot";
  a.name = "Test Dot";
  a.stack_group = "test_dot";
  a.spell = true;
  a.target = TargetType::Enemy;
  a.resist = ResistType::Fire;
  EffectDef e;
  e.type = EffectType::Dot;
  e.base = 10;
  e.duration = 12;
  e.tick = 3;
  a.effects.push_back(e);
  EffectDef s;
  s.type = EffectType::Snare;
  s.value = 40;
  s.duration = 6;
  s.stat = BuffStat::MoveSpeedPct;
  a.effects.push_back(s);
  return a;
}

}  // namespace

TEST_CASE("resist chance: none never resists; higher resist and higher targets resist more; bounded", "[spells][resist]") {
  const auto r = rules();
  CHECK(resist_chance(ResistType::None, 500, 10, 60, r) == 0.0f);
  const float base = resist_chance(ResistType::Fire, 20, 30, 30, r);
  CHECK(base > r.min_resist);
  CHECK(base < 0.2f);
  CHECK(resist_chance(ResistType::Fire, 200, 30, 30, r) > base);
  CHECK(resist_chance(ResistType::Fire, 20, 30, 35, r) == Approx(base + 5 * r.resist_per_level_diff));
  CHECK(resist_chance(ResistType::Fire, 20, 30, 20, r) < base);
  // Higher-level casters punch through the same resist value more often.
  CHECK(resist_chance(ResistType::Cold, 50, 50, 50, r) < resist_chance(ResistType::Cold, 50, 10, 10, r));
  CHECK(resist_chance(ResistType::Magic, 1000, 1, 60, r) == Approx(r.max_resist));
  CHECK(resist_chance(ResistType::Magic, 0, 60, 1, r) == Approx(r.min_resist));
}

TEST_CASE("resist rolls match their chance; partial resists keep a bounded fraction", "[spells][resist]") {
  const auto r = rules();
  Rng rng(7);
  int resisted = 0;
  int partial = 0;
  const int n = 40000;
  for (int i = 0; i < n; ++i) {
    const ResistRoll roll = roll_resist(0.3f, true, r, rng);
    if (roll.resisted) {
      ++resisted;
      CHECK(roll.fraction == 0.0f);
    } else if (roll.fraction < 1.0f) {
      ++partial;
      CHECK(roll.fraction >= r.partial_resist_min);
    }
  }
  CHECK(static_cast<double>(resisted) / n == Approx(0.3).margin(0.01));
  CHECK(static_cast<double>(partial) / n == Approx(0.7 * 0.3).margin(0.01));
  // Without partials a landed spell is always whole; chance 0 never resists.
  for (int i = 0; i < 1000; ++i) {
    const auto a = roll_resist(0.5f, false, r, rng);
    CHECK((a.resisted || a.fraction == 1.0f));
    CHECK_FALSE(roll_resist(0.0f, true, r, rng).resisted);
  }
}

TEST_CASE("mob resists default by level, honour per-mob values, and add debuffs", "[spells][resist]") {
  MobDef m;
  m.level = 40;
  MobScaling s;
  CHECK(base_mob_resist(m, ResistType::Fire, s) == static_cast<int>(s.resist_base + s.resist_per_level * 40));
  CHECK(base_mob_resist(m, ResistType::None, s) == 0);
  m.resists[static_cast<std::size_t>(ResistType::Fire)] = 150;
  CHECK(base_mob_resist(m, ResistType::Fire, s) == 150);
  std::vector<ActiveBuff> buffs{buff("frailty", 1, false)};
  buffs[0].mods = {{BuffStat::ResistAll, -20}, {BuffStat::ResistFire, -5}};
  CHECK(resist_bonus(buffs, ResistType::Fire) == -25);
  CHECK(resist_bonus(buffs, ResistType::Cold) == -20);
  CHECK(resist_bonus(buffs, ResistType::None) == 0);
}

TEST_CASE("concentration: channeling helps, big hits hurt, always bounded", "[spells][cast]") {
  const auto r = rules();
  const float novice = concentration_chance(0, 10, 100, r);
  const float adept = concentration_chance(200, 10, 100, r);
  CHECK(adept > novice);
  CHECK(concentration_chance(50, 50, 100, r) < concentration_chance(50, 5, 100, r));
  CHECK(concentration_chance(0, 100000, 100, r) == Approx(r.min_concentration));
  CHECK(concentration_chance(100000, 0, 100, r) <= r.max_concentration);
  CHECK(concentration_chance(0, 0, 100, r) == Approx(r.concentration_base));
}

TEST_CASE("cast state machine: idle -> casting -> complete; moving interrupts", "[spells][cast]") {
  AbilityDef a;
  a.id = "bolt";
  CastState c;
  CHECK_FALSE(c.casting());
  CHECK_FALSE(cast_complete(c, 100));
  c = begin_cast(a, 42, 1, 100, 40, {0, 0, 0});
  CHECK(c.casting());
  CHECK(c.ability == &a);
  CHECK(c.target == 42);
  CHECK(c.gem == 1);
  CHECK_FALSE(cast_complete(c, 139));
  CHECK(cast_complete(c, 140));
  CHECK_FALSE(moved_while_casting(c, {0.3f, 0, 0.3f}, 0.5f));
  CHECK(moved_while_casting(c, {0.5f, 0, 0.5f}, 0.5f));
  end_cast(c);
  CHECK_FALSE(c.casting());
  CHECK(c.ability == nullptr);
  CHECK_FALSE(moved_while_casting(c, {50, 0, 50}, 0.5f));
  // An instant cast (0 ticks) completes on its first check.
  c = begin_cast(a, 0, -1, 7, 0, {});
  CHECK(cast_complete(c, 7));
}

TEST_CASE("stacking: ranks overwrite, lower ranks are blocked, DoTs stack per caster, slots fill up", "[spells][stacking]") {
  const auto r = rules();
  std::vector<ActiveBuff> host;
  CHECK(decide_stacking(host, buff("ward", 2, true), 15, 10).action == StackAction::Add);
  host.push_back(buff("ward", 2, true));
  CHECK(decide_stacking(host, buff("ward", 3, true), 15, 10).action == StackAction::Replace);
  CHECK(decide_stacking(host, buff("ward", 2, true), 15, 10).action == StackAction::Replace);  // refresh
  const auto blocked = decide_stacking(host, buff("ward", 1, true), 15, 10);
  CHECK(blocked.action == StackAction::Blocked);
  CHECK(blocked.index == 0);
  // A buff from another caster in the same group still replaces (one ward per target).
  CHECK(decide_stacking(host, buff("ward", 2, true, 99), 15, 10).action == StackAction::Replace);
  // Detrimental: a DoT from a second caster stacks beside the first; the same caster refreshes.
  std::vector<ActiveBuff> mob{buff("brand", 1, false, 1, -10)};
  CHECK(decide_stacking(mob, buff("brand", 1, false, 2, -10), 15, 10).action == StackAction::Add);
  CHECK(decide_stacking(mob, buff("brand", 1, false, 1, -10), 15, 10).action == StackAction::Replace);
  // Non-periodic detrimental effects (snares, roots) overwrite whoever cast them.
  std::vector<ActiveBuff> snared{buff("snare", 1, false, 1)};
  CHECK(decide_stacking(snared, buff("snare", 1, false, 2), 15, 10).action == StackAction::Replace);
  // A buff and a debuff in the same group are different lines.
  CHECK(decide_stacking(host, buff("ward", 1, false), 15, 10).action == StackAction::Add);
  // Slots: 15 beneficial and 10 detrimental, counted separately.
  std::vector<ActiveBuff> full;
  for (int i = 0; i < r.buff_slots; ++i) full.push_back(buff(("b" + std::to_string(i)).c_str(), 1, true));
  CHECK(decide_stacking(full, buff("one_more", 1, true), r.buff_slots, r.debuff_slots).action == StackAction::NoSlot);
  CHECK(decide_stacking(full, buff("b3", 2, true), r.buff_slots, r.debuff_slots).action == StackAction::Replace);
  CHECK(decide_stacking(full, buff("curse", 1, false), r.buff_slots, r.debuff_slots).action == StackAction::Add);
  for (int i = 0; i < r.debuff_slots; ++i) full.push_back(buff(("d" + std::to_string(i)).c_str(), 1, false));
  CHECK(decide_stacking(full, buff("curse", 1, false), r.buff_slots, r.debuff_slots).action == StackAction::NoSlot);
}

TEST_CASE("dispels take the newest effects of one polarity", "[spells][stacking]") {
  std::vector<ActiveBuff> b{buff("a", 1, true), buff("x", 1, false), buff("b", 1, true), buff("y", 1, false), buff("z", 1, false)};
  const auto bad = dispel_targets(b, false, 2);
  REQUIRE(bad.size() == 2);
  CHECK(bad[0] == 4);
  CHECK(bad[1] == 3);
  const auto good = dispel_targets(b, true, 5);
  REQUIRE(good.size() == 2);
  CHECK(good[0] == 2);
  CHECK(good[1] == 0);
  CHECK(dispel_targets({}, true, 3).empty());
}

TEST_CASE("build_buff collects every lasting line of a spell into one effect", "[spells]") {
  const auto r = rules();
  const AbilityDef a = dot_spell();
  const ActiveBuff b = build_buff(a, 20, 5, 77, "Caster", 100, 20.0f, r);
  CHECK(b.ability_id == "test_dot");
  CHECK_FALSE(b.beneficial);
  CHECK(b.caster == 5);
  CHECK(b.caster_char == 77);
  REQUIRE(b.periodics.size() == 1);
  CHECK(b.periodics[0].amount == -10);
  CHECK(b.periodics[0].interval == 60);
  CHECK(b.periodics[0].next_tick == 160);
  CHECK(b.periodics[0].expires_tick == 100 + 12 * 20);
  CHECK(b.expires_tick == 100 + 12 * 20);  // the longest line decides when the slot frees
  REQUIRE(b.mods.size() == 1);
  CHECK(b.mods[0].stat == BuffStat::MoveSpeedPct);
  CHECK(b.mods[0].value == -40.0f);
  CHECK(b.mods[0].expires_tick == 100 + 6 * 20);  // the snare has its own, shorter duration
  CHECK(b.harms_over_time());
  // Lines end on their own: at 6 s the snare goes and the DoT stays; at 12 s nothing is left.
  ActiveBuff c = b;
  CHECK_FALSE(prune_lines(c, 219));
  CHECK(prune_lines(c, 220));
  CHECK(c.mods.empty());
  CHECK(c.periodics.size() == 1);
  CHECK(prune_lines(c, 340));
  CHECK_FALSE(has_lasting_part(c));
  CHECK(has_lasting_part(b));
  CHECK(move_speed_multiplier({b}) == Approx(0.6f));
  AbilityDef instant;
  instant.effects.push_back(EffectDef{});  // damage only
  CHECK_FALSE(has_lasting_part(build_buff(instant, 1, 0, 0, "", 0, 20.0f, r)));
}

TEST_CASE("control bits, slows, and hastes", "[spells]") {
  ActiveBuff root = buff("root", 1, false);
  root.controls.push_back({kControlRoot, kNever});
  ActiveBuff mez = buff("mez", 1, false);
  mez.controls.push_back({kControlMez, kNever});
  CHECK(control_bits({root, mez}) == (kControlRoot | kControlMez));
  CHECK(control_bits({}) == 0);
  // Breaking a root strips only that line: a DoT or a debuff in the same spell stays.
  ActiveBuff bind = buff("bind", 1, false, 1, -5);
  bind.controls.push_back({kControlRoot, kNever});
  CHECK(strip_control(bind, kControlRoot));
  CHECK(bind.control() == 0);
  CHECK(has_lasting_part(bind));
  CHECK_FALSE(strip_control(bind, kControlRoot));
  ActiveBuff slow = buff("slow", 1, false);
  slow.mods = {{BuffStat::AttackSpeedPct, -50}};
  CHECK(attack_delay_multiplier({slow}) == Approx(2.0f));
  ActiveBuff haste = buff("haste", 1, true);
  haste.mods = {{BuffStat::AttackSpeedPct, 25}};
  CHECK(attack_delay_multiplier({haste}) == Approx(0.8f));
  ActiveBuff snare = buff("snare", 1, false);
  snare.mods = {{BuffStat::MoveSpeedPct, -95}};
  CHECK(move_speed_multiplier({snare}) == Approx(0.1f));  // clamped: never fully stopped by a snare
}

TEST_CASE("effect magnitudes scale with level and stop at the cap", "[spells]") {
  EffectDef e;
  e.base = 10;
  e.per_level = 2;
  CHECK(effect_amount(e, 1) == 12);
  CHECK(effect_amount(e, 20) == 50);
  e.max = 30;
  CHECK(effect_amount(e, 20) == 30);
  e.base = -100;
  CHECK(effect_amount(e, 1) == 0);
  Rng rng(3);
  for (int i = 0; i < 200; ++i) {
    const int v = roll_spread(100, 0.1f, rng);
    CHECK(v >= 90);
    CHECK(v <= 110);
  }
  CHECK(roll_spread(0, 0.1f, rng) == 0);
}

TEST_CASE("gems unlock by level; line of sight is blocked by obstacles", "[spells]") {
  const auto r = rules();
  CHECK(gem_count(1, r) == 2);
  CHECK(gem_count(4, r) == 3);
  CHECK(gem_count(44, r) == 7);
  CHECK(gem_count(60, r) == 8);
  const std::vector<Obstacle> obs{{10, 0, 2}};
  CHECK_FALSE(line_of_sight({0, 0, 0}, {20, 0, 0}, obs));
  CHECK(line_of_sight({0, 0, 5}, {20, 0, 5}, obs));
  CHECK(line_of_sight({0, 0, 0}, {5, 0, 0}, obs));
  CHECK(line_of_sight({9, 0, 0}, {30, 0, 0}, obs));  // standing inside the pillar's edge
  CHECK(line_of_sight({0, 0, 0}, {20, 0, 0}, {}));
}

TEST_CASE("resurrection XP, buff threat, and meditation", "[spells]") {
  const auto r = rules();
  CHECK(rez_xp(1000, 50) == 500);
  CHECK(rez_xp(1000, 96) == 960);
  CHECK(rez_xp(1000, 150) == 1000);
  CHECK(rez_xp(0, 96) == 0);
  CHECK(buff_threat(30, r) == Approx(60.0));
  CHECK(meditate_multiplier(0, 100, r) == 1.0f);
  CHECK(meditate_multiplier(100, 100, r) == Approx(1.0f + r.meditate_bonus));
  CHECK(meditate_multiplier(50, 100, r) == Approx(1.0f + r.meditate_bonus / 2));
}
