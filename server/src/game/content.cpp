#include "game/content.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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

 private:
  std::vector<std::string>& errors_;
};

[[noreturn]] void throw_errors(const std::vector<std::string>& errors) {
  std::ostringstream os;
  os << "content validation failed (" << errors.size() << " problem" << (errors.size() == 1 ? "" : "s") << "):";
  for (const auto& e : errors) os << "\n  - " << e;
  throw ContentError(os.str());
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
  for (const char* key : {"xp", "con", "combat", "death", "mob_scaling"})
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
}

std::optional<EffectType> effect_type(const std::string& s) {
  static const std::map<std::string, EffectType, std::less<>> kTypes{
      {"damage", EffectType::Damage},       {"stun", EffectType::Stun},
      {"threat", EffectType::Threat},       {"taunt", EffectType::Taunt},
      {"buff", EffectType::Buff},           {"heal_pct", EffectType::HealPct},
      {"restore_endurance_pct", EffectType::RestoreEndurancePct}, {"area_threat", EffectType::AreaThreat}};
  auto it = kTypes.find(s);
  if (it == kTypes.end()) return std::nullopt;
  return it->second;
}

std::optional<BuffStat> buff_stat(const std::string& s) {
  if (s == "damage_taken_pct") return BuffStat::DamageTakenPct;
  if (s == "melee_damage_pct") return BuffStat::MeleeDamagePct;
  if (s == "crit_chance_pct") return BuffStat::CritChancePct;
  return std::nullopt;
}

AbilityDef parse_ability(Parser& p, const json& j, const std::string& w) {
  AbilityDef a;
  a.id = p.str(j, "id", w);
  a.name = p.str(j, "name", w);
  a.class_id = p.str(j, "class", w);
  a.level = p.integer(j, "level", w, 1);
  a.type = p.str(j, "type", w);
  a.description = p.str(j, "description", w, false);
  a.skill = p.str(j, "skill", w, false);
  if (a.passive()) {
    if (a.skill.empty()) p.errors().push_back(w + ": passive abilities must name a skill");
    return a;
  }
  const std::string res = p.str(j, "resource", w, false, "none");
  if (res == "endurance") a.resource = Resource::Endurance;
  else if (res == "mana") a.resource = Resource::Mana;
  else if (res == "none") a.resource = Resource::None;
  else p.errors().push_back(w + ": resource must be none, endurance, or mana");
  a.cost = p.integer(j, "cost", w, 0);
  a.cast_time = p.num(j, "cast_time", w, 0.0f);
  a.cooldown = p.num(j, "cooldown", w, 0.0f);
  a.gcd = p.boolean(j, "gcd", w, true);
  a.range = p.num(j, "range", w, 0.0f);
  const std::string tgt = p.str(j, "target", w);
  if (tgt == "enemy") a.target = TargetType::Enemy;
  else if (tgt == "self") a.target = TargetType::Self;
  else p.errors().push_back(w + ": target must be enemy or self");
  a.resist = p.str(j, "resist", w, false, "none");
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
    ef.value = p.opt(e, "value", we, 0.0f);
    ef.duration = p.opt(e, "duration", we, 0.0f);
    ef.radius = p.opt(e, "radius", we, 0.0f);
    if (ef.type == EffectType::Buff) {
      const auto st = buff_stat(p.str(e, "stat", we));
      if (!st) p.errors().push_back(we + ": unknown buff stat");
      else ef.stat = *st;
      if (ef.duration <= 0.0f) p.errors().push_back(we + ": buff duration must be > 0");
    }
    if (ef.type == EffectType::Stun && ef.duration <= 0.0f) p.errors().push_back(we + ": stun duration must be > 0");
    if (ef.type == EffectType::AreaThreat && ef.radius <= 0.0f) p.errors().push_back(we + ": area_threat radius must be > 0");
    a.effects.push_back(ef);
  }
  return a;
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
      if (cb.contains("stats")) k.stats = p.stats(cb.at("stats"), wc + ".stats", StatBlock{}, false);
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
      c.zones.push_back(std::move(z));
    }
  }
  if (const auto* list = p.list(docs.abilities, "abilities", "abilities.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) c.abilities.push_back(parse_ability(p, (*list)[i], "abilities[" + std::to_string(i) + "]"));
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
      m.loot = p.str(j, "loot", w, false);
      c.mobs.push_back(std::move(m));
    }
  }

  if (!errors.empty()) throw_errors(errors);
  auto problems = validate_content(c);
  if (!problems.empty()) throw_errors(problems);
  return c;
}

std::vector<std::string> validate_content(const Content& c) {
  std::vector<std::string> e;
  const auto& R = c.rules;
  if (R.max_level < 1 || R.max_level > 255) e.emplace_back("rules.max_level must be 1..255");
  if (R.max_characters_per_account < 1 || R.max_characters_per_account > 16) e.emplace_back("rules.max_characters_per_account must be 1..16");
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

  for (const auto& z : c.zones) {
    if (!(z.min_x < z.max_x && z.min_z < z.max_z)) e.push_back("zone '" + z.id + "': bounds min must be < max");
    if (z.interest_radius < 10.0f || z.interest_radius > 500.0f) e.push_back("zone '" + z.id + "': interest_radius must be 10..500");
    if (z.say_radius <= 0.0f) e.push_back("zone '" + z.id + "': say_radius must be > 0");
    const auto& s = z.safe_spawn;
    if (s.x < z.min_x || s.x > z.max_x || s.z < z.min_z || s.z > z.max_z) e.push_back("zone '" + z.id + "': safe_spawn is outside bounds");
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
  }
  for (const auto& a : c.abilities) {
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
    if (a.cast_time != 0.0f) e.push_back("ability '" + a.id + "': cast times arrive with the spell engine (Phase 3); use 0");
    if (a.target == TargetType::Enemy && a.range <= 0.0f) e.push_back("ability '" + a.id + "': enemy-target abilities need a range");
    if (a.description.size() > 256) e.push_back("ability '" + a.id + "': description must be at most 256 chars");
  }
  for (const auto& it : c.items) {
    if (it.stack < 1 || it.stack > 1000) e.push_back("item '" + it.id + "': stack must be 1..1000");
    if (it.value < 0) e.push_back("item '" + it.id + "': value must be >= 0");
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
  // parse_content treats a null document as "absent" (handy in tests); on disk every file must be
  // an object with its list.
  const std::pair<const char*, const nlohmann::json*> files[] = {
      {"rules.json", &d.rules}, {"races.json", &d.races},         {"classes.json", &d.classes}, {"cities.json", &d.cities},
      {"zones.json", &d.zones}, {"skills.json", &d.skills},       {"abilities.json", &d.abilities}, {"items.json", &d.items},
      {"loot.json", &d.loot},   {"mobs.json", &d.mobs}};
  for (const auto& [name, doc] : files)
    if (!doc->is_object()) errors.push_back(std::string(name) + ": must be a JSON object");
  if (!errors.empty()) throw_errors(errors);
  return parse_content(d);
}

}  // namespace ef::game
