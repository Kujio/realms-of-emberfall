// Zone tradeskill intents (Phase 13): using a crafting station and combining a recipe at it. The rules
// themselves are pure functions in crafting.cpp; a combine marks the character for saving like any other
// inventory change.
#include <algorithm>
#include <iterator>
#include <utility>

#include "game/crafting.hpp"
#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint16_t u16(int v) { return static_cast<std::uint16_t>(std::clamp(v, 0, 65535)); }

std::string counted(int n, const std::string& name) { return n > 1 ? std::to_string(n) + " x " + name : name; }

}  // namespace

int Zone::craft_skill(const PlayerEntity& p, std::string_view tradeskill) const {
  for (const auto& s : p.crafts)
    if (s.id == tradeskill) return std::clamp(s.value, 0, content_.crafts.rules.max_skill);
  return 0;
}

void Zone::send_craft_skills(PlayerEntity& p, ZoneSink& sink, bool announce, std::string_view only) {
  proto::CraftSkills msg;
  msg.announce = announce;
  const int cap = content_.crafts.rules.max_skill;
  for (const auto& t : content_.crafts.tradeskills) {
    if (msg.skills.size() >= 16) break;
    if (!only.empty() && t.id != only) continue;
    msg.skills.push_back({t.id, t.name, u16(craft_skill(p, t.id)), u16(cap)});
  }
  send(sink, p, msg);
}

void Zone::craft_open(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (items_locked(p, sink)) return;
  auto it = npcs_.find(npc);
  if (it == npcs_.end() || it->second.def->role != "station" || !std::binary_search(p.known.begin(), p.known.end(), npc))
    return say("That is not a crafting station.");
  const NpcEntity* n = npc_in_reach(p, npc, "station");
  if (!n) return say("You are too far away to use the " + it->second.def->name + ".");
  const TradeskillDef* t = content_.crafts.for_station(n->def->station);
  if (!t) return say("Nothing can be made at the " + n->def->name + ".");
  p.crafting = npc;
  send(sink, p, proto::CraftWindow{n->id, n->def->name, t->id});
}

void Zone::craft_combine(EntityId player, EntityId npc, std::string_view recipe_id, ZoneSink& sink, std::uint16_t request) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  // Every combine is answered: a refusal says why in chat and tells the window it may send another at once.
  const std::string asked = std::string(recipe_id.substr(0, 48));
  auto say = [&](const std::string& text) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", text});
    send(sink, p, proto::CraftResult{asked, false, 0, request, true});
  };
  if (items_locked(p, sink)) return send(sink, p, proto::CraftResult{asked, false, 0, request, true});
  const NpcEntity* n = npc_in_reach(p, npc, "station");
  if (!n) return say("You must stand at a crafting station to do that.");
  const TradeskillDef* t = content_.crafts.for_station(n->def->station);
  const RecipeDef* r = content_.crafts.find_recipe(recipe_id);
  if (!t || !r) return say("There is no such recipe.");
  const std::string made = item_name(r->result.item);
  if (r->tradeskill != t->id) {
    const TradeskillDef* other = content_.crafts.find_tradeskill(r->tradeskill);
    return say(made + " is not made at the " + n->def->name + (other ? "; it needs a " + other->station_name + "." : "."));
  }
  if (p.cast.casting()) return say("You cannot do that while casting.");
  if (tick_ < p.next_combine_tick) return say("You are still at work on the last one.");
  const CombinePlan plan = check_combine(p.inv, content_, *r);
  switch (plan.check) {
    case CombineCheck::MissingIngredient:
      return say("You need " + counted(plan.need, item_name(plan.item)) + " for that (you have " + std::to_string(plan.have) + ").");
    case CombineCheck::MissingTool:
      return say("You need a " + item_name(plan.item) + " in your pack for that.");
    case CombineCheck::NoRoom:
      return say("You have no room in your pack for " + counted(r->result.count, made) + ".");
    case CombineCheck::Lore:
      return say(made + " is lore: you already have one.");
    case CombineCheck::Ok:
      break;
  }
  const CraftRules& rules = content_.crafts.rules;
  const int skill = craft_skill(p, t->id);
  const bool ok = rng_.chance(combine_chance(skill, r->trivial, rules));
  if (ok) {
    // On a copy, committed only when the whole result fits (check_combine said it would): never half a combine.
    Inventory after = p.inv;
    for (const auto& in : r->ingredients) take_from_pack(after, in.item, in.count);
    ItemStackState stack{r->result.item, r->result.count, 0};
    if (!game::give_item(after, content_, stack)) return say("You have no room in your pack for " + counted(r->result.count, made) + ".");
    p.inv = std::move(after);
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You have made " + counted(r->result.count, made) + "."});
  } else {
    // Each unit is lost with fail_loss_chance; tools never are.
    std::string lost;
    for (const auto& in : r->ingredients) {
      const int l = units_lost(in.count, rules.fail_loss_chance, rng_);
      if (l <= 0) continue;
      take_from_pack(p.inv, in.item, l);
      lost += (lost.empty() ? "" : ", ") + counted(l, item_name(in.item));
    }
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You lacked the skill to make " + made + "." +
                                                          (lost.empty() ? " Your materials are unharmed." : " You lost " + lost + ".")});
  }
  p.next_combine_tick = tick_ + std::max<std::uint32_t>(1, ticks(rules.combine_seconds));
  p.crafting = npc;
  mark_inventory(p);
  send(sink, p, proto::CraftResult{r->id, ok, ok ? u16(r->result.count) : std::uint16_t{0}, request, false});
  // Either outcome may teach something, until the recipe is trivial.
  if (rng_.chance(craft_skill_up_chance(skill, r->trivial, rules.max_skill, rules))) {
    auto s = std::find_if(p.crafts.begin(), p.crafts.end(), [&](const SkillState& c) { return c.id == t->id; });
    if (s == p.crafts.end()) {
      p.crafts.push_back({t->id, 0});
      s = std::prev(p.crafts.end());
    }
    s->value = std::min(rules.max_skill, std::max(s->value, skill) + 1);
    send_craft_skills(p, sink, true, t->id);
  }
}

}  // namespace ef::game
