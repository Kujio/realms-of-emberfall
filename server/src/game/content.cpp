#include "game/content.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace ef::game {

namespace {

using nlohmann::json;

template <class T>
const T* find_by_id(const std::vector<T>& v, std::string_view id) {
  auto it = std::find_if(v.begin(), v.end(), [&](const T& t) { return t.id == id; });
  return it == v.end() ? nullptr : &*it;
}

// Small helper that records errors with a path prefix instead of throwing on the first one.
class Parser {
 public:
  explicit Parser(std::vector<std::string>& errors) : errors_(errors) {}

  std::string str(const json& j, const char* key, const std::string& where, bool required = true, std::string fallback = {}) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_string()) {
      errors_.push_back(where + ": '" + key + "' must be a string");
      return fallback;
    }
    return j.at(key).get<std::string>();
  }

  float num(const json& j, const char* key, const std::string& where, float fallback, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_number()) {
      errors_.push_back(where + ": '" + key + "' must be a number");
      return fallback;
    }
    const double v = j.at(key).get<double>();
    if (!std::isfinite(v) || std::fabs(v) > 1e9) {
      errors_.push_back(where + ": '" + key + "' must be a finite number");
      return fallback;
    }
    return static_cast<float>(v);
  }

  // Optional number with a default (no error when absent).
  float opt(const json& j, const char* key, const std::string& where, float fallback) { return num(j, key, where, fallback, false); }

  double dbl(const json& j, const char* key, const std::string& where, double fallback) {
    return static_cast<double>(num(j, key, where, static_cast<float>(fallback), false));
  }

  int integer(const json& j, const char* key, const std::string& where, int fallback, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_number_integer()) {
      errors_.push_back(where + ": '" + key + "' must be an integer");
      return fallback;
    }
    const auto v = j.at(key).get<long long>();
    if (v < -1000000000LL || v > 1000000000LL) {
      errors_.push_back(where + ": '" + key + "' is out of range");
      return fallback;
    }
    return static_cast<int>(v);
  }

  bool boolean(const json& j, const char* key, const std::string& where, bool fallback, bool required = true) {
    if (!j.is_object() || !j.contains(key)) {
      if (required) errors_.push_back(where + ": missing '" + key + "'");
      return fallback;
    }
    if (!j.at(key).is_boolean()) {
      errors_.push_back(where + ": '" + key + "' must be true or false");
      return fallback;
    }
    return j.at(key).get<bool>();
  }

  std::vector<std::string> str_list(const json& j, const char* key, const std::string& where) {
    std::vector<std::string> out;
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_array()) {
      errors_.push_back(where + ": '" + key + "' must be an array of strings");
      return out;
    }
    for (const auto& v : j.at(key)) {
      if (!v.is_string()) {
        errors_.push_back(where + ": '" + key + "' must contain only strings");
        continue;
      }
      out.push_back(v.get<std::string>());
    }
    return out;
  }

  SpawnPoint spawn(const json& j, const char* key, const std::string& where) {
    SpawnPoint s;
    if (!j.is_object() || !j.contains(key) || !j.at(key).is_object()) {
      errors_.push_back(where + ": missing object '" + key + "'");
      return s;
    }
    const auto& o = j.at(key);
    const std::string w = where + "." + key;
    s.x = num(o, "x", w, 0.0f);
    s.y = num(o, "y", w, 0.0f);
    s.z = num(o, "z", w, 0.0f);
    s.yaw = num(o, "yaw", w, 0.0f);
    return s;
  }

  StatBlock stats(const json& j, const std::string& where, StatBlock fallback, bool all_required) {
    StatBlock s = fallback;
    if (!j.is_object()) {
      if (all_required) errors_.push_back(where + ": stats must be an object");
      return s;
    }
    s.str = integer(j, "str", where, s.str, all_required);
    s.sta = integer(j, "sta", where, s.sta, all_required);
    s.agi = integer(j, "agi", where, s.agi, all_required);
    s.dex = integer(j, "dex", where, s.dex, all_required);
    s.wis = integer(j, "wis", where, s.wis, all_required);
    s.intel = integer(j, "int", where, s.intel, all_required);
    s.cha = integer(j, "cha", where, s.cha, all_required);
    return s;
  }

  SkillCap skill_cap(const json& j, const std::string& where) {
    SkillCap c;
    if (!j.is_object()) {
      errors_.push_back(where + ": skill cap must be an object");
      return c;
    }
    c.level = integer(j, "level", where, 1);
    c.per_level = num(j, "per_level", where, 5.0f);
    c.max = integer(j, "max", where, 250);
    return c;
  }

  // A top-level array. A null document is an empty list (tests pass only what they need).
  const json* list(const json& doc, const char* key, const char* file) {
    if (doc.is_null()) return nullptr;
    if (!doc.is_object() || !doc.contains(key) || !doc.at(key).is_array()) {
      errors_.push_back(std::string(file) + ": top-level '" + key + "' array is required");
      return nullptr;
    }
    return &doc.at(key);
  }

  std::vector<std::string>& errors() { return errors_; }
  void error(std::string msg) { errors_.push_back(std::move(msg)); }

 private:
  std::vector<std::string>& errors_;
};

[[noreturn]] void throw_errors(const std::vector<std::string>& errors) {
  std::ostringstream os;
  os << "content validation failed (" << errors.size() << " problem" << (errors.size() == 1 ? "" : "s") << "):";
  for (const auto& e : errors) os << "\n  - " << e;
  throw ContentError(os.str());
}

// Joins strings with one allocation (for messages built inside loops).
std::string cat(std::initializer_list<std::string_view> parts) {
  std::size_t n = 0;
  for (const auto part : parts) n += part.size();
  std::string out;
  out.reserve(n);
  for (const auto part : parts) out += part;
  return out;
}

bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 32) return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

template <class T>
void check_ids(const std::vector<T>& v, const char* kind, std::vector<std::string>& errors) {
  std::set<std::string> seen;
  for (const auto& t : v) {
    if (!valid_id(t.id)) errors.push_back(std::string(kind) + " id '" + t.id + "' must be 1-32 chars of [a-z0-9_]");
    if (!seen.insert(t.id).second) errors.push_back(std::string("duplicate ") + kind + " id '" + t.id + "'");
    if constexpr (requires { t.name; }) {
      if (t.name.empty() || t.name.size() > 48) errors.push_back(std::string(kind) + " '" + t.id + "': name must be 1-48 chars");
    }
  }
}

json read_json(const std::filesystem::path& path, std::vector<std::string>& errors) {
  std::ifstream in(path);
  if (!in) {
    errors.push_back("cannot open " + path.string());
    return json::object();
  }
  try {
    return json::parse(in);
  } catch (const json::exception& e) {
    errors.push_back(path.string() + ": invalid JSON: " + e.what());
    return json::object();
  }
}

const json& section(const json& j, const char* key) {
  static const json kEmpty = json::object();
  return j.is_object() && j.contains(key) && j.at(key).is_object() ? j.at(key) : kEmpty;
}

