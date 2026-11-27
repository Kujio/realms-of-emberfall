// Zone tradeskill intents (Phase 13): using a crafting station and combining a recipe at it. The rules
// themselves are pure functions in crafting.cpp; a combine marks the character for saving like any other
// inventory change. Phase 16: a combine takes time (work_step finishes or stops it), and recipes past the
// first tier are learned from books.
#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

#include "game/crafting.hpp"
#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint16_t u16(int v) { return static_cast<std::uint16_t>(std::clamp(v, 0, 65535)); }

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

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
  // Every message (announcements too) carries the whole list of books read: the client replaces its copy.
  for (const auto& b : p.books)
    if (msg.books.size() < kMaxBooks) msg.books.push_back(b);
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

bool Zone::knows_recipe(const PlayerEntity& p, const RecipeDef& r) const {
  return r.book.empty() || std::binary_search(p.books.begin(), p.books.end(), r.book);
}

void Zone::read_book(PlayerEntity& p, SlotCode slot, const ItemDef& book, ZoneSink& sink) {
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  const TradeskillDef* t = content_.crafts.find_tradeskill(book.tradeskill);
  if (!t) return say("The pages of " + book.name + " make no sense to you.");
  if (std::binary_search(p.books.begin(), p.books.end(), book.id)) return say("You already know everything in " + book.name + ".");
  if (p.books.size() >= kMaxBooks) return say("You cannot take in another book.");
  int taught = 0;
  for (const auto& r : content_.crafts.recipes)
    if (r.book == book.id) ++taught;
  p.books.insert(std::upper_bound(p.books.begin(), p.books.end(), book.id), book.id);
  ItemStackState* s = p.inv.at(slot);
  if (s && !s->empty() && --s->count <= 0) *s = {};  // read once: the knowledge stays, the book is spent
  mark_inventory(p);
  say("You study " + book.name + " and learn " + std::to_string(taught) + " " + t->name + " recipe" + (taught == 1 ? "" : "s") + ".");
  send_craft_skills(p, sink, false);
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
  if (!knows_recipe(p, *r)) return say("You do not know how to make " + made + ": it is taught in " + item_name(r->book) + ".");
  if (p.work.recipe) return say("You are still at work on the last one.");
  if (p.cast.casting()) return say("You cannot do that while casting.");
  // A song pulses on its own: stop singing first (starting a song stops a combine likewise).
  if (p.song.active()) return say("You cannot craft while singing.");
  if (in_combat(p)) return say("You cannot craft while you are fighting.");
  if (p.dead || p.stunned_until > tick_ || (control_bits(p.buffs) & (kControlMez | kControlFear)) != 0)
    return say("You cannot do that now.");
  if (const std::string why = combine_problem(p, *r); !why.empty()) return say(why);
  // The work takes combine_seconds; work_step() finishes it, or stops it if the crafter moves or fights.
  const std::uint32_t work = std::max<std::uint32_t>(1, ticks(content_.crafts.rules.combine_seconds));
  stand(p);
  p.work = {r, npc, request, tick_, tick_ + work, p.pos};
  p.changed_tick = tick_;  // Phase 17: others see the work begin (kFlag2Working)
  p.crafting = npc;
  send(sink, p, proto::CraftStarted{r->id, request, ms_of(work, dt_)});
}

std::string Zone::combine_problem(const PlayerEntity& p, const RecipeDef& r) const {
  const CombinePlan plan = check_combine(p.inv, content_, r);
  const std::string made = item_name(r.result.item);
  switch (plan.check) {
    case CombineCheck::MissingIngredient:
      return "You need " + counted(plan.need, item_name(plan.item)) + " for that (you have " + std::to_string(plan.have) + ").";
    case CombineCheck::MissingTool:
      return "You need a " + item_name(plan.item) + " in your pack for that.";
    case CombineCheck::NoRoom:
      return "You have no room in your pack for " + counted(r.result.count, made) + ".";
    case CombineCheck::Lore:
      return made + " is lore: you already have one.";
    case CombineCheck::Ok:
      break;
  }
  return "";
}

