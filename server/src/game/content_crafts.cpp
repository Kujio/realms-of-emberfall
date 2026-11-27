// Tradeskill content (Phase 13): content/crafts.json parsing and checks, and the recipe index.
#include <algorithm>
#include <set>

#include "game/content.hpp"
#include "game/content_parser.hpp"

namespace ef::game {

const TradeskillDef* CraftContent::find_tradeskill(std::string_view id) const {
  for (const auto& t : tradeskills)
    if (t.id == id) return &t;
  return nullptr;
}

const TradeskillDef* CraftContent::for_station(std::string_view station) const {
  for (const auto& t : tradeskills)
    if (t.station == station) return &t;
  return nullptr;
}

const RecipeDef* CraftContent::find_recipe(std::string_view id) const {
  if (auto it = recipe_index_.find(std::string(id)); it != recipe_index_.end() && it->second < recipes.size() && recipes[it->second].id == id)
    return &recipes[it->second];
  for (const auto& r : recipes)  // an index out of date (a test edited the list): look it up the slow way
    if (r.id == id) return &r;
  return nullptr;
}

void CraftContent::index() {
  recipe_index_.clear();
  recipe_index_.reserve(recipes.size());
  for (std::size_t i = 0; i < recipes.size(); ++i) recipe_index_.emplace(recipes[i].id, i);
}

namespace content_detail {

namespace {

ItemCount item_count(Parser& p, const json& j, const std::string& where) {
  ItemCount ic;
  ic.item = p.str(j, "item", where);
  ic.count = p.integer(j, "count", where, 1);
  return ic;
}

}  // namespace

void parse_crafts(Parser& p, const ContentDocs& docs, Content& c) {
  CraftContent& k = c.crafts;
  const json& doc = docs.crafts;
  if (doc.is_null()) return;  // partial test content: no tradeskills
  if (!doc.is_object()) {
    p.error("crafts.json: must be a JSON object");
    return;
  }
  if (!doc.contains("rules") || !doc.at("rules").is_object()) {
    p.error("crafts.json: missing object 'rules'");
  } else {
    const json& r = doc.at("rules");
    const std::string w = "crafts.rules";
    CraftRules& cr = k.rules;
    cr.max_skill = p.integer(r, "max_skill", w, cr.max_skill);
    cr.combine_seconds = p.num(r, "combine_seconds", w, cr.combine_seconds);
    cr.success_max = p.num(r, "success_max", w, cr.success_max);
    cr.success_min = p.num(r, "success_min", w, cr.success_min);
    cr.success_per_point = p.num(r, "success_per_point", w, cr.success_per_point);
    cr.skill_up_base = p.num(r, "skill_up_base", w, cr.skill_up_base);
    cr.skill_up_per_point = p.num(r, "skill_up_per_point", w, cr.skill_up_per_point);
    cr.skill_up_max = p.num(r, "skill_up_max", w, cr.skill_up_max);
    cr.fail_loss_chance = p.num(r, "fail_loss_chance", w, cr.fail_loss_chance);
  }
  if (const auto* list = p.list(doc, "tradeskills", "crafts.json")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "crafts.tradeskills[" + std::to_string(i) + "]";
      TradeskillDef t;
      t.id = p.str(j, "id", w);
      t.name = p.str(j, "name", w);
      t.station = p.str(j, "station", w);
      t.station_name = p.str(j, "station_name", w);
      t.description = p.str(j, "description", w, false);
      k.tradeskills.push_back(std::move(t));
    }
  }
  if (const auto* list = p.list(doc, "recipes", "crafts.json")) {
    k.recipes.reserve(list->size());
    for (std::size_t i = 0; i < list->size(); ++i) {
      const auto& j = (*list)[i];
      const std::string w = "crafts.recipes[" + std::to_string(i) + "]";
      RecipeDef r;
      r.id = p.str(j, "id", w);
      r.tradeskill = p.str(j, "tradeskill", w);
      r.trivial = p.integer(j, "trivial", w, 0);
      if (!j.is_object() || !j.contains("result") || !j.at("result").is_object()) p.error(w + ": missing object 'result'");
      else r.result = item_count(p, j.at("result"), w + ".result");
      if (!j.is_object() || !j.contains("ingredients") || !j.at("ingredients").is_array()) {
        p.error(w + ": 'ingredients' must be an array");
      } else {
        const auto& ing = j.at("ingredients");
        for (std::size_t n = 0; n < ing.size(); ++n) r.ingredients.push_back(item_count(p, ing[n], w + ".ingredients[" + std::to_string(n) + "]"));
      }
      if (j.is_object() && j.contains("tools")) r.tools = p.str_list(j, "tools", w);
      r.book = p.str(j, "book", w, false);
      k.recipes.push_back(std::move(r));
    }
  }
  k.index();
}

void validate_crafts(const Content& c, std::vector<std::string>& e) {
  const CraftContent& k = c.crafts;
  const CraftRules& r = k.rules;
  if (r.max_skill < 1 || r.max_skill > 1000) e.emplace_back("crafts.rules.max_skill must be 1..1000");
  if (r.combine_seconds < 0.1f || r.combine_seconds > 60.0f) e.emplace_back("crafts.rules.combine_seconds must be 0.1..60");
  auto unit = [](float v) { return v >= 0.0f && v <= 1.0f; };
  if (!unit(r.success_min) || !unit(r.success_max) || r.success_min > r.success_max)
    e.emplace_back("crafts.rules: success_min and success_max must satisfy 0 <= success_min <= success_max <= 1");
  if (!unit(r.success_per_point)) e.emplace_back("crafts.rules.success_per_point must be 0..1");
  if (!unit(r.skill_up_base) || !unit(r.skill_up_max) || r.skill_up_base > r.skill_up_max)
    e.emplace_back("crafts.rules: skill_up_base and skill_up_max must satisfy 0 <= skill_up_base <= skill_up_max <= 1");
  if (!unit(r.skill_up_per_point)) e.emplace_back("crafts.rules.skill_up_per_point must be 0..1");
  if (!unit(r.fail_loss_chance)) e.emplace_back("crafts.rules.fail_loss_chance must be 0..1");

  std::set<std::string, std::less<>> ids;
  std::set<std::string, std::less<>> station_kinds;
  for (const auto& t : k.tradeskills) {
    const std::string w = "tradeskill '" + t.id + "': ";
    if (t.id.empty() || t.id.size() > 32) e.push_back(w + "id must be 1..32 characters");
    if (t.name.empty() || t.name.size() > 32) e.push_back(w + "name must be 1..32 characters");
    if (t.station.empty() || t.station.size() > 32) e.push_back(w + "station must be 1..32 characters");
    if (t.station_name.empty() || t.station_name.size() > 48) e.push_back(w + "station_name must be 1..48 characters");
    if (t.description.size() > 256) e.push_back(w + "description must be at most 256 characters");
    if (!ids.insert(t.id).second) e.push_back(w + "listed twice");
    if (!station_kinds.insert(t.station).second) e.push_back(w + "shares its station kind '" + t.station + "' with another tradeskill");
    // Tradeskills are saved beside the combat skills (one map per character): the ids must not meet.
    if (c.find_skill(t.id)) e.push_back(w + "has the id of a combat skill in skills.json");
  }
  if (k.tradeskills.size() > 16) e.emplace_back("crafts.tradeskills: at most 16");

  std::set<std::string, std::less<>> recipe_ids;
  std::set<std::string, std::less<>> used;  // tradeskills with a recipe
  std::set<std::string, std::less<>> taught;  // books with a recipe
  for (const auto& rc : k.recipes) {
    const std::string w = "recipe '" + rc.id + "': ";
    if (rc.id.empty() || rc.id.size() > 48) e.push_back(w + "id must be 1..48 characters");
    if (!recipe_ids.insert(rc.id).second) e.push_back(w + "listed twice");
    if (!k.find_tradeskill(rc.tradeskill)) e.push_back(w + "unknown tradeskill '" + rc.tradeskill + "'");
    used.insert(rc.tradeskill);
    if (rc.trivial < 1 || rc.trivial > r.max_skill) e.push_back(w + "trivial must be 1..max_skill");
    const ItemDef* out = c.find_item(rc.result.item);
    if (!out) e.push_back(w + "makes unknown item '" + rc.result.item + "'");
    else {
      if (rc.result.count < 1 || rc.result.count > std::max(1, out->stack)) e.push_back(w + "makes 1..stack of its item");
      if (out->summoned) e.push_back(w + "cannot make a summoned item");
      if (out->lore && rc.result.count != 1) e.push_back(w + "makes a lore item, so only one at a time");
    }
    if (rc.ingredients.empty() || rc.ingredients.size() > 8) e.push_back(w + "needs 1..8 ingredients");
    std::set<std::string, std::less<>> seen;
    for (const auto& in : rc.ingredients) {
      const ItemDef* d = c.find_item(in.item);
      if (!d) e.push_back(w + "uses unknown item '" + in.item + "'");
      else if (d->summoned || d->is_bag()) e.push_back(w + "cannot use '" + in.item + "' (summoned items and bags are not materials)");
      if (in.count < 1 || in.count > 100) e.push_back(w + "uses 1..100 of each ingredient");
      if (!seen.insert(in.item).second) e.push_back(w + "lists '" + in.item + "' twice");
      if (in.item == rc.result.item) e.push_back(w + "uses what it makes");
    }
    if (rc.tools.size() > 4) e.push_back(w + "needs at most 4 tools");
    for (const auto& tool : rc.tools) {
      if (!c.find_item(tool)) e.push_back(w + "needs unknown tool '" + tool + "'");
      if (seen.contains(tool)) e.push_back(w + "lists '" + tool + "' as both a tool and an ingredient");
    }
    if (!rc.book.empty()) {  // Phase 16
      const ItemDef* b = c.find_item(rc.book);
      if (!b || b->kind != "book") e.push_back(w + "is taught by '" + rc.book + "', which is not a book");
      else if (b->tradeskill != rc.tradeskill) e.push_back(w + "is taught by '" + rc.book + "', a book of another tradeskill");
      else taught.insert(rc.book);
    }
  }
  // Phase 16: every book names a real tradeskill and teaches at least one recipe.
  for (const auto& it : c.items) {
    if (it.kind != "book") continue;
    const std::string w = "item '" + it.id + "': ";
    if (!k.find_tradeskill(it.tradeskill)) e.push_back(w + "a book of unknown tradeskill '" + it.tradeskill + "'");
    else if (!taught.contains(it.id)) e.push_back(w + "a book that teaches no recipe");
  }

  // Stations: a known station kind, nothing to teach, sell or travel to; and every tradeskill with
  // recipes has at least one station somewhere.
  std::set<std::string, std::less<>> placed;
  for (const auto& n : c.npcs) {
    if (n.role != "station") continue;
    const std::string w = "npc '" + n.id + "': ";
    if (!k.for_station(n.station)) e.push_back(w + "a station needs the station kind of a tradeskill (got '" + n.station + "')");
    else placed.insert(n.station);
    if (!n.classes.empty() || !n.sells.empty() || !n.stock.empty() || !n.travel.empty())
      e.push_back(w + "a station teaches, sells and offers travel to nothing");
  }
  for (const auto& t : k.tradeskills)
    if (used.contains(t.id) && !placed.contains(t.station)) e.push_back("tradeskill '" + t.id + "': has recipes but no '" + t.station + "' station");
}

}  // namespace content_detail
}  // namespace ef::game