void parse_rules(Parser& p, const json& rules, Rules& r) {
  r.max_level = p.integer(rules, "max_level", "rules", 60);
  r.max_characters_per_account = p.integer(rules, "max_characters_per_account", "rules", 4);
  r.starting_coin = p.integer(rules, "starting_coin", "rules", 0, false);
  if (rules.is_object() && rules.contains("movement")) {
    const auto& m = rules.at("movement");
    r.movement.run_speed = p.num(m, "run_speed", "rules.movement", 7.0f);
    r.movement.input_budget_max = static_cast<std::uint32_t>(std::max(1, p.integer(m, "input_budget_max", "rules.movement", 4)));
    r.movement.desync_tolerance = p.num(m, "desync_tolerance", "rules.movement", 2.0f);
    r.movement.teleport_tolerance = p.num(m, "teleport_tolerance", "rules.movement", 1.5f);
  } else {
    p.errors().emplace_back("rules: missing 'movement'");
  }

  // The remaining sections are optional in the parser (defaults match content/rules.json);
  // tools/validate_content.py requires them in the shipped files. Present but malformed is an error.
  for (const char* key : {"xp", "con", "combat", "death", "mob_scaling", "spells", "pets", "songs", "stealth", "content"})
    if (rules.is_object() && rules.contains(key) && !rules.at(key).is_object())
      p.errors().push_back(std::string("rules: '") + key + "' must be an object");
  for (const auto& [outer, inner] : {std::pair{"xp", "con_multipliers"}, std::pair{"combat", "regen"}})
    if (const auto& o = section(rules, outer); o.contains(inner) && !o.at(inner).is_object())
      p.errors().push_back(std::string("rules.") + outer + ": '" + inner + "' must be an object");
  const auto& xp = section(rules, "xp");
  r.xp.base = p.dbl(xp, "base", "rules.xp", r.xp.base);
  r.xp.level_exponent = p.dbl(xp, "level_exponent", "rules.xp", r.xp.level_exponent);
  r.xp.growth = p.dbl(xp, "growth", "rules.xp", r.xp.growth);
  r.xp.kill_base = p.dbl(xp, "kill_base", "rules.xp", r.xp.kill_base);
  r.xp.group_bonus_per_member = p.dbl(xp, "group_bonus_per_member", "rules.xp", r.xp.group_bonus_per_member);
  const auto& cm = section(xp, "con_multipliers");
  const char* con_names[kConCount] = {"grey", "green", "blue", "white", "yellow", "red"};
  for (std::size_t i = 0; i < kConCount; ++i) r.xp.con_multipliers[i] = p.dbl(cm, con_names[i], "rules.xp.con_multipliers", r.xp.con_multipliers[i]);

  const auto& con = section(rules, "con");
  r.con.blue_divisor = p.integer(con, "blue_divisor", "rules.con", r.con.blue_divisor, false);
  r.con.green_divisor = p.integer(con, "green_divisor", "rules.con", r.con.green_divisor, false);

  const auto& c = section(rules, "combat");
  const std::string w = "rules.combat";
  auto& cr = r.combat;
  cr.gcd_seconds = p.opt(c, "gcd_seconds", w, cr.gcd_seconds);
  cr.melee_range = p.opt(c, "melee_range", w, cr.melee_range);
  cr.loot_range = p.opt(c, "loot_range", w, cr.loot_range);
  cr.event_radius = p.opt(c, "event_radius", w, cr.event_radius);
  cr.in_combat_seconds = p.opt(c, "in_combat_seconds", w, cr.in_combat_seconds);
  cr.base_hit_chance = p.opt(c, "base_hit_chance", w, cr.base_hit_chance);
  cr.hit_per_skill_point = p.opt(c, "hit_per_skill_point", w, cr.hit_per_skill_point);
  cr.hit_per_level = p.opt(c, "hit_per_level", w, cr.hit_per_level);
  cr.min_hit_chance = p.opt(c, "min_hit_chance", w, cr.min_hit_chance);
  cr.max_hit_chance = p.opt(c, "max_hit_chance", w, cr.max_hit_chance);
  cr.crit_multiplier = p.opt(c, "crit_multiplier", w, cr.crit_multiplier);
  cr.mitigation_base = p.opt(c, "mitigation_base", w, cr.mitigation_base);
  cr.mitigation_per_level = p.opt(c, "mitigation_per_level", w, cr.mitigation_per_level);
  cr.max_mitigation = p.opt(c, "max_mitigation", w, cr.max_mitigation);
  cr.avoid_skill_divisor = p.opt(c, "avoid_skill_divisor", w, cr.avoid_skill_divisor);
  cr.avoid_level_scale = p.opt(c, "avoid_level_scale", w, cr.avoid_level_scale);
  cr.avoid_base = p.opt(c, "avoid_base", w, cr.avoid_base);
  cr.max_avoid_chance = p.opt(c, "max_avoid_chance", w, cr.max_avoid_chance);
  cr.riposte_scale = p.opt(c, "riposte_scale", w, cr.riposte_scale);
  cr.double_attack_divisor = p.opt(c, "double_attack_divisor", w, cr.double_attack_divisor);
  cr.stun_level_margin = p.integer(c, "stun_level_margin", w, cr.stun_level_margin, false);
  cr.taunt_base_chance = p.opt(c, "taunt_base_chance", w, cr.taunt_base_chance);
  cr.taunt_per_level = p.opt(c, "taunt_per_level", w, cr.taunt_per_level);
  cr.skill_up_base = p.opt(c, "skill_up_base", w, cr.skill_up_base);
  cr.skill_up_scale = p.opt(c, "skill_up_scale", w, cr.skill_up_scale);
  cr.behind_arc_degrees = p.opt(c, "behind_arc_degrees", w, cr.behind_arc_degrees);
  cr.offhand_damage_mult = p.opt(c, "offhand_damage_mult", w, cr.offhand_damage_mult);
  cr.dual_wield_divisor = p.opt(c, "dual_wield_divisor", w, cr.dual_wield_divisor);
  cr.max_dual_wield_chance = p.opt(c, "max_dual_wield_chance", w, cr.max_dual_wield_chance);
  cr.enhanced_damage_max = p.opt(c, "enhanced_damage_max", w, cr.enhanced_damage_max);
  const auto& rg = section(c, "regen");
  const std::string wr = "rules.combat.regen";
  cr.regen.hp_base = p.opt(rg, "hp_base", wr, cr.regen.hp_base);
  cr.regen.hp_per_level = p.opt(rg, "hp_per_level", wr, cr.regen.hp_per_level);
  cr.regen.endurance_base = p.opt(rg, "endurance_base", wr, cr.regen.endurance_base);
  cr.regen.endurance_per_level = p.opt(rg, "endurance_per_level", wr, cr.regen.endurance_per_level);
  cr.regen.mana_base = p.opt(rg, "mana_base", wr, cr.regen.mana_base);
  cr.regen.mana_per_level = p.opt(rg, "mana_per_level", wr, cr.regen.mana_per_level);
  cr.regen.out_of_combat_multiplier = p.opt(rg, "out_of_combat_multiplier", wr, cr.regen.out_of_combat_multiplier);
  cr.regen.sitting_multiplier = p.opt(rg, "sitting_multiplier", wr, cr.regen.sitting_multiplier);

  const auto& d = section(rules, "death");
  const std::string wd = "rules.death";
  r.death.xp_loss_fraction = p.opt(d, "xp_loss_fraction", wd, r.death.xp_loss_fraction);
  r.death.min_level_for_xp_loss = p.integer(d, "min_level_for_xp_loss", wd, r.death.min_level_for_xp_loss, false);
  r.death.player_corpse_seconds = p.opt(d, "player_corpse_seconds", wd, r.death.player_corpse_seconds);
  r.death.mob_corpse_seconds = p.opt(d, "mob_corpse_seconds", wd, r.death.mob_corpse_seconds);
  r.death.empty_mob_corpse_seconds = p.opt(d, "empty_mob_corpse_seconds", wd, r.death.empty_mob_corpse_seconds);

  const auto& ms = section(rules, "mob_scaling");
  const std::string wm = "rules.mob_scaling";
  auto& m = r.mob_scaling;
  m.hp_base = p.opt(ms, "hp_base", wm, m.hp_base);
  m.hp_per_level = p.opt(ms, "hp_per_level", wm, m.hp_per_level);
  m.hp_quad = p.opt(ms, "hp_quad", wm, m.hp_quad);
  m.max_hit_base = p.opt(ms, "max_hit_base", wm, m.max_hit_base);
  m.max_hit_per_level = p.opt(ms, "max_hit_per_level", wm, m.max_hit_per_level);
  m.max_hit_quad = p.opt(ms, "max_hit_quad", wm, m.max_hit_quad);
  m.ac_per_level = p.opt(ms, "ac_per_level", wm, m.ac_per_level);
  m.skill_per_level = p.opt(ms, "skill_per_level", wm, m.skill_per_level);
  m.avoid_skill_per_level = p.opt(ms, "avoid_skill_per_level", wm, m.avoid_skill_per_level);
  m.chase_seconds = p.opt(ms, "chase_seconds", wm, m.chase_seconds);
  m.flee_seconds = p.opt(ms, "flee_seconds", wm, m.flee_seconds);
  m.aggro_scan_ticks = p.integer(ms, "aggro_scan_ticks", wm, m.aggro_scan_ticks, false);
  m.resist_base = p.opt(ms, "resist_base", wm, m.resist_base);
  m.resist_per_level = p.opt(ms, "resist_per_level", wm, m.resist_per_level);

  const auto& sp = section(rules, "spells");
  const std::string wsp = "rules.spells";
  auto& s = r.spells;
  s.tick_seconds = p.opt(sp, "tick_seconds", wsp, s.tick_seconds);
  s.buff_slots = p.integer(sp, "buff_slots", wsp, s.buff_slots, false);
  s.debuff_slots = p.integer(sp, "debuff_slots", wsp, s.debuff_slots, false);
  s.spellbook_size = p.integer(sp, "spellbook_size", wsp, s.spellbook_size, false);
  if (!sp.contains("gem_unlock_levels")) {
    // The defaults must still fit a lower level cap (explicit lists are validated as given).
    for (int& l : s.gem_unlock_levels) l = std::clamp(l, 1, std::max(1, r.max_level));
  } else {
    const auto& g = sp.at("gem_unlock_levels");
    if (!g.is_array()) {
      p.errors().emplace_back("rules.spells: 'gem_unlock_levels' must be an array of integers");
    } else {
      s.gem_unlock_levels.clear();
      for (const auto& v : g) {
        if (!v.is_number_integer()) {
          p.errors().emplace_back("rules.spells: 'gem_unlock_levels' must contain only integers");
          continue;
        }
        s.gem_unlock_levels.push_back(static_cast<int>(std::clamp<long long>(v.get<long long>(), -1, 1000)));
      }
    }
  }
  s.memorize_seconds = p.opt(sp, "memorize_seconds", wsp, s.memorize_seconds);
  s.move_interrupt_distance = p.opt(sp, "move_interrupt_distance", wsp, s.move_interrupt_distance);
  s.range_slack = p.opt(sp, "range_slack", wsp, s.range_slack);
  s.damage_spread = p.opt(sp, "damage_spread", wsp, s.damage_spread);
  s.resist_scale_base = p.opt(sp, "resist_scale_base", wsp, s.resist_scale_base);
  s.resist_scale_per_level = p.opt(sp, "resist_scale_per_level", wsp, s.resist_scale_per_level);
  s.resist_per_level_diff = p.opt(sp, "resist_per_level_diff", wsp, s.resist_per_level_diff);
  s.min_resist = p.opt(sp, "min_resist", wsp, s.min_resist);
  s.max_resist = p.opt(sp, "max_resist", wsp, s.max_resist);
  s.partial_resist_min = p.opt(sp, "partial_resist_min", wsp, s.partial_resist_min);
  s.concentration_base = p.opt(sp, "concentration_base", wsp, s.concentration_base);
  s.concentration_skill_bonus = p.opt(sp, "concentration_skill_bonus", wsp, s.concentration_skill_bonus);
  s.concentration_skill_scale = p.opt(sp, "concentration_skill_scale", wsp, s.concentration_skill_scale);
  s.concentration_damage_scale = p.opt(sp, "concentration_damage_scale", wsp, s.concentration_damage_scale);
  s.min_concentration = p.opt(sp, "min_concentration", wsp, s.min_concentration);
  s.max_concentration = p.opt(sp, "max_concentration", wsp, s.max_concentration);
  s.root_break_chance = p.opt(sp, "root_break_chance", wsp, s.root_break_chance);
  s.heal_aggro_radius = p.opt(sp, "heal_aggro_radius", wsp, s.heal_aggro_radius);
  s.buff_threat_per_level = p.opt(sp, "buff_threat_per_level", wsp, s.buff_threat_per_level);
  s.rez_offer_seconds = p.opt(sp, "rez_offer_seconds", wsp, s.rez_offer_seconds);
  s.rez_hp_pct = p.opt(sp, "rez_hp_pct", wsp, s.rez_hp_pct);
  s.max_area_targets = p.integer(sp, "max_area_targets", wsp, s.max_area_targets, false);
  s.meditate_bonus = p.opt(sp, "meditate_bonus", wsp, s.meditate_bonus);

  const auto& pe = section(rules, "pets");
  const std::string wpe = "rules.pets";
  auto& pr = r.pets;
  pr.follow_distance = p.opt(pe, "follow_distance", wpe, pr.follow_distance);
  pr.leash_distance = p.opt(pe, "leash_distance", wpe, pr.leash_distance);
  pr.assist_radius = p.opt(pe, "assist_radius", wpe, pr.assist_radius);
  pr.back_off_seconds = p.opt(pe, "back_off_seconds", wpe, pr.back_off_seconds);
  pr.charm_check_seconds = p.opt(pe, "charm_check_seconds", wpe, pr.charm_check_seconds);
  pr.charm_break_base = p.opt(pe, "charm_break_base", wpe, pr.charm_break_base);
  pr.charm_break_per_level = p.opt(pe, "charm_break_per_level", wpe, pr.charm_break_per_level);
  pr.charm_break_cha_scale = p.opt(pe, "charm_break_cha_scale", wpe, pr.charm_break_cha_scale);
  pr.min_charm_break = p.opt(pe, "min_charm_break", wpe, pr.min_charm_break);
  pr.max_charm_break = p.opt(pe, "max_charm_break", wpe, pr.max_charm_break);

  r.songs.pulse_seconds = p.opt(section(rules, "songs"), "pulse_seconds", "rules.songs", r.songs.pulse_seconds);

  const auto& st = section(rules, "stealth");
  const std::string wst = "rules.stealth";
  auto& sr = r.stealth;
  sr.sneak_speed_mult = p.opt(st, "sneak_speed_mult", wst, sr.sneak_speed_mult);
  sr.sneak_aggro_mult = p.opt(st, "sneak_aggro_mult", wst, sr.sneak_aggro_mult);
  sr.hide_base = p.opt(st, "hide_base", wst, sr.hide_base);
  sr.hide_per_skill = p.opt(st, "hide_per_skill", wst, sr.hide_per_skill);
  sr.feign_base = p.opt(st, "feign_base", wst, sr.feign_base);
  sr.feign_per_skill = p.opt(st, "feign_per_skill", wst, sr.feign_per_skill);
  sr.feign_per_level = p.opt(st, "feign_per_level", wst, sr.feign_per_level);
  sr.pick_base = p.opt(st, "pick_base", wst, sr.pick_base);
  sr.pick_per_skill = p.opt(st, "pick_per_skill", wst, sr.pick_per_skill);
  sr.pick_per_level = p.opt(st, "pick_per_level", wst, sr.pick_per_level);
  sr.min_chance = p.opt(st, "min_chance", wst, sr.min_chance);
  sr.max_chance = p.opt(st, "max_chance", wst, sr.max_chance);

  const auto& cn = section(rules, "content");
  r.content.min_class_abilities = p.integer(cn, "min_class_abilities", "rules.content", r.content.min_class_abilities, false);
  r.content.max_level_gap = p.integer(cn, "max_level_gap", "rules.content", r.content.max_level_gap, false);
}

std::optional<EffectType> effect_type(const std::string& s) {
  static const std::map<std::string, EffectType, std::less<>> kTypes{
      {"damage", EffectType::Damage},       {"stun", EffectType::Stun},
      {"threat", EffectType::Threat},       {"taunt", EffectType::Taunt},
      {"buff", EffectType::Buff},           {"heal_pct", EffectType::HealPct},
      {"restore_endurance_pct", EffectType::RestoreEndurancePct}, {"area_threat", EffectType::AreaThreat},
      {"heal", EffectType::Heal},           {"dot", EffectType::Dot},
      {"hot", EffectType::Hot},             {"debuff", EffectType::Debuff},
      {"slow", EffectType::Slow},           {"snare", EffectType::Snare},
      {"root", EffectType::Root},           {"mez", EffectType::Mez},
      {"fear", EffectType::Fear},           {"dispel", EffectType::Dispel},
      {"resurrect", EffectType::Resurrect}, {"teleport", EffectType::Teleport},
      {"restore_mana_pct", EffectType::RestoreManaPct},
      {"summon_pet", EffectType::SummonPet}, {"charm", EffectType::Charm},
      {"illusion", EffectType::Illusion},    {"summon_item", EffectType::SummonItem},
      {"lifetap", EffectType::Lifetap},      {"drain", EffectType::Drain},
      {"feign_death", EffectType::FeignDeath}, {"hide", EffectType::Hide},
      {"sneak", EffectType::Sneak},          {"threat_drop", EffectType::ThreatDrop},
      {"track", EffectType::Track},          {"pick_lock", EffectType::PickLock},
      {"hp_to_mana", EffectType::HpToMana}};
  auto it = kTypes.find(s);
  if (it == kTypes.end()) return std::nullopt;
  return it->second;
}

