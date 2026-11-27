// Quests in the zone (Phase 6): talking to quest givers (dialogue with offers, turn-ins and news),
// accepting and completing quests with their rewards, kill credit for the whole loot party, quest
// items that drop only while you need them, and the journal the client shows. The rules are in quests.hpp.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

constexpr std::size_t kMaxOptions = 16;

// The pack and its bags: not the bank, and not what is worn.
bool carry_slot(SlotCode code) { return !in_bank(code) && (code >> 8) != 0; }

std::string cut(const std::string& s, std::size_t n) { return s.size() <= n ? s : s.substr(0, n); }

}  // namespace

int Zone::carried(const PlayerEntity& p, std::string_view item_id) const {
  int n = 0;
  p.inv.for_each([&](SlotCode code, const ItemStackState& st) {
    if (st.item_id == item_id && carry_slot(code)) n += st.count;
  });
  return n;
}

bool Zone::take_carried(PlayerEntity& p, std::string_view item_id, int count) {
  if (count <= 0) return true;
  if (carried(p, item_id) < count) return false;
  int left = count;
  std::vector<SlotCode> slots;
  p.inv.for_each([&](SlotCode code, const ItemStackState& st) {
    if (st.item_id == item_id && carry_slot(code)) slots.push_back(code);
  });
  for (SlotCode code : slots) {
    if (left <= 0) break;
    ItemStackState* st = p.inv.at(code);
    if (!st) continue;
    const int take = std::min(left, st->count);
    st->count -= take;
    left -= take;
    if (st->count <= 0) *st = ItemStackState{};
  }
  mark_inventory(p);
  p.save_dirty = true;
  return true;
}

std::vector<const QuestDef*> Zone::active_quest_defs(const PlayerEntity& p) const {
  std::vector<const QuestDef*> out;
  for (const auto& a : p.quests.active())
    if (const QuestDef* q = content_.find_quest(a.id)) out.push_back(q);
  return out;
}

void Zone::quest_say(PlayerEntity& p, const NpcDef& n, const std::string& text, ZoneSink& sink) {
  if (!text.empty()) send(sink, p, proto::ChatMessage{kChatSay, n.name, text});
}

// Talk and delivery steps naming this NPC, and collect steps finished at it (the turn-in NPC).
void Zone::advance_at_npc(PlayerEntity& p, const NpcDef& n, ZoneSink& sink) {
  bool moved = true;
  for (int guard = 0; moved && guard < 32; ++guard) {
    moved = false;
    for (const QuestDef* q : active_quest_defs(p)) {
      const ActiveQuest* a = p.quests.find(q->id);
      if (!a || a->step >= static_cast<int>(q->steps.size())) continue;
      const QuestStepDef& s = q->steps[static_cast<std::size_t>(a->step)];
      if (s.type == QuestStepType::Talk && s.npc == n.id) {
        const bool has = s.item.empty() || carried(p, s.item) >= std::max(1, s.count);
        if (p.quests.try_talk(*q, n.id, has)) {
          if (!s.item.empty()) take_carried(p, s.item, std::max(1, s.count));
          quest_say(p, n, s.say, sink);
          send(sink, p, proto::ChatMessage{kChatSystem, "", q->name + ": " + s.text + " - done."});
          p.journal_dirty = true;
          moved = true;
        }
      } else if (s.type == QuestStepType::Collect && q->turn_in_npc() == n.id) {
        if (p.quests.try_collect(*q, carried(p, s.item))) {
          take_carried(p, s.item, s.count);
          send(sink, p, proto::ChatMessage{kChatSystem, "", q->name + ": you hand over " + std::to_string(s.count) + " " + item_name(s.item) + "."});
          p.journal_dirty = true;
          moved = true;
        }
      }
    }
  }
}

void Zone::npc_talk(EntityId player, EntityId npc, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (p.dead) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are dead."});
  auto it = npcs_.find(npc);
  if (it == npcs_.end() || it->second.def->role != "questgiver" || !std::binary_search(p.known.begin(), p.known.end(), npc))
    return send(sink, p, proto::ChatMessage{kChatSystem, "", "They have nothing to say to you."});
  const NpcEntity* n = npc_in_reach(p, npc, "questgiver");
  if (!n) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are too far away to talk to " + it->second.def->name + "."});
  if (npc_refuses(p, *n->def, sink)) return;
  advance_at_npc(p, *n->def, sink);
  send_dialog(p, *n, n->def->greeting, sink);
}

void Zone::send_dialog(PlayerEntity& p, const NpcEntity& n, const std::string& text, ZoneSink& sink) {
  proto::NpcDialog d;
  d.npc_id = n.id;
  d.name = cut(n.def->name, 64);
  d.text = cut(text, 1024);
  auto add = [&](const std::string& key, const std::string& label) {
    if (d.options.size() < kMaxOptions) d.options.push_back({cut(key, 64), cut(label, 96)});
  };
  for (const QuestDef* q : active_quest_defs(p)) {
    if (q->turn_in_npc() != n.def->id) continue;
    if (p.quests.ready(*q)) add("complete:" + q->id, "[Complete] " + q->name);
    else add("status:" + q->id, "[In progress] " + q->name);
  }
  for (const auto& q : content_.quests) {
    if (q.giver != n.def->id) continue;
    const QuestRefusal r = p.quests.can_accept(q, p.level, p.class_id);
    // Offers you can take now, and (to tell you there is more) ones only a few levels away.
    if (r == QuestRefusal::None) add("offer:" + q.id, q.name + " (level " + std::to_string(q.level) + ")");
    else if (r == QuestRefusal::Level && q.min_level <= p.level + 3) add("offer:" + q.id, q.name + " (from level " + std::to_string(q.min_level) + ")");
  }
  send(sink, p, d);
}