void Zone::stop_work(PlayerEntity& p, const std::string& why, ZoneSink& sink) {
  if (!p.work.recipe) return;
  const auto w = p.work;
  p.work = {};
  p.changed_tick = tick_;
  send(sink, p, proto::ChatMessage{kChatSystem, "", why});
  send(sink, p, proto::CraftResult{w.recipe->id, false, 0, w.request, true});
}

void Zone::interrupt_work(PlayerEntity& p, ZoneSink& sink) {
  if (p.work.recipe) stop_work(p, "You stop working on " + item_name(p.work.recipe->result.item) + ".", sink);
}

void Zone::work_step(PlayerEntity& p, ZoneSink& sink) {
  if (!p.work.recipe) return;
  const std::string made = item_name(p.work.recipe->result.item);
  // Anything that would interrupt a cast stops the work: moving off the spot, a fight, a spell, a stun.
  if (p.dead) return stop_work(p, "You stop working on " + made + ".", sink);
  if (horizontal_distance(p.pos, p.work.at) > kWorkDrift) return stop_work(p, "You moved, and stop working on " + made + ".", sink);
  // >= : a fight marked on the very tick the work began (after the start's own check) counts too.
  if (p.ever_in_combat && p.last_combat_tick >= p.work.started_tick)
    return stop_work(p, "The fight pulls you away from " + made + ".", sink);
  // A trade's offer must never be spent by a combine landing under it.
  if (p.trade != 0) return stop_work(p, "You stop working on " + made + " to trade.", sink);
  if (p.cast.casting() || p.sitting || p.stunned_until > tick_ || (control_bits(p.buffs) & (kControlMez | kControlFear)) != 0)
    return stop_work(p, "You stop working on " + made + ".", sink);
  if (!npc_in_reach(p, p.work.npc, "station"))
    return stop_work(p, "You stepped away from the station, and stop working on " + made + ".", sink);
  if (tick_ < p.work.done_tick) return;
  const RecipeDef& r = *p.work.recipe;
  const std::uint16_t request = p.work.request;
  p.work = {};
  p.changed_tick = tick_;
  // The pack may have changed during the work (a trade, a drop): the same checks again, nothing used if they fail.
  if (const std::string why = combine_problem(p, r); !why.empty()) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", why});
    send(sink, p, proto::CraftResult{r.id, false, 0, request, true});
    return;
  }
  const TradeskillDef* t = content_.crafts.find_tradeskill(r.tradeskill);
  if (!t) return;
  const CraftRules& rules = content_.crafts.rules;
  const int skill = craft_skill(p, t->id);
  const bool ok = rng_.chance(combine_chance(skill, r.trivial, rules));
  if (ok) {
    // On a copy, committed only when the whole result fits (check_combine said it would): never half a combine.
    Inventory after = p.inv;
    for (const auto& in : r.ingredients) take_from_pack(after, in.item, in.count);
    ItemStackState stack{r.result.item, r.result.count, 0};
    if (!game::give_item(after, content_, stack)) {
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You have no room in your pack for " + counted(r.result.count, made) + "."});
      send(sink, p, proto::CraftResult{r.id, false, 0, request, true});
      return;
    }
    p.inv = std::move(after);
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You have made " + counted(r.result.count, made) + "."});
    quest_on_craft(p, r, sink);
  } else {
    // Each unit is lost with fail_loss_chance; tools never are.
    std::string lost;
    for (const auto& in : r.ingredients) {
      const int l = units_lost(in.count, rules.fail_loss_chance, rng_);
      if (l <= 0) continue;
      take_from_pack(p.inv, in.item, l);
      lost += (lost.empty() ? "" : ", ") + counted(l, item_name(in.item));
    }
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You lacked the skill to make " + made + "." +
                                                          (lost.empty() ? " Your materials are unharmed." : " You lost " + lost + ".")});
  }
  mark_inventory(p);
  send(sink, p, proto::CraftResult{r.id, ok, ok ? u16(r.result.count) : std::uint16_t{0}, request, false});
  // Either outcome may teach something, until the recipe is trivial.
  if (rng_.chance(craft_skill_up_chance(skill, r.trivial, rules.max_skill, rules))) {
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