std::optional<BuffStat> buff_stat(const std::string& s) {
  static const std::map<std::string, BuffStat, std::less<>> kStats{
      {"damage_taken_pct", BuffStat::DamageTakenPct}, {"melee_damage_pct", BuffStat::MeleeDamagePct},
      {"crit_chance_pct", BuffStat::CritChancePct},   {"ac", BuffStat::Ac},
      {"max_hp", BuffStat::MaxHp},                    {"max_mana", BuffStat::MaxMana},
      {"hp_regen", BuffStat::HpRegen},                {"mana_regen", BuffStat::ManaRegen},
      {"str", BuffStat::Str}, {"sta", BuffStat::Sta}, {"agi", BuffStat::Agi}, {"dex", BuffStat::Dex},
      {"wis", BuffStat::Wis}, {"int", BuffStat::Int}, {"cha", BuffStat::Cha},
      {"attack_speed_pct", BuffStat::AttackSpeedPct}, {"move_speed_pct", BuffStat::MoveSpeedPct},
      {"resist_magic", BuffStat::ResistMagic},        {"resist_fire", BuffStat::ResistFire},
      {"resist_cold", BuffStat::ResistCold},          {"resist_poison", BuffStat::ResistPoison},
      {"resist_disease", BuffStat::ResistDisease},    {"resist_all", BuffStat::ResistAll},
      {"damage_shield", BuffStat::DamageShield},      {"threat_pct", BuffStat::ThreatPct},
      {"invisible", BuffStat::Invisible}};
  auto it = kStats.find(s);
  if (it == kStats.end()) return std::nullopt;
  return it->second;
}

std::optional<ResistType> resist_type(const std::string& s) {
  for (std::size_t i = 0; i < kResistCount; ++i)
    if (s == resist_name(static_cast<ResistType>(i))) return static_cast<ResistType>(i);
  return std::nullopt;
}

std::optional<TargetType> target_type(const std::string& s) {
  static const std::map<std::string, TargetType, std::less<>> kTargets{
      {"enemy", TargetType::Enemy}, {"self", TargetType::Self},   {"friendly", TargetType::Friendly},
      {"group", TargetType::Group}, {"area", TargetType::Area},   {"corpse", TargetType::Corpse},
      {"pet", TargetType::Pet}};
  auto it = kTargets.find(s);
  if (it == kTargets.end()) return std::nullopt;
  return it->second;
}

AbilityDef parse_ability(Parser& p, const json& j, const std::string& w, bool spell) {
  AbilityDef a;
  a.spell = spell;
  a.id = p.str(j, "id", w);
  a.name = p.str(j, "name", w);
  a.class_id = p.str(j, "class", w);
  a.level = p.integer(j, "level", w, 1);
  a.type = p.str(j, "type", w);
  a.description = p.str(j, "description", w, false);
  a.skill = p.str(j, "skill", w, false);
  if (a.passive()) {
    if (spell) p.errors().push_back(w + ": spells cannot be passive");
    if (a.skill.empty()) p.errors().push_back(w + ": passive abilities must name a skill");
    return a;
  }
  a.song = p.boolean(j, "song", w, false, false);
  a.position = p.str(j, "position", w, false);
  const std::string res = p.str(j, "resource", w, false, spell && !a.song ? "mana" : "none");
  if (res == "endurance") a.resource = Resource::Endurance;
  else if (res == "mana") a.resource = Resource::Mana;
  else if (res == "none") a.resource = Resource::None;
  else p.errors().push_back(w + ": resource must be none, endurance, or mana");
  a.cost = p.integer(j, "cost", w, 0);
  a.cast_time = p.num(j, "cast_time", w, 0.0f);
  a.cooldown = p.num(j, "cooldown", w, 0.0f);
  a.gcd = p.boolean(j, "gcd", w, true);
  a.range = p.num(j, "range", w, 0.0f);
  if (const auto t = target_type(p.str(j, "target", w))) a.target = *t;
  else p.errors().push_back(w + ": target must be enemy, self, friendly, group, area, corpse, or pet");
  if (const auto r = resist_type(p.str(j, "resist", w, false, "none"))) a.resist = *r;
  else p.errors().push_back(w + ": resist must be none, magic, fire, cold, poison, or disease");
  a.resist_adjust = p.integer(j, "resist_adjust", w, 0, false);
  a.stack_group = p.str(j, "stack_group", w, false, a.id);
  a.rank = p.integer(j, "rank", w, 1, false);
  a.radius = p.opt(j, "radius", w, 0.0f);
  a.max_targets = p.integer(j, "max_targets", w, 0, false);
  a.requires_body = p.str(j, "requires_body", w, false);
  if (!j.contains("effects") || !j.at("effects").is_array() || j.at("effects").empty()) {
    p.errors().push_back(w + ": effects must be a non-empty array");
    return a;
  }
  const auto& effects = j.at("effects");
  for (std::size_t k = 0; k < effects.size(); ++k) {
    const auto& e = effects[k];
    const std::string we = w + ".effects[" + std::to_string(k) + "]";
    EffectDef ef;
    const auto t = effect_type(p.str(e, "type", we));
    if (!t) {
      p.errors().push_back(we + ": unknown effect type");
      continue;
    }
    ef.type = *t;
    ef.base = p.opt(e, "base", we, 0.0f);
    ef.per_level = p.opt(e, "per_level", we, 0.0f);
    ef.max = p.opt(e, "max", we, 0.0f);
    ef.value = p.opt(e, "value", we, 0.0f);
    ef.duration = p.opt(e, "duration", we, 0.0f);
    ef.radius = p.opt(e, "radius", we, 0.0f);
    ef.tick = p.opt(e, "tick", we, 0.0f);
    ef.max_target_level = p.integer(e, "max_target_level", we, 0, false);
    ef.destination = p.str(e, "destination", we, false);
    // One reference per effect, named for what it points at.
    int refs = 0;
    for (const char* key : {"pet", "item", "model"})
      if (e.is_object() && e.contains(key)) {
        ef.ref = p.str(e, key, we);
        ++refs;
      }
    if (refs > 1) p.errors().push_back(we + ": an effect names at most one of pet, item, or model");
    if (ef.type == EffectType::Buff || ef.type == EffectType::Debuff) {
      const auto st = buff_stat(p.str(e, "stat", we));
      if (!st) p.errors().push_back(we + ": unknown buff stat");
      else ef.stat = *st;
    }
    if (ef.type == EffectType::Slow) ef.stat = BuffStat::AttackSpeedPct;
    if (ef.type == EffectType::Snare) ef.stat = BuffStat::MoveSpeedPct;
    a.effects.push_back(ef);
  }
  return a;
}

// Per-effect checks that do not need the rest of the content.
void check_effect(const AbilityDef& a, const EffectDef& e, std::vector<std::string>& out) {
  const std::string w = "ability '" + a.id + "': ";
  const bool lasting = is_lasting(e.type);
  if (lasting && (e.duration <= 0.0f || e.duration > 86400.0f)) out.push_back(w + "lasting effects need a duration of (0, 86400] s");
  if (e.type == EffectType::Stun && (e.duration <= 0.0f || e.duration > 30.0f)) out.push_back(w + "stun duration must be (0, 30] s");
  if (e.type == EffectType::AreaThreat && e.radius <= 0.0f) out.push_back(w + "area_threat radius must be > 0");
  if ((e.type == EffectType::Slow || e.type == EffectType::Snare) && (e.value <= 0.0f || e.value > 90.0f))
    out.push_back(w + "slow/snare value is a percentage in (0, 90]");
  if ((e.type == EffectType::Dot || e.type == EffectType::Hot || e.type == EffectType::Heal) && e.base <= 0.0f && e.per_level <= 0.0f)
    out.push_back(w + "heal/dot/hot needs a positive base or per_level");
  if ((e.type == EffectType::Dot || e.type == EffectType::Hot) && e.tick != 0.0f && (e.tick < 1.0f || e.tick > e.duration))
    out.push_back(w + "dot/hot tick must be at least 1 s and no longer than the duration");
  if (e.max < 0.0f) out.push_back(w + "effect max must be >= 0");
  if (e.type == EffectType::Dispel && (e.value < 1.0f || e.value > 15.0f)) out.push_back(w + "dispel value (count) must be 1..15");
  if (e.type == EffectType::Resurrect && (e.value <= 0.0f || e.value > 100.0f)) out.push_back(w + "resurrect value (XP percent) must be (0, 100]");
  if ((e.type == EffectType::RestoreManaPct || e.type == EffectType::RestoreEndurancePct || e.type == EffectType::HealPct) &&
      (e.value <= 0.0f || e.value > 100.0f))
    out.push_back(w + "restore/heal percentages must be (0, 100]");
  if (e.type == EffectType::Teleport && e.destination.empty()) out.push_back(w + "teleport needs a destination");
  if (e.max_target_level < 0 || e.max_target_level > 255) out.push_back(w + "max_target_level must be 0..255");
  if (e.type == EffectType::SummonPet && (e.ref.empty() || (e.base <= 0.0f && e.per_level <= 0.0f)))
    out.push_back(w + "summon_pet needs a pet and a positive base or per_level (the pet's level)");
  if (e.type == EffectType::SummonItem && (e.ref.empty() || e.value < 1.0f || e.value > 100.0f || e.duration < 0.0f || e.duration > 86400.0f))
    out.push_back(w + "summon_item needs an item, a count (value) of 1..100, and a duration of 0..86400 s");
  if (e.type == EffectType::Illusion && e.ref.empty()) out.push_back(w + "illusion needs a model");
  if ((e.type == EffectType::Lifetap || e.type == EffectType::Drain) && (e.base <= 0.0f && e.per_level <= 0.0f))
    out.push_back(w + "lifetap/drain needs a positive base or per_level");
  if ((e.type == EffectType::Lifetap || e.type == EffectType::Drain || e.type == EffectType::ThreatDrop) && (e.value < 0.0f || e.value > 100.0f))
    out.push_back(w + "lifetap/drain/threat_drop value is a percentage in [0, 100]");
  if (e.type == EffectType::ThreatDrop && e.value <= 0.0f) out.push_back(w + "threat_drop needs a value");
  if ((e.type == EffectType::FeignDeath || e.type == EffectType::Hide || e.type == EffectType::Sneak) && a.target != TargetType::Self)
    out.push_back(w + "feign_death, hide, and sneak target self");
  if ((e.type == EffectType::Track || e.type == EffectType::HpToMana) && a.target != TargetType::Self)
    out.push_back(w + "track and hp_to_mana target self");
  if (e.type == EffectType::Track && e.radius <= 0.0f) out.push_back(w + "track needs a radius");
  if (e.type == EffectType::HpToMana && e.base <= 0.0f && e.per_level <= 0.0f) out.push_back(w + "hp_to_mana needs a positive base or per_level");
  if ((e.type == EffectType::Charm || e.type == EffectType::PickLock) && a.target != TargetType::Enemy)
    out.push_back(w + "charm and pick_lock need an enemy (targeted) ability");
  // Polarity: beneficial spells cannot harm their target and detrimental spells cannot help it.
  static const std::set<EffectType> kHarm{EffectType::Damage, EffectType::Stun, EffectType::Taunt, EffectType::Dot,
                                          EffectType::Debuff, EffectType::Slow, EffectType::Snare, EffectType::Root,
                                          EffectType::Mez, EffectType::Fear, EffectType::Charm, EffectType::Lifetap,
                                          EffectType::Drain};
  // Effects only the beneficial resolver applies: on a detrimental spell they would silently do nothing.
  static const std::set<EffectType> kHelp{EffectType::Heal,           EffectType::Hot,           EffectType::Resurrect,
                                          EffectType::HealPct,        EffectType::RestoreManaPct, EffectType::RestoreEndurancePct,
                                          EffectType::Teleport,       EffectType::SummonPet,     EffectType::SummonItem,
                                          EffectType::Illusion,       EffectType::FeignDeath,    EffectType::Hide,
                                          EffectType::Sneak,          EffectType::Track,         EffectType::HpToMana};
  if (a.beneficial() && kHarm.contains(e.type)) out.push_back(w + "a beneficial spell cannot carry harmful effects");
  if (!a.beneficial() && kHelp.contains(e.type)) out.push_back(w + "a detrimental spell cannot heal, restore, teleport, summon, or resurrect");
  if (a.target == TargetType::Corpse && e.type != EffectType::Resurrect) out.push_back(w + "a corpse-target spell can only resurrect");
  if (a.spell && !a.beneficial() && e.type == EffectType::Buff) out.push_back(w + "use 'debuff' for stat changes on enemies");
  if (a.spell && a.beneficial() && e.type == EffectType::Debuff) out.push_back(w + "use 'buff' for stat changes on friends");
}

}  // namespace