void Zone::npc_choose(EntityId player, EntityId npc, std::string_view key, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  if (p.dead) return;
  const NpcEntity* n = npc_in_reach(p, npc, "questgiver");
  if (!n) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are too far away."});
  if (npc_refuses(p, *n->def, sink)) return;
  const auto colon = key.find(':');
  const std::string_view verb = key.substr(0, colon);
  const std::string_view qid = colon == std::string_view::npos ? std::string_view{} : key.substr(colon + 1);
  const QuestDef* q = qid.empty() ? nullptr : content_.find_quest(qid);
  auto back = [&](const std::string& text) { send_dialog(p, *n, text, sink); };
  if (verb == "back" || !q) return back(n->def->greeting);
  if (verb == "offer") {
    if (q->giver != n->def->id) return back(n->def->greeting);
    proto::NpcDialog d;
    d.npc_id = n->id;
    d.name = cut(n->def->name, 64);
    std::string text = q->offer;
    const QuestRefusal r = p.quests.can_accept(*q, p.level, p.class_id);
    if (r == QuestRefusal::Level) text += "\n\n(Come back when you are level " + std::to_string(q->min_level) + ".)";
    d.text = cut(text, 1024);
    if (r == QuestRefusal::None) d.options.push_back({cut("accept:" + q->id, 64), "I will do it."});
    d.options.push_back({"back", "Perhaps another time."});
    return send(sink, p, d);
  }
  if (verb == "accept") {
    if (q->giver != n->def->id) return back(n->def->greeting);
    const QuestRefusal r = p.quests.can_accept(*q, p.level, p.class_id);
    if (r == QuestRefusal::Full) return back("Your mind is already full of errands. Finish or abandon one first.");
    if (r != QuestRefusal::None) return back(n->def->greeting);
    // Whatever the giver hands over must fit; nothing is given unless all of it does.
    Inventory trial = p.inv;
    for (const auto& g : q->give) {
      ItemStackState st{g.item, g.count, 0};
      if (!game::give_item(trial, content_, st)) return back("You have no room for what I would give you. Make some space first.");
    }
    for (const auto& g : q->give) {
      ItemStackState st{g.item, g.count, 0};
      give_item(p, st);
    }
    if (!q->give.empty()) mark_inventory(p);
    p.quests.accept(*q);
    p.journal_dirty = true;
    p.save_dirty = true;
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You have accepted the quest: " + q->name + "."});
    advance_at_npc(p, *n->def, sink);  // a first step that is "talk to me" is already done
    return back(q->accepted.empty() ? n->def->greeting : q->accepted);
  }
  if (verb == "status") {
    if (q->turn_in_npc() != n->def->id) return back(n->def->greeting);
    return back(q->progress.empty() ? "Have you finished what I asked?" : q->progress);
  }
  if (verb == "complete") {
    if (q->turn_in_npc() != n->def->id || !p.quests.ready(*q)) return back(n->def->greeting);
    complete_quest(p, *n, *q, sink);
    return;
  }
  back(n->def->greeting);
}

void Zone::complete_quest(PlayerEntity& p, const NpcEntity& n, const QuestDef& q, ZoneSink& sink) {
  Inventory trial = p.inv;
  for (const auto& r : q.items) {
    ItemStackState st{r.item, r.count, 0};
    if (!game::give_item(trial, content_, st)) return send_dialog(p, n, "Make room in your pack for your reward, then come back.", sink);
  }
  if (!p.quests.complete(q)) return;
  send(sink, p, proto::ChatMessage{kChatSystem, "", "You have completed the quest: " + q.name + "!"});
  for (const auto& r : q.items) {
    ItemStackState st{r.item, r.count, 0};
    give_item(p, st);
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You receive " + (r.count > 1 ? std::to_string(r.count) + " " : std::string()) + item_name(r.item) + "."});
  }
  if (!q.items.empty()) mark_inventory(p);
  if (q.coin > 0) add_coin(p.id, static_cast<std::uint64_t>(q.coin), sink);
  for (const auto& [f, v] : q.faction) adjust_faction(p.id, f, v, sink);
  const int lvl = std::clamp(q.level, 1, rules_.max_level);
  const std::uint64_t step = lvl >= rules_.max_level ? 0 : total_xp_for_level(lvl + 1, rules_.xp) - total_xp_for_level(lvl, rules_.xp);
  if (const std::uint64_t xp = quest_xp(q.xp_levels, step); xp > 0) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", "You gain experience!"});
    award_xp(p, xp, sink);
  }
  p.journal_dirty = true;
  p.save_dirty = true;
  sink.zone_character_changed(p);
  send_dialog(p, n, q.complete, sink);
}