const SkillCap* ClassDef::skill(std::string_view skill_id) const {
  for (const auto& [sid, cap] : skills)
    if (sid == skill_id) return &cap;
  return nullptr;
}

const RaceDef* Content::find_race(std::string_view id) const { return find_by_id(races, id); }
const ClassDef* Content::find_class(std::string_view id) const { return find_by_id(classes, id); }
const CityDef* Content::find_city(std::string_view id) const { return find_by_id(cities, id); }
const ZoneDef* Content::find_zone(std::string_view id) const { return find_by_id(zones, id); }
const SkillDef* Content::find_skill(std::string_view id) const { return find_by_id(skills, id); }
const AbilityDef* Content::find_ability(std::string_view id) const { return find_by_id(abilities, id); }
const AbilityDef* Content::find_spell(std::string_view id) const { return find_by_id(spells, id); }
const NpcDef* Content::find_npc(std::string_view id) const { return find_by_id(npcs, id); }
const PetDef* Content::find_pet(std::string_view id) const { return find_by_id(pets, id); }
const ModelDef* Content::find_model(std::string_view id) const { return find_by_id(models, id); }
const ItemVisualDef* Content::find_item_visual(std::string_view id) const { return find_by_id(item_visuals, id); }
const ItemDef* Content::find_item(std::string_view id) const { return find_by_id(items, id); }
const LootTable* Content::find_loot(std::string_view id) const { return find_by_id(loot_tables, id); }
const MobDef* Content::find_mob(std::string_view id) const { return find_by_id(mobs, id); }

std::vector<const AbilityDef*> Content::abilities_for(std::string_view class_id) const {
  std::vector<const AbilityDef*> out;
  for (const auto& a : abilities)
    if (a.class_id == class_id) out.push_back(&a);
  std::stable_sort(out.begin(), out.end(), [](const AbilityDef* a, const AbilityDef* b) { return a->level < b->level; });
  return out;
}

std::vector<const AbilityDef*> Content::spells_for(std::string_view class_id) const {
  std::vector<const AbilityDef*> out;
  for (const auto& a : spells)
    if (a.class_id == class_id) out.push_back(&a);
  std::stable_sort(out.begin(), out.end(), [](const AbilityDef* a, const AbilityDef* b) { return a->level < b->level; });
  return out;
}

const ItemDef* Content::scroll_for(std::string_view spell_id) const {
  for (const auto& it : items)
    if (it.kind == "scroll" && it.spell == spell_id) return &it;
  return nullptr;
}

bool is_lasting(EffectType t) {
  switch (t) {
    case EffectType::Buff: case EffectType::Debuff: case EffectType::Slow: case EffectType::Snare:
    case EffectType::Root: case EffectType::Mez: case EffectType::Fear: case EffectType::Dot: case EffectType::Hot:
    case EffectType::Charm: case EffectType::Illusion: case EffectType::Drain:
      return true;
    default:
      return false;
  }
}

bool AbilityDef::has(EffectType t) const {
  return std::any_of(effects.begin(), effects.end(), [&](const EffectDef& e) { return e.type == t; });
}

float AbilityDef::lasting_seconds() const {
  float d = 0.0f;
  for (const auto& e : effects)
    if (is_lasting(e.type)) d = std::max(d, e.duration);
  return d;
}

const char* resist_name(ResistType r) {
  switch (r) {
    case ResistType::None: return "none";
    case ResistType::Magic: return "magic";
    case ResistType::Fire: return "fire";
    case ResistType::Cold: return "cold";
    case ResistType::Poison: return "poison";
    case ResistType::Disease: return "disease";
  }
  return "none";
}

namespace {

// models.json (models, animation_maps, item_visuals) and appearance.json (races, classes).
void parse_visuals(Parser& p, const ContentDocs& docs, Content& c) {
  if (const auto* list = p.list(docs.models, "models", "models.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "models[" + std::to_string(i) + "]";
      ModelDef m;
      m.id = p.str(j, "id", w);
      p.str(j, "scene", w);  // client path ("" = placeholder); checked by tools/validate_content.py
      m.scale = p.opt(j, "scale", w, 1.0f);
      m.nameplate_height = p.num(j, "nameplate_height", w, 2.2f);
      m.collision_radius = p.num(j, "collision_radius", w, 0.5f);
      m.animations = p.str(j, "animations", w, false);
      c.models.push_back(std::move(m));
    }
    if (const auto* maps = p.list(docs.models, "animation_maps", "models.json")) {
      for (std::size_t i = 0; i < maps->size(); ++i) {
        const auto& j = (*maps)[i];
        const std::string w = "animation_maps[" + std::to_string(i) + "]";
        AnimationMapDef a;
        a.id = p.str(j, "id", w);
        if (!j.is_object() || !j.contains("clips") || !j.at("clips").is_object()) p.error(w + ": 'clips' must be an object");
        c.animation_maps.push_back(std::move(a));
      }
    }
    if (const auto* vis = p.list(docs.models, "item_visuals", "models.json")) {
      for (std::size_t i = 0; i < vis->size(); ++i) {
        const auto& j = (*vis)[i];
        const std::string w = "item_visuals[" + std::to_string(i) + "]";
        ItemVisualDef v;
        v.id = p.str(j, "id", w);
        p.str(j, "scene", w);
        v.slot = p.str(j, "slot", w);
        c.item_visuals.push_back(std::move(v));
      }
    }
  }
  if (const auto* list = p.list(docs.appearance, "races", "appearance.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "appearance.races[" + std::to_string(i) + "]";
      RaceAppearance r;
      r.race = p.str(j, "race", w);
      for (std::size_t g = 0; g < kGenders.size(); ++g) {
        const std::string wg = w + "." + kGenders[g];
        if (!j.is_object() || !j.contains(kGenders[g]) || !j.at(kGenders[g]).is_object()) {
          p.error(wg + ": missing object");
          continue;
        }
        r.models[g] = p.str(j.at(kGenders[g]), "model", wg);
      }
      c.race_appearance.push_back(std::move(r));
    }
  }
  if (const auto* list = p.list(docs.appearance, "classes", "appearance.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "appearance.classes[" + std::to_string(i) + "]";
      ClassAppearance a;
      a.class_id = p.str(j, "class", w);
      if (j.is_object() && j.contains("held")) {
        if (!j.at("held").is_object()) {
          p.error(w + ": 'held' must be an object of slot -> item id");
        } else {
          for (const auto& [slot, item] : j.at("held").items()) {
            if (!item.is_string()) {
              p.error(cat({w, ".held.", slot, ": must be an item id"}));
              continue;
            }
            a.held.emplace_back(slot, item.get<std::string>());
          }
        }
      }
      c.class_appearance.push_back(std::move(a));
    }
  }
}

bool valid_slot(const std::string& s) {
  return std::any_of(kAttachSlots.begin(), kAttachSlots.end(), [&](const char* k) { return s == k; });
}

// Model, animation map, item visual and appearance checks. Skipped when no models are defined at all
// (unit tests that build partial content); load_content() requires models.json on disk.
void validate_visuals(const Content& c, std::vector<std::string>& e) {
  if (c.models.empty()) return;
  check_ids(c.models, "model", e);
  check_ids(c.animation_maps, "animation map", e);
  check_ids(c.item_visuals, "item visual", e);
  for (const auto& m : c.models) {
    const std::string w = "model '" + m.id + "': ";
    if (m.scale < 0.05f || m.scale > 20.0f) e.push_back(w + "scale must be 0.05..20");
    if (m.nameplate_height < 0.2f || m.nameplate_height > 30.0f) e.push_back(w + "nameplate_height must be 0.2..30");
    if (m.collision_radius < 0.05f || m.collision_radius > 15.0f) e.push_back(w + "collision_radius must be 0.05..15");
    if (!m.animations.empty() && !find_by_id(c.animation_maps, m.animations))
      e.push_back(w + "unknown animation map '" + m.animations + "'");
  }
  for (const auto& v : c.item_visuals)
    if (!valid_slot(v.slot)) e.push_back("item visual '" + v.id + "': slot must be main_hand, off_hand, back or head");
  for (const auto& it : c.items)
    if (!it.visual.empty() && !c.find_item_visual(it.visual)) e.push_back("item '" + it.id + "': unknown visual '" + it.visual + "'");
  for (const auto& m : c.mobs)
    if (!c.find_model(m.model)) e.push_back("mob '" + m.id + "': unknown model '" + m.model + "'");
  for (const auto& n : c.npcs)
    if (!c.find_model(n.model)) e.push_back("npc '" + n.id + "': unknown model '" + n.model + "'");
  // Corpses restored from the database use the generic humanoid model.
  if (!c.find_model("humanoid")) e.emplace_back("models.json: a 'humanoid' model is required (restored player corpses use it)");
  std::set<std::string> seen;
  for (const auto& r : c.race_appearance) {
    const std::string w = "appearance race '" + r.race + "': ";
    if (!c.find_race(r.race)) e.push_back(w + "unknown race");
    if (!seen.insert(r.race).second) e.push_back(w + "listed twice");
    for (std::size_t g = 0; g < kGenders.size(); ++g)
      if (!r.models[g].empty() && !c.find_model(r.models[g]))
        e.push_back(w + kGenders[g] + " uses unknown model '" + r.models[g] + "'");
  }
  for (const auto& r : c.races)
    if (!seen.contains(r.id)) e.push_back("race '" + r.id + "': no entry in appearance.json");
  seen.clear();
  for (const auto& a : c.class_appearance) {
    const std::string w = "appearance class '" + a.class_id + "': ";
    if (!c.find_class(a.class_id)) e.push_back(w + "unknown class");
    if (!seen.insert(a.class_id).second) e.push_back(w + "listed twice");
    for (const auto& [slot, item] : a.held) {
      if (!valid_slot(slot)) e.push_back(cat({w, "held slot '", slot, "' must be main_hand, off_hand, back or head"}));
      const ItemDef* it = c.find_item(item);
      if (!it) {
        e.push_back(cat({w, "holds unknown item '", item, "'"}));
      } else if (it->visual.empty()) {
        e.push_back(cat({w, "holds item '", item, "', which has no visual"}));
      } else if (const ItemVisualDef* v = c.find_item_visual(it->visual); v && v->slot != slot) {
        e.push_back(cat({w, "item '", item, "' belongs in ", v->slot, ", not ", slot}));
      }
    }
  }
}

}  // namespace

Content parse_content(const ContentDocs& docs) {
  std::vector<std::string> errors;
  Parser p(errors);
  Content c;
  parse_rules(p, docs.rules, c.rules);

  // Skill defaults first: classes merge them with their own overrides.
  std::vector<std::pair<std::string, SkillCap>> default_skills;
  if (const auto* list = p.list(docs.skills, "skills", "skills.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "skills[" + std::to_string(i) + "]";
      SkillDef s;
      s.id = p.str(j, "id", w);
      s.name = p.str(j, "name", w);
      s.kind = p.str(j, "kind", w);
      s.description = p.str(j, "description", w, false);
      c.skills.push_back(std::move(s));
    }
    if (docs.skills.contains("defaults") && !docs.skills.at("defaults").is_object()) errors.emplace_back("skills.json: 'defaults' must be an object");
    const auto& defs = section(docs.skills, "defaults");
    for (const auto& [k, v] : defs.items()) default_skills.emplace_back(k, p.skill_cap(v, "skills.defaults." + k));
  }

  if (const auto* list = p.list(docs.races, "races", "races.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "races[" + std::to_string(i) + "]";
      RaceDef r;
      r.id = p.str(j, "id", w);
      r.name = p.str(j, "name", w);
      r.description = p.str(j, "description", w, false);
      r.allowed_classes = p.str_list(j, "allowed_classes", w);
      r.cities = p.str_list(j, "cities", w);
      if (j.is_object() && j.contains("stats")) r.stats = p.stats(j.at("stats"), w + ".stats", r.stats, true);
      c.races.push_back(std::move(r));
    }
  }
  if (const auto* list = p.list(docs.classes, "classes", "classes.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "classes[" + std::to_string(i) + "]";
      ClassDef d;
      d.id = p.str(j, "id", w);
      d.name = p.str(j, "name", w);
      d.archetype = p.str(j, "archetype", w);
      d.description = p.str(j, "description", w, false);
      for (const char* key : {"combat", "skills"})
        if (j.is_object() && j.contains(key) && !j.at(key).is_object()) errors.push_back(w + ": '" + key + "' must be an object");
      const auto& cb = section(j, "combat");
      const std::string wc = w + ".combat";
      auto& k = d.combat;
      k.hp_base = p.opt(cb, "hp_base", wc, k.hp_base);
      k.hp_per_level = p.opt(cb, "hp_per_level", wc, k.hp_per_level);
      k.mana_base = p.opt(cb, "mana_base", wc, k.mana_base);
      k.mana_per_level = p.opt(cb, "mana_per_level", wc, k.mana_per_level);
      k.endurance_base = p.opt(cb, "endurance_base", wc, k.endurance_base);
      k.endurance_per_level = p.opt(cb, "endurance_per_level", wc, k.endurance_per_level);
      k.ac_per_level = p.opt(cb, "ac_per_level", wc, k.ac_per_level);
      k.weapon_damage_base = p.opt(cb, "weapon_damage_base", wc, k.weapon_damage_base);
      k.weapon_damage_per_level = p.opt(cb, "weapon_damage_per_level", wc, k.weapon_damage_per_level);
      k.weapon_delay = p.opt(cb, "weapon_delay", wc, k.weapon_delay);
      k.crit_chance = p.opt(cb, "crit_chance", wc, k.crit_chance);
      k.threat_mult = p.opt(cb, "threat_mult", wc, k.threat_mult);
      d.armor = p.str(j, "armor", w, false, d.armor);
      if (j.is_object() && j.contains("weapons")) d.weapons = p.str_list(j, "weapons", w);
      if (cb.contains("stats")) k.stats = p.stats(cb.at("stats"), wc + ".stats", StatBlock{}, false);
      if (j.is_object() && j.contains("starter_spells")) d.starter_spells = p.str_list(j, "starter_spells", w);
      d.skills = default_skills;
      for (const auto& [sid, v] : section(j, "skills").items()) {
        std::string where = w;
        where += ".skills.";
        where += sid;
        const SkillCap cap = p.skill_cap(v, where);
        auto it = std::find_if(d.skills.begin(), d.skills.end(), [&](const auto& e) { return e.first == sid; });
        if (it != d.skills.end()) it->second = cap;
        else d.skills.emplace_back(sid, cap);
      }
      c.classes.push_back(std::move(d));
    }
  }
  if (const auto* list = p.list(docs.cities, "cities", "cities.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "cities[" + std::to_string(i) + "]";
      CityDef d;
      d.id = p.str(j, "id", w);
      d.name = p.str(j, "name", w);
      d.description = p.str(j, "description", w, false);
      d.zone = p.str(j, "zone", w);
      d.spawn = p.spawn(j, "spawn", w);
      c.cities.push_back(std::move(d));
    }
  }
  if (const auto* list = p.list(docs.zones, "zones", "zones.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "zones[" + std::to_string(i) + "]";
      ZoneDef z;
      z.id = p.str(j, "id", w);
      z.name = p.str(j, "name", w);
      z.description = p.str(j, "description", w, false);
      if (j.is_object() && j.contains("bounds")) {
        const auto& b = j.at("bounds");
        z.min_x = p.num(b, "min_x", w + ".bounds", 0.0f);
        z.min_z = p.num(b, "min_z", w + ".bounds", 0.0f);
        z.max_x = p.num(b, "max_x", w + ".bounds", 0.0f);
        z.max_z = p.num(b, "max_z", w + ".bounds", 0.0f);
      } else {
        errors.push_back(w + ": missing 'bounds'");
      }
      z.ground_y = p.num(j, "ground_y", w, 0.0f);
      z.safe_spawn = p.spawn(j, "safe_spawn", w);
      z.interest_radius = p.num(j, "interest_radius", w, 80.0f);
      z.say_radius = p.num(j, "say_radius", w, 60.0f);
      if (j.is_object() && j.contains("spawns")) {
        if (!j.at("spawns").is_array()) {
          errors.push_back(w + ": spawns must be an array");
        } else {
          const auto& sp = j.at("spawns");
          for (std::size_t k = 0; k < sp.size(); ++k) {
            const std::string ws = w + ".spawns[" + std::to_string(k) + "]";
            MobSpawn s;
            s.mob = p.str(sp[k], "mob", ws);
            s.x = p.num(sp[k], "x", ws, 0.0f);
            s.z = p.num(sp[k], "z", ws, 0.0f);
            s.count = p.integer(sp[k], "count", ws, 1, false);
            s.radius = p.opt(sp[k], "radius", ws, 0.0f);
            z.spawns.push_back(std::move(s));
          }
        }
      }
      if (j.is_object() && j.contains("obstacles")) {
        if (!j.at("obstacles").is_array()) {
          errors.push_back(w + ": obstacles must be an array");
        } else {
          const auto& ob = j.at("obstacles");
          for (std::size_t k = 0; k < ob.size(); ++k) {
            const std::string wo = w + ".obstacles[" + std::to_string(k) + "]";
            Obstacle o;
            o.x = p.num(ob[k], "x", wo, 0.0f);
            o.z = p.num(ob[k], "z", wo, 0.0f);
            o.radius = p.num(ob[k], "radius", wo, 1.0f);
            z.obstacles.push_back(o);
          }
        }
      }
      c.zones.push_back(std::move(z));
    }
  }
  if (const auto* list = p.list(docs.abilities, "abilities", "abilities.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) c.abilities.push_back(parse_ability(p, (*list)[i], "abilities[" + std::to_string(i) + "]", false));
  }
  if (const auto* list = p.list(docs.spells, "spells", "spells.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) c.spells.push_back(parse_ability(p, (*list)[i], "spells[" + std::to_string(i) + "]", true));
  }
  if (const auto* list = p.list(docs.items, "items", "items.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "items[" + std::to_string(i) + "]";
      ItemDef it;
      it.id = p.str(j, "id", w);
      it.name = p.str(j, "name", w);
      it.kind = p.str(j, "kind", w);
      it.stack = p.integer(j, "stack", w, 1);
      it.value = p.integer(j, "value", w, 0);
      it.rarity = p.str(j, "rarity", w, false, "common");
      it.description = p.str(j, "description", w, false);
      it.spell = p.str(j, "spell", w, false);
      it.visual = p.str(j, "visual", w, false);
      it.summoned = p.boolean(j, "summoned", w, false, false);
      c.items.push_back(std::move(it));
    }
  }
  if (const auto* list = p.list(docs.loot, "loot_tables", "loot.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "loot_tables[" + std::to_string(i) + "]";
      LootTable t;
      t.id = p.str(j, "id", w);
      t.coin_min = p.integer(j, "coin_min", w, 0);
      t.coin_max = p.integer(j, "coin_max", w, 0);
      t.max_drops = p.integer(j, "max_drops", w, 1);
      if (j.is_object() && j.contains("drops") && j.at("drops").is_array()) {
        const auto& drops = j.at("drops");
        for (std::size_t k = 0; k < drops.size(); ++k) {
          const std::string wd = w + ".drops[" + std::to_string(k) + "]";
          LootDrop d;
          d.item = p.str(drops[k], "item", wd);
          d.chance = p.num(drops[k], "chance", wd, 0.0f);
          d.min = p.integer(drops[k], "min", wd, 1);
          d.max = p.integer(drops[k], "max", wd, 1);
          t.drops.push_back(std::move(d));
        }
      } else {
        errors.push_back(w + ": drops must be an array");
      }
      c.loot_tables.push_back(std::move(t));
    }
  }
  if (const auto* list = p.list(docs.mobs, "mobs", "mobs.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "mobs[" + std::to_string(i) + "]";
      MobDef m;
      m.id = p.str(j, "id", w);
      m.name = p.str(j, "name", w);
      m.level = p.integer(j, "level", w, 1);
      m.model = p.str(j, "model", w);
      m.scale = p.num(j, "scale", w, 1.0f);
      m.hp_mult = p.num(j, "hp_mult", w, 1.0f);
      m.damage_mult = p.num(j, "damage_mult", w, 1.0f);
      m.attack_delay = p.num(j, "attack_delay", w, 2.5f);
      m.run_speed = p.num(j, "run_speed", w, 6.0f);
      m.aggro_radius = p.num(j, "aggro_radius", w, 0.0f);
      m.assist = p.boolean(j, "assist", w, false);
      m.assist_radius = p.num(j, "assist_radius", w, 0.0f);
      m.faction = p.str(j, "faction", w);
      m.flee_hp_pct = p.num(j, "flee_hp_pct", w, 0.0f);
      m.wander_radius = p.num(j, "wander_radius", w, 0.0f);
      m.leash_radius = p.num(j, "leash_radius", w, 80.0f);
      m.respawn_seconds = p.num(j, "respawn_seconds", w, 60.0f);
      m.stun_immune = p.boolean(j, "stun_immune", w, false);
      m.charm_immune = p.boolean(j, "charm_immune", w, false, false);
      m.loot = p.str(j, "loot", w, false);
      m.body = p.str(j, "body", w, false, "humanoid");
      if (j.is_object() && j.contains("resists")) {
        const auto& rs = j.at("resists");
        if (!rs.is_object()) {
          errors.push_back(w + ": resists must be an object");
        } else {
          for (const auto& [k, v] : rs.items()) {
            const auto rt = resist_type(k);
            if (!rt || *rt == ResistType::None) {
              errors.push_back(cat({w, ".resists: unknown resist '", k, "'"}));
              continue;
            }
            m.resists[static_cast<std::size_t>(*rt)] = p.integer(rs, k.c_str(), w + ".resists", -1);
          }
        }
      }
      c.mobs.push_back(std::move(m));
    }
  }

  if (const auto* list = p.list(docs.npcs, "npcs", "npcs.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "npcs[" + std::to_string(i) + "]";
      NpcDef n;
      n.id = p.str(j, "id", w);
      n.name = p.str(j, "name", w);
      n.role = p.str(j, "role", w);
      n.model = p.str(j, "model", w, false, "humanoid");
      n.level = p.integer(j, "level", w, 50, false);
      n.zone = p.str(j, "zone", w);
      n.x = p.num(j, "x", w, 0.0f);
      n.z = p.num(j, "z", w, 0.0f);
      n.yaw = p.opt(j, "yaw", w, 0.0f);
      if (j.is_object() && j.contains("classes")) n.classes = p.str_list(j, "classes", w);
      if (j.is_object() && j.contains("sells")) n.sells = p.str_list(j, "sells", w);
      n.greeting = p.str(j, "greeting", w, false);
      n.lock_level = p.integer(j, "lock_level", w, 0, false);
      c.npcs.push_back(std::move(n));
    }
  }

  if (const auto* list = p.list(docs.pets, "pets", "pets.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "pets[" + std::to_string(i) + "]";
      PetDef d;
      d.id = p.str(j, "id", w);
      d.name = p.str(j, "name", w);
      d.variant = p.str(j, "variant", w);
      d.model = p.str(j, "model", w);
      d.body = p.str(j, "body", w, false, d.body);
      d.scale = p.opt(j, "scale", w, d.scale);
      d.hp_mult = p.opt(j, "hp_mult", w, d.hp_mult);
      d.damage_mult = p.opt(j, "damage_mult", w, d.damage_mult);
      d.attack_delay = p.opt(j, "attack_delay", w, d.attack_delay);
      d.run_speed = p.opt(j, "run_speed", w, d.run_speed);
      d.threat_mult = p.opt(j, "threat_mult", w, d.threat_mult);
      d.damage_shield_per_level = p.opt(j, "damage_shield_per_level", w, d.damage_shield_per_level);
      d.description = p.str(j, "description", w, false);
      if (j.is_object() && j.contains("resists")) {
        const auto& rs = j.at("resists");
        if (!rs.is_object()) {
          errors.push_back(w + ": resists must be an object");
        } else {
          for (const auto& [k, v] : rs.items()) {
            const auto rt = resist_type(k);
            if (!rt || *rt == ResistType::None) {
              errors.push_back(cat({w, ".resists: unknown resist '", k, "'"}));
              continue;
            }
            d.resists[static_cast<std::size_t>(*rt)] = p.integer(rs, k.c_str(), w + ".resists", -1);
          }
        }
      }
      c.pets.push_back(std::move(d));
    }
  }

  parse_visuals(p, docs, c);

  if (!errors.empty()) throw_errors(errors);
  // Mobs take their body radius from their model (melee reach scales with it).
  for (auto& m : c.mobs)
    if (const ModelDef* md = c.find_model(m.model)) m.collision_radius = md->collision_radius;
  auto problems = validate_content(c);
  if (!problems.empty()) throw_errors(problems);
  return c;
}

std::vector<std::string> validate_content(const Content& c) {
  std::vector<std::string> e;
  const auto& R = c.rules;
  validate_visuals(c, e);
  if (R.max_level < 1 || R.max_level > 255) e.emplace_back("rules.max_level must be 1..255");
  if (R.max_characters_per_account < 1 || R.max_characters_per_account > 16) e.emplace_back("rules.max_characters_per_account must be 1..16");
  if (R.starting_coin < 0 || R.starting_coin > 1000000) e.emplace_back("rules.starting_coin must be 0..1000000");
  if (R.movement.run_speed <= 0.0f || R.movement.run_speed > 50.0f) e.emplace_back("rules.movement.run_speed must be in (0, 50]");
  if (R.movement.desync_tolerance <= 0.0f) e.emplace_back("rules.movement.desync_tolerance must be > 0");
  if (R.movement.teleport_tolerance < 1.0f) e.emplace_back("rules.movement.teleport_tolerance must be >= 1");
  if (R.xp.base <= 0.0 || R.xp.kill_base <= 0.0) e.emplace_back("rules.xp.base and kill_base must be > 0");
  if (R.xp.growth < 1.0 || R.xp.growth > 1.5) e.emplace_back("rules.xp.growth must be in [1, 1.5]");
  if (R.xp.level_exponent < 0.0 || R.xp.level_exponent > 4.0) e.emplace_back("rules.xp.level_exponent must be in [0, 4]");
  {
    // The whole curve up to max_level must stay representable (see rules.hpp: kMaxXp).
    double total = 0.0;
    for (int l = 1; l < std::max(1, R.max_level) && l <= 255; ++l)
      total += R.xp.base * std::pow(static_cast<double>(l), R.xp.level_exponent) * std::pow(R.xp.growth, static_cast<double>(l - 1));
    if (!std::isfinite(total) || total >= 1.0e15) e.emplace_back("rules.xp: total XP to max_level must stay below 1e15");
  }
  for (double m : R.xp.con_multipliers)
    if (m < 0.0 || m > 10.0) e.emplace_back("rules.xp.con_multipliers must be in [0, 10]");
  if (R.con.blue_divisor < 1 || R.con.green_divisor < 1) e.emplace_back("rules.con divisors must be >= 1");
  const auto& cr = R.combat;
  if (cr.gcd_seconds < 0.0f || cr.gcd_seconds > 10.0f) e.emplace_back("rules.combat.gcd_seconds must be 0..10");
  if (cr.melee_range <= 0.0f || cr.loot_range <= 0.0f || cr.event_radius <= 0.0f) e.emplace_back("rules.combat ranges must be > 0");
  if (!(cr.min_hit_chance >= 0.0f && cr.min_hit_chance <= cr.max_hit_chance && cr.max_hit_chance <= 1.0f))
    e.emplace_back("rules.combat hit chance bounds must satisfy 0 <= min <= max <= 1");
  if (cr.max_mitigation < 0.0f || cr.max_mitigation > 0.95f) e.emplace_back("rules.combat.max_mitigation must be 0..0.95");
  if (cr.max_avoid_chance < 0.0f || cr.max_avoid_chance > 0.75f) e.emplace_back("rules.combat.max_avoid_chance must be 0..0.75");
  if (cr.avoid_skill_divisor <= 0.0f || cr.double_attack_divisor <= 0.0f || cr.mitigation_base <= 0.0f)
    e.emplace_back("rules.combat divisors must be > 0");
  if (R.death.player_corpse_seconds < 60.0f) e.emplace_back("rules.death.player_corpse_seconds must be >= 60");
  if (R.death.xp_loss_fraction < 0.0f || R.death.xp_loss_fraction > 1.0f) e.emplace_back("rules.death.xp_loss_fraction must be 0..1");
  if (R.mob_scaling.aggro_scan_ticks < 1) e.emplace_back("rules.mob_scaling.aggro_scan_ticks must be >= 1");
  {
    const auto& S = R.spells;
    if (S.tick_seconds < 0.5f || S.tick_seconds > 60.0f) e.emplace_back("rules.spells.tick_seconds must be 0.5..60");
    if (S.buff_slots < 1 || S.buff_slots > 15) e.emplace_back("rules.spells.buff_slots must be 1..15");
    if (S.debuff_slots < 1 || S.debuff_slots > 30) e.emplace_back("rules.spells.debuff_slots must be 1..30");
    if (S.spellbook_size < 1 || S.spellbook_size > 128) e.emplace_back("rules.spells.spellbook_size must be 1..128");
    if (S.gem_unlock_levels.empty() || S.gem_unlock_levels.size() > 8) e.emplace_back("rules.spells.gem_unlock_levels needs 1..8 entries");
    for (std::size_t i = 0; i < S.gem_unlock_levels.size(); ++i) {
      const int l = S.gem_unlock_levels[i];
      if (l < 1 || l > R.max_level || (i > 0 && l < S.gem_unlock_levels[i - 1])) {
        e.emplace_back("rules.spells.gem_unlock_levels must be non-decreasing levels within 1..max_level");
        break;
      }
    }
    if (S.memorize_seconds < 0.0f || S.memorize_seconds > 60.0f) e.emplace_back("rules.spells.memorize_seconds must be 0..60");
    if (S.range_slack < 1.0f) e.emplace_back("rules.spells.range_slack must be >= 1");
    if (S.damage_spread < 0.0f || S.damage_spread > 0.5f) e.emplace_back("rules.spells.damage_spread must be 0..0.5");
    if (S.resist_scale_base <= 0.0f || S.resist_scale_per_level < 0.0f) e.emplace_back("rules.spells resist scale must be > 0");
    if (!(S.min_resist >= 0.0f && S.min_resist <= S.max_resist && S.max_resist <= 1.0f)) e.emplace_back("rules.spells resist bounds must satisfy 0 <= min <= max <= 1");
    if (!(S.min_concentration >= 0.0f && S.min_concentration <= S.max_concentration && S.max_concentration <= 1.0f))
      e.emplace_back("rules.spells concentration bounds must satisfy 0 <= min <= max <= 1");
    if (S.concentration_skill_scale <= 0.0f) e.emplace_back("rules.spells.concentration_skill_scale must be > 0");
    if (S.partial_resist_min < 0.0f || S.partial_resist_min > 1.0f) e.emplace_back("rules.spells.partial_resist_min must be 0..1");
    if (S.root_break_chance < 0.0f || S.root_break_chance > 1.0f) e.emplace_back("rules.spells.root_break_chance must be 0..1");
    if (S.rez_hp_pct <= 0.0f || S.rez_hp_pct > 1.0f) e.emplace_back("rules.spells.rez_hp_pct must be (0, 1]");
    if (S.rez_offer_seconds < 5.0f) e.emplace_back("rules.spells.rez_offer_seconds must be >= 5");
    if (S.max_area_targets < 1 || S.max_area_targets > 50) e.emplace_back("rules.spells.max_area_targets must be 1..50");
  }
  {
    if (cr.behind_arc_degrees <= 0.0f || cr.behind_arc_degrees > 360.0f) e.emplace_back("rules.combat.behind_arc_degrees must be (0, 360]");
    if (cr.offhand_damage_mult < 0.0f || cr.offhand_damage_mult > 2.0f) e.emplace_back("rules.combat.offhand_damage_mult must be 0..2");
    if (cr.dual_wield_divisor <= 0.0f) e.emplace_back("rules.combat.dual_wield_divisor must be > 0");
    if (cr.max_dual_wield_chance < 0.0f || cr.max_dual_wield_chance > 1.0f) e.emplace_back("rules.combat.max_dual_wield_chance must be 0..1");
    if (cr.enhanced_damage_max < 0.0f || cr.enhanced_damage_max > 2.0f) e.emplace_back("rules.combat.enhanced_damage_max must be 0..2");
    const auto& P = R.pets;
    if (P.follow_distance < 0.5f || P.follow_distance > 20.0f) e.emplace_back("rules.pets.follow_distance must be 0.5..20");
    if (P.leash_distance < 20.0f || P.assist_radius <= 0.0f) e.emplace_back("rules.pets.leash_distance must be >= 20 and assist_radius > 0");
    if (P.back_off_seconds < 0.0f || P.back_off_seconds > 60.0f) e.emplace_back("rules.pets.back_off_seconds must be 0..60");
    if (P.charm_check_seconds < 1.0f || P.charm_check_seconds > 120.0f) e.emplace_back("rules.pets.charm_check_seconds must be 1..120");
    if (!(P.min_charm_break >= 0.0f && P.min_charm_break <= P.max_charm_break && P.max_charm_break <= 1.0f))
      e.emplace_back("rules.pets charm break bounds must satisfy 0 <= min <= max <= 1");
    if (R.songs.pulse_seconds < 1.0f || R.songs.pulse_seconds > 30.0f) e.emplace_back("rules.songs.pulse_seconds must be 1..30");
    const auto& T = R.stealth;
    if (T.sneak_speed_mult <= 0.0f || T.sneak_speed_mult > 1.0f || T.sneak_aggro_mult < 0.0f || T.sneak_aggro_mult > 1.0f)
      e.emplace_back("rules.stealth sneak multipliers must be in (0, 1]");
    if (!(T.min_chance >= 0.0f && T.min_chance <= T.max_chance && T.max_chance <= 1.0f))
      e.emplace_back("rules.stealth chance bounds must satisfy 0 <= min <= max <= 1");
    if (R.content.min_class_abilities < 0 || R.content.min_class_abilities > 200 || R.content.max_level_gap < 0 || R.content.max_level_gap > 60)
      e.emplace_back("rules.content: min_class_abilities must be 0..200 and max_level_gap 0..60");
  }

  if (c.races.empty()) e.emplace_back("at least one race is required");
  if (c.classes.empty()) e.emplace_back("at least one class is required");
  if (c.cities.empty()) e.emplace_back("at least one city is required");
  if (c.zones.empty()) e.emplace_back("at least one zone is required");
  check_ids(c.races, "race", e);
  check_ids(c.classes, "class", e);
  check_ids(c.cities, "city", e);
  check_ids(c.zones, "zone", e);
  check_ids(c.skills, "skill", e);
  check_ids(c.abilities, "ability", e);
  check_ids(c.items, "item", e);
  check_ids(c.loot_tables, "loot table", e);
  check_ids(c.mobs, "mob", e);
  check_ids(c.spells, "spell", e);
  check_ids(c.npcs, "npc", e);
  check_ids(c.pets, "pet", e);
  for (const auto& s : c.spells)
    if (c.find_ability(s.id)) e.push_back("spell '" + s.id + "': id is also an ability id");

  for (const auto& z : c.zones) {
    if (!(z.min_x < z.max_x && z.min_z < z.max_z)) e.push_back("zone '" + z.id + "': bounds min must be < max");
    if (z.interest_radius < 10.0f || z.interest_radius > 500.0f) e.push_back("zone '" + z.id + "': interest_radius must be 10..500");
    if (z.say_radius <= 0.0f) e.push_back("zone '" + z.id + "': say_radius must be > 0");
    const auto& s = z.safe_spawn;
    if (s.x < z.min_x || s.x > z.max_x || s.z < z.min_z || s.z > z.max_z) e.push_back("zone '" + z.id + "': safe_spawn is outside bounds");
    if (z.obstacles.size() > 1000) e.push_back("zone '" + z.id + "': at most 1000 obstacles");
    for (const auto& o : z.obstacles)
      if (o.radius <= 0.0f || o.radius > 100.0f) e.push_back("zone '" + z.id + "': obstacle radius must be (0, 100]");
    for (const auto& sp : z.spawns) {
      if (!c.find_mob(sp.mob)) e.push_back("zone '" + z.id + "': spawn references unknown mob '" + sp.mob + "'");
      if (sp.count < 1 || sp.count > 20) e.push_back("zone '" + z.id + "': spawn count for '" + sp.mob + "' must be 1..20");
      if (sp.radius < 0.0f || sp.radius > 100.0f) e.push_back("zone '" + z.id + "': spawn radius for '" + sp.mob + "' must be 0..100");
      if (sp.x < z.min_x || sp.x > z.max_x || sp.z < z.min_z || sp.z > z.max_z)
        e.push_back("zone '" + z.id + "': spawn for '" + sp.mob + "' is outside bounds");
    }
  }
  for (const auto& city : c.cities) {
    const auto* z = c.find_zone(city.zone);
    if (!z) {
      e.push_back("city '" + city.id + "': unknown zone '" + city.zone + "'");
      continue;
    }
    const auto& s = city.spawn;
    if (s.x < z->min_x || s.x > z->max_x || s.z < z->min_z || s.z > z->max_z) e.push_back("city '" + city.id + "': spawn is outside zone bounds");
  }
  for (const auto& r : c.races) {
    if (r.allowed_classes.empty()) e.push_back("race '" + r.id + "': allowed_classes must not be empty");
    if (r.cities.empty()) e.push_back("race '" + r.id + "': cities must not be empty");
    if (r.allowed_classes.size() > 32 || r.cities.size() > 8) e.push_back("race '" + r.id + "': too many classes (max 32) or cities (max 8)");
    for (const auto& cl : r.allowed_classes)
      if (!c.find_class(cl)) e.push_back("race '" + r.id + "': unknown class '" + cl + "'");
    for (const auto& ci : r.cities)
      if (!c.find_city(ci)) e.push_back("race '" + r.id + "': unknown city '" + ci + "'");
    for (int v : {r.stats.str, r.stats.sta, r.stats.agi, r.stats.dex, r.stats.wis, r.stats.intel, r.stats.cha})
      if (v < 1 || v > 255) {
        e.push_back("race '" + r.id + "': stats must be 1..255");
        break;
      }
  }
  for (const auto& cl : c.classes) {
    const auto& k = cl.combat;
    if (k.hp_base < 1.0f || k.hp_per_level <= 0.0f) e.push_back("class '" + cl.id + "': hp_base must be >= 1 and hp_per_level > 0");
    if (k.weapon_delay < 0.5f || k.weapon_delay > 10.0f) e.push_back("class '" + cl.id + "': weapon_delay must be 0.5..10 s");
    if (k.weapon_damage_base < 1.0f) e.push_back("class '" + cl.id + "': weapon_damage_base must be >= 1");
    if (k.crit_chance < 0.0f || k.crit_chance > 0.5f) e.push_back("class '" + cl.id + "': crit_chance must be 0..0.5");
    for (const auto& [sid, cap] : cl.skills) {
      if (!c.find_skill(sid)) e.push_back("class '" + cl.id + "': unknown skill '" + sid + "'");
      if (cap.level < 1 || cap.level > R.max_level || cap.per_level <= 0.0f || cap.max < 1 || cap.max > 1000)
        e.push_back("class '" + cl.id + "': skill '" + sid + "' has an invalid cap");
    }
    if (!cl.skill("offense") || !cl.skill("defense")) e.push_back("class '" + cl.id + "': needs offense and defense skills");
    if (k.threat_mult <= 0.0f || k.threat_mult > 10.0f) e.push_back("class '" + cl.id + "': threat_mult must be in (0, 10]");
    if (std::find_if(kArmorTypes.begin(), kArmorTypes.end(), [&](const char* t) { return cl.armor == t; }) == kArmorTypes.end())
      e.push_back("class '" + cl.id + "': armor must be cloth, leather, chain, or plate");
    for (const auto& wpn : cl.weapons)
      if (std::find_if(kWeaponTypes.begin(), kWeaponTypes.end(), [&](const char* t) { return wpn == t; }) == kWeaponTypes.end())
        e.push_back(cat({"class '", cl.id, "': unknown weapon type '", wpn, "'"}));
  }
  std::vector<const AbilityDef*> all_abilities;
  all_abilities.reserve(c.abilities.size() + c.spells.size());
  for (const auto& a : c.abilities) all_abilities.push_back(&a);
  for (const auto& a : c.spells) all_abilities.push_back(&a);
  for (const AbilityDef* ap : all_abilities) {
    const AbilityDef& a = *ap;
    const auto* cl = c.find_class(a.class_id);
    if (!cl) e.push_back("ability '" + a.id + "': unknown class '" + a.class_id + "'");
    if (a.level < 1 || a.level > R.max_level) e.push_back("ability '" + a.id + "': level must be 1..max_level");
    static const std::set<std::string, std::less<>> kTypes{"nuke", "heal", "dot", "hot", "buff", "debuff", "cc", "pet", "utility", "passive", "discipline"};
    if (!kTypes.contains(a.type)) e.push_back("ability '" + a.id + "': unknown type '" + a.type + "'");
    if (!a.skill.empty()) {
      if (!c.find_skill(a.skill)) e.push_back("ability '" + a.id + "': unknown skill '" + a.skill + "'");
      const SkillCap* cap = cl ? cl->skill(a.skill) : nullptr;
      if (cl && !cap) e.push_back("ability '" + a.id + "': class '" + cl->id + "' lacks skill '" + a.skill + "'");
      if (a.passive() && cap && cap->level != a.level)
        e.push_back("ability '" + a.id + "': passive level must match the class skill unlock level (" + std::to_string(cap->level) + ")");
    }
    if (a.passive()) continue;
    if (a.cost < 0 || a.cost > 10000) e.push_back("ability '" + a.id + "': cost must be 0..10000");
    if (a.cooldown < 0.0f || a.cooldown > 86400.0f) e.push_back("ability '" + a.id + "': cooldown must be 0..86400 s");
    if (a.range < 0.0f || a.range > 200.0f) e.push_back("ability '" + a.id + "': range must be 0..200");
    if (a.cast_time < 0.0f || a.cast_time > 30.0f) e.push_back("ability '" + a.id + "': cast_time must be 0..30 s");
    if ((a.target == TargetType::Enemy || a.target == TargetType::Corpse) && a.range <= 0.0f)
      e.push_back("ability '" + a.id + "': enemy and corpse targets need a range");
    if (a.target == TargetType::Area && a.radius <= 0.0f) e.push_back("ability '" + a.id + "': area spells need a radius");
    if (a.radius < 0.0f || a.radius > 100.0f) e.push_back("ability '" + a.id + "': radius must be 0..100");
    if (a.max_targets < 0 || a.max_targets > 50) e.push_back("ability '" + a.id + "': max_targets must be 0..50");
    if (a.rank < 1 || a.rank > 100) e.push_back("ability '" + a.id + "': rank must be 1..100");
    if (!valid_id(a.stack_group)) e.push_back("ability '" + a.id + "': stack_group must be 1-32 chars of [a-z0-9_]");
    if (a.resist_adjust < -1000 || a.resist_adjust > 1000) e.push_back("ability '" + a.id + "': resist_adjust must be -1000..1000");
    if (a.description.size() > 256) e.push_back("ability '" + a.id + "': description must be at most 256 chars");
    if (a.spell && !a.song && a.resource != Resource::Mana) e.push_back("spell '" + a.id + "': spells cost mana");
    if (a.spell && !a.song && cl && cl->combat.mana_per_level <= 0.0f) e.push_back("spell '" + a.id + "': class '" + cl->id + "' has no mana");
    if (a.song) {
      if (!a.spell) e.push_back("ability '" + a.id + "': songs are spells (spells.json)");
      if (a.resource != Resource::None || a.cost != 0 || a.cast_time != 0.0f) e.push_back("song '" + a.id + "': songs have no cost and no cast time");
      if (a.target == TargetType::Corpse || a.has(EffectType::SummonPet) || a.has(EffectType::Charm) || a.has(EffectType::SummonItem))
        e.push_back("song '" + a.id + "': songs cannot resurrect, summon, or charm");
    }
    if (!a.position.empty() && (a.position != "behind" || a.target != TargetType::Enemy))
      e.push_back("ability '" + a.id + "': position must be 'behind' on an enemy-target ability");
    if (a.target == TargetType::Pet && !a.has(EffectType::Heal) && !a.has(EffectType::HealPct) && !a.has(EffectType::Hot) && !a.has(EffectType::Buff))
      e.push_back("ability '" + a.id + "': pet-target abilities heal or buff the pet");
    const bool rez = std::any_of(a.effects.begin(), a.effects.end(), [](const EffectDef& x) { return x.type == EffectType::Resurrect; });
    if (rez != (a.target == TargetType::Corpse)) e.push_back("ability '" + a.id + "': corpse targets go with a resurrect effect, and only them");
    for (const auto& ef : a.effects) {
      check_effect(a, ef, e);
      if (ef.type == EffectType::Teleport && ef.destination != "safe_spot" && ef.destination != "bind" && !c.find_city(ef.destination))
        e.push_back("ability '" + a.id + "': unknown teleport destination '" + ef.destination + "'");
      if (ef.type == EffectType::SummonPet && !ef.ref.empty() && !c.find_pet(ef.ref)) e.push_back(cat({"ability '", a.id, "': unknown pet '", ef.ref, "'"}));
      if (ef.type == EffectType::SummonItem && !ef.ref.empty()) {
        const ItemDef* it = c.find_item(ef.ref);
        if (!it) e.push_back(cat({"ability '", a.id, "': unknown item '", ef.ref, "'"}));
        else if (!it->summoned) e.push_back(cat({"ability '", a.id, "': item '", ef.ref, "' must be marked summoned"}));
        else if (ef.value > static_cast<float>(it->stack)) e.push_back(cat({"ability '", a.id, "': summons more '", ef.ref, "' than one stack holds"}));
      }
      if (ef.type == EffectType::Illusion && !ef.ref.empty() && !c.find_race(ef.ref) && !c.find_model(ef.ref) && !c.models.empty())
        e.push_back(cat({"ability '", a.id, "': illusion '", ef.ref, "' is neither a race nor a model"}));
    }
  }
  // Loot tables that can award an item: used by a mob that some zone spawns, with max_drops > 0.
  std::set<std::string> live_tables;
  for (const auto& z : c.zones)
    for (const auto& sp : z.spawns)
      if (const MobDef* m = c.find_mob(sp.mob); m && !m->loot.empty()) live_tables.insert(m->loot);
  // Spells: each one must be learnable at its level (a starter spell, sold by a trainer, or dropped).
  for (const auto& s : c.spells) {
    // Any scroll that teaches the spell counts as a route.
    std::vector<const ItemDef*> scrolls;
    for (const auto& it : c.items)
      if (it.kind == "scroll" && it.spell == s.id) scrolls.push_back(&it);
    const ClassDef* cl = c.find_class(s.class_id);
    const bool starter = cl && std::find(cl->starter_spells.begin(), cl->starter_spells.end(), s.id) != cl->starter_spells.end();
    bool sold = false;
    bool dropped = false;
    for (const ItemDef* scroll : scrolls) {
      for (const auto& n : c.npcs)
        if (c.find_zone(n.zone) && std::find(n.sells.begin(), n.sells.end(), scroll->id) != n.sells.end() &&
            std::find(n.classes.begin(), n.classes.end(), s.class_id) != n.classes.end())
          sold = true;
      for (const auto& t : c.loot_tables)
        if (t.max_drops > 0 && live_tables.contains(t.id))
          for (const auto& d : t.drops)
            if (d.item == scroll->id && d.chance > 0.0f) dropped = true;
    }
    if (!starter && !sold && !dropped) e.push_back("spell '" + s.id + "': not reachable (no starter grant, trainer, or loot drop teaches it)");
  }
  for (const auto& cl : c.classes) {
    for (const auto& sid : cl.starter_spells) {
      const AbilityDef* sp = c.find_spell(sid);
      if (!sp) e.push_back("class '" + cl.id + "': unknown starter spell '" + sid + "'");
      else if (sp->class_id != cl.id || sp->level != 1) e.push_back("class '" + cl.id + "': starter spell '" + sid + "' must be its own level-1 spell");
    }
    // Starters are memorized at level 1, so they must fit the gems open at level 1, and the book.
    const std::set<std::string> distinct(cl.starter_spells.begin(), cl.starter_spells.end());
    const auto level1_gems = static_cast<std::size_t>(std::count(R.spells.gem_unlock_levels.begin(), R.spells.gem_unlock_levels.end(), 1));
    if (distinct.size() > level1_gems) e.push_back("class '" + cl.id + "': more starter spells than gems open at level 1");
    if (distinct.size() > static_cast<std::size_t>(std::max(0, R.spells.spellbook_size))) e.push_back("class '" + cl.id + "': starter spells do not fit the spellbook");
    // Every class that has any abilities or spells has some in each level band.
    int total = 0;
    std::array<int, kLevelBands.size()> per_band{};
    for (const AbilityDef* a : all_abilities) {
      if (a->class_id != cl.id) continue;
      ++total;
      for (std::size_t b = 0; b < kLevelBands.size(); ++b)
        if (a->level >= kLevelBands[b].first && a->level <= kLevelBands[b].second) ++per_band[b];
    }
    if (total == 0) {
      // Partial test content leaves classes empty; shipped content (min_class_abilities > 0) may not.
      if (R.content.min_class_abilities > 0) e.push_back("class '" + cl.id + "': has no abilities or spells");
      continue;
    }
    for (std::size_t b = 0; b < kLevelBands.size(); ++b)
      if (per_band[b] == 0 && kLevelBands[b].first <= R.max_level)
        e.push_back("class '" + cl.id + "': no abilities in levels " + std::to_string(kLevelBands[b].first) + "-" + std::to_string(kLevelBands[b].second));
    if (R.content.min_class_abilities > 0 && total < R.content.min_class_abilities)
      e.push_back("class '" + cl.id + "': " + std::to_string(total) + " abilities and spells, needs at least " + std::to_string(R.content.min_class_abilities));
    if (R.content.max_level_gap > 0) {
      // Something new at level 1 and then at least every max_level_gap levels up to max_level.
      std::set<int> levels;
      for (const AbilityDef* a : all_abilities)
        if (a->class_id == cl.id) levels.insert(a->level);
      int prev = 1;
      if (!levels.contains(1)) e.push_back("class '" + cl.id + "': needs an ability at level 1");
      for (int l : levels) {
        if (l - prev > R.content.max_level_gap)
          e.push_back("class '" + cl.id + "': nothing new between levels " + std::to_string(prev) + " and " + std::to_string(l));
        prev = l;
      }
      if (R.max_level - prev > R.content.max_level_gap)
        e.push_back("class '" + cl.id + "': nothing new after level " + std::to_string(prev));
    }
  }
  for (const auto& it : c.items) {
    if (it.stack < 1 || it.stack > 1000) e.push_back("item '" + it.id + "': stack must be 1..1000");
    if (it.value < 0) e.push_back("item '" + it.id + "': value must be >= 0");
    if ((it.kind == "scroll") != !it.spell.empty()) e.push_back("item '" + it.id + "': scrolls (and only scrolls) name a spell");
    if (!it.spell.empty() && !c.find_spell(it.spell)) e.push_back("item '" + it.id + "': unknown spell '" + it.spell + "'");
    if (it.summoned && it.kind == "scroll") e.push_back("item '" + it.id + "': scrolls cannot be summoned items");
  }
  for (const auto& t : c.loot_tables)
    for (const auto& d : t.drops)
      if (const ItemDef* it = c.find_item(d.item); it && it->summoned)
        e.push_back(cat({"loot table '", t.id, "': summoned item '", d.item, "' cannot drop"}));
  for (const auto& pt : c.pets) {
    const std::string w = "pet '" + pt.id + "': ";
    static const std::set<std::string, std::less<>> kVariants{"elemental", "undead", "spirit", "animated", "beast"};
    if (!kVariants.contains(pt.variant)) e.push_back(w + "variant must be elemental, undead, spirit, animated, or beast");
    if (!c.models.empty() && !c.find_model(pt.model)) e.push_back(cat({w, "unknown model '", pt.model, "'"}));
    if (pt.name.empty() || pt.name.size() > 24) e.push_back(w + "name must be 1..24 chars");
    if (pt.scale < 0.1f || pt.scale > 10.0f) e.push_back(w + "scale must be 0.1..10");
    if (pt.hp_mult <= 0.0f || pt.damage_mult <= 0.0f || pt.threat_mult <= 0.0f) e.push_back(w + "hp_mult, damage_mult and threat_mult must be > 0");
    if (pt.attack_delay < 0.5f || pt.attack_delay > 10.0f) e.push_back(w + "attack_delay must be 0.5..10 s");
    if (pt.run_speed <= 0.0f || pt.run_speed > 20.0f) e.push_back(w + "run_speed must be in (0, 20]");
    if (pt.damage_shield_per_level < 0.0f || pt.damage_shield_per_level > 20.0f) e.push_back(w + "damage_shield_per_level must be 0..20");
    if (!valid_id(pt.body)) e.push_back(w + "body must be 1-32 chars of [a-z0-9_]");
    for (int r : pt.resists)
      if (r < -1 || r > 1000) e.push_back(w + "resists must be 0..1000");
  }
  for (const auto& n : c.npcs) {
    const std::string w = "npc '" + n.id + "': ";
    if (n.role != "trainer" && n.role != "lockbox") e.push_back(w + "role must be 'trainer' or 'lockbox'");
    if (n.level < 1 || n.level > 255) e.push_back(w + "level must be 1..255");
    if (n.greeting.size() > 256) e.push_back(w + "greeting must be at most 256 chars");
    const ZoneDef* z = c.find_zone(n.zone);
    if (!z) e.push_back(w + "unknown zone '" + n.zone + "'");
    else if (n.x < z->min_x || n.x > z->max_x || n.z < z->min_z || n.z > z->max_z) e.push_back(w + "position is outside the zone");
    if (n.role == "lockbox") {
      if (n.lock_level < 1 || n.lock_level > 255) e.push_back(w + "a lockbox needs a lock_level of 1..255");
      if (!n.classes.empty() || !n.sells.empty()) e.push_back(w + "a lockbox teaches and sells nothing");
      continue;
    }
    if (n.classes.empty()) e.push_back(w + "a trainer needs at least one class");
    for (const auto& cl : n.classes)
      if (!c.find_class(cl)) e.push_back(cat({w, "unknown class '", cl, "'"}));
    if (n.sells.size() > 128) e.push_back(w + "sells at most 128 items");
    for (const auto& id : n.sells) {
      const ItemDef* it = c.find_item(id);
      if (!it) {
        e.push_back(cat({w, "sells unknown item '", id, "'"}));
        continue;
      }
      const AbilityDef* sp = it->kind == "scroll" ? c.find_spell(it->spell) : nullptr;
      if (sp && std::find(n.classes.begin(), n.classes.end(), sp->class_id) == n.classes.end())
        e.push_back(cat({w, "sells '", id, "' to a class it does not teach"}));
    }
  }
  for (const auto& t : c.loot_tables) {
    if (t.coin_min < 0 || t.coin_max < t.coin_min) e.push_back("loot table '" + t.id + "': need 0 <= coin_min <= coin_max");
    if (t.max_drops < 0 || t.max_drops > 16) e.push_back("loot table '" + t.id + "': max_drops must be 0..16");
    for (const auto& d : t.drops) {
      const auto* item = c.find_item(d.item);
      if (!item) e.push_back("loot table '" + t.id + "': unknown item '" + d.item + "'");
      if (d.chance < 0.0f || d.chance > 1.0f) e.push_back("loot table '" + t.id + "': chance must be 0..1");
      if (d.min < 1 || d.max < d.min || (item && d.max > item->stack)) e.push_back("loot table '" + t.id + "': need 1 <= min <= max <= stack");
    }
  }
  for (const auto& m : c.mobs) {
    const std::string w = "mob '" + m.id + "': ";
    if (m.level < 1 || m.level > R.max_level) e.push_back(w + "level must be 1..max_level");
    if (m.scale < 0.1f || m.scale > 10.0f) e.push_back(w + "scale must be 0.1..10");
    if (m.hp_mult <= 0.0f || m.damage_mult <= 0.0f) e.push_back(w + "hp_mult and damage_mult must be > 0");
    if (m.attack_delay < 0.5f || m.attack_delay > 10.0f) e.push_back(w + "attack_delay must be 0.5..10 s");
    if (m.run_speed <= 0.0f || m.run_speed > 20.0f) e.push_back(w + "run_speed must be in (0, 20]");
    if (m.aggro_radius < 0.0f || m.aggro_radius > 100.0f || m.assist_radius < 0.0f || m.assist_radius > 100.0f)
      e.push_back(w + "aggro/assist radius must be 0..100");
    if (m.flee_hp_pct < 0.0f || m.flee_hp_pct >= 1.0f) e.push_back(w + "flee_hp_pct must be in [0, 1)");
    if (m.wander_radius < 0.0f || m.leash_radius < 10.0f) e.push_back(w + "wander_radius must be >= 0 and leash_radius >= 10");
    if (m.respawn_seconds < 1.0f) e.push_back(w + "respawn_seconds must be >= 1");
    if (m.faction.empty()) e.push_back(w + "faction is required");
    if (!m.loot.empty() && !c.find_loot(m.loot)) e.push_back(w + "unknown loot table '" + m.loot + "'");
    if (!valid_id(m.body)) e.push_back(w + "body must be 1-32 chars of [a-z0-9_]");
    for (int r : m.resists)
      if (r < -1 || r > 1000) e.push_back(w + "resists must be 0..1000");
  }
  return e;
}

Content load_content(const std::filesystem::path& dir) {
  std::vector<std::string> errors;
  ContentDocs d;
  d.rules = read_json(dir / "rules.json", errors);
  d.races = read_json(dir / "races.json", errors);
  d.classes = read_json(dir / "classes.json", errors);
  d.cities = read_json(dir / "cities.json", errors);
  d.zones = read_json(dir / "zones.json", errors);
  d.skills = read_json(dir / "skills.json", errors);
  d.abilities = read_json(dir / "abilities.json", errors);
  d.items = read_json(dir / "items.json", errors);
  d.loot = read_json(dir / "loot.json", errors);
  d.mobs = read_json(dir / "mobs.json", errors);
  d.spells = read_json(dir / "spells.json", errors);
  d.npcs = read_json(dir / "npcs.json", errors);
  d.pets = read_json(dir / "pets.json", errors);
  d.models = read_json(dir / "models.json", errors);
  d.appearance = read_json(dir / "appearance.json", errors);
  // parse_content treats a null document as "absent" (handy in tests); on disk every file must be
  // an object with its list.
  const std::pair<const char*, const nlohmann::json*> files[] = {
      {"rules.json", &d.rules}, {"races.json", &d.races},         {"classes.json", &d.classes}, {"cities.json", &d.cities},
      {"zones.json", &d.zones}, {"skills.json", &d.skills},       {"abilities.json", &d.abilities}, {"items.json", &d.items},
      {"loot.json", &d.loot},   {"mobs.json", &d.mobs},             {"spells.json", &d.spells},   {"npcs.json", &d.npcs},
      {"pets.json", &d.pets},
      {"models.json", &d.models}, {"appearance.json", &d.appearance}};
  for (const auto& [name, doc] : files)
    if (!doc->is_object()) errors.push_back(std::string(name) + ": must be a JSON object");
  // Visual checks are skipped when no models are defined (partial test content); on disk the
  // catalog must not be empty, or every model reference would go unchecked.
  if (d.models.is_object() && d.models.contains("models") && d.models.at("models").is_array() && d.models.at("models").empty())
    errors.emplace_back("models.json: 'models' must list at least one model");
  if (!errors.empty()) throw_errors(errors);
  return parse_content(d);
}

}  // namespace ef::game