void Zone::quest_abandon(EntityId player, std::string_view quest_id, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p) return;
  const QuestDef* q = content_.find_quest(quest_id);
  if (!q || !p->quests.abandon(quest_id)) return;
  p->journal_dirty = true;
  p->save_dirty = true;
  send(sink, *p, proto::ChatMessage{kChatSystem, "", "You abandon the quest: " + q->name + "."});
}

void Zone::quest_progress(PlayerEntity& p, const std::vector<std::string>& moved, ZoneSink& sink) {
  for (const auto& id : moved) {
    const QuestDef* q = content_.find_quest(id);
    const ActiveQuest* a = p.quests.find(id);
    if (!q || !a) continue;
    p.journal_dirty = true;
    p.save_dirty = true;
    // The step just finished (count reset) or advanced.
    const int done_step = a->count == 0 ? a->step - 1 : a->step;
    if (done_step < 0 || done_step >= static_cast<int>(q->steps.size())) continue;
    const QuestStepDef& s = q->steps[static_cast<std::size_t>(done_step)];
    const int have = a->count == 0 ? s.count : a->count;
    send(sink, p, proto::ChatMessage{kChatSystem, "", q->name + ": " + s.text + " (" + std::to_string(have) + "/" + std::to_string(s.count) + ")"});
  }
}

void Zone::quest_on_craft(PlayerEntity& p, const RecipeDef& r, ZoneSink& sink) {
  if (p.quests.active().empty()) return;
  quest_progress(p, p.quests.on_craft(active_quest_defs(p), r.id), sink);
}

void Zone::quest_on_kill(PlayerEntity& p, const MobDef& m, ZoneSink& sink) {
  if (p.quests.active().empty()) return;
  const auto defs = active_quest_defs(p);
  quest_progress(p, p.quests.on_kill(defs, m.id), sink);
  // Quest items fall only for those who need them.
  for (const QuestDef* q : defs) {
    const ActiveQuest* a = p.quests.find(q->id);
    if (!a || a->step >= static_cast<int>(q->steps.size())) continue;
    const QuestStepDef& s = q->steps[static_cast<std::size_t>(a->step)];
    if (s.type != QuestStepType::Collect || std::find(s.drops_from.begin(), s.drops_from.end(), m.id) == s.drops_from.end()) continue;
    if (carried(p, s.item) >= s.count || !rng_.chance(s.chance)) continue;
    ItemStackState st{s.item, 1, 0};
    if (!give_item(p, st)) {
      send(sink, p, proto::ChatMessage{kChatSystem, "", "You see " + item_name(s.item) + " but have no room to carry it."});
      continue;
    }
    mark_inventory(p);
    p.save_dirty = true;
    p.journal_dirty = true;
    send(sink, p, proto::ChatMessage{kChatSystem, "", q->name + ": you find " + item_name(s.item) + " (" + std::to_string(carried(p, s.item)) + "/" +
                                                      std::to_string(s.count) + ")."});
  }
}

void Zone::send_journal(PlayerEntity& p, ZoneSink& sink) {
  p.journal_dirty = false;
  proto::QuestJournal j;
  j.completed = static_cast<std::uint16_t>(std::min<std::size_t>(p.quests.done().size(), 65535));
  for (const auto& a : p.quests.active()) {
    const QuestDef* q = content_.find_quest(a.id);
    if (!q || j.quests.size() >= 20) continue;
    proto::QuestEntry e;
    e.id = cut(q->id, 48);
    e.name = cut(q->name, 64);
    e.level = static_cast<std::uint8_t>(std::clamp(q->level, 1, 255));
    e.zone = cut(q->zone, 32);
    e.steps = static_cast<std::uint8_t>(q->steps.size());
    e.step = static_cast<std::uint8_t>(std::clamp(a.step, 0, 255));
    e.ready = a.step >= static_cast<int>(q->steps.size());
    e.epic = q->epic;
    const NpcDef* ti = content_.find_npc(q->turn_in_npc());
    e.turn_in = ti ? cut(ti->name, 64) : "";
    if (e.ready) {
      e.text = cut("Return to " + e.turn_in + ".", 128);
    } else {
      const QuestStepDef& s = q->steps[static_cast<std::size_t>(a.step)];
      e.text = cut(s.text, 128);
      e.need = static_cast<std::uint16_t>(s.count);
      if (s.type == QuestStepType::Kill || s.type == QuestStepType::Craft) e.have = static_cast<std::uint16_t>(std::min(a.count, s.count));
      else if (s.type == QuestStepType::Collect) e.have = static_cast<std::uint16_t>(std::min(carried(p, s.item), s.count));
      if (s.type == QuestStepType::Talk) {
        const NpcDef* who = content_.find_npc(s.npc);
        e.turn_in = who ? cut(who->name, 64) : e.turn_in;
        e.need = 0;
      }
    }
    j.quests.push_back(std::move(e));
  }
  send(sink, p, j);
}

}  // namespace ef::game
