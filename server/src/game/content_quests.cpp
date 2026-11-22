// Quest content (Phase 6): content/quests.json parsing and the checks that tie quests to NPCs, mobs,
// items, factions and each other (prerequisites exist and never loop).
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "game/content.hpp"
#include "game/content_parser.hpp"

namespace ef::game {

const QuestDef* Content::find_quest(std::string_view id) const {
  for (const auto& q : quests)
    if (q.id == id) return &q;
  return nullptr;
}

namespace content_detail {

namespace {

std::vector<QuestRewardItem> reward_items(Parser& p, const json& j, const char* key, const std::string& w) {
  std::vector<QuestRewardItem> out;
  if (!j.is_object() || !j.contains(key)) return out;
  const auto& a = j.at(key);
  if (!a.is_array()) {
    p.error(w + ": '" + key + "' must be an array of {item, count}");
    return out;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    const std::string wi = w + "." + key + "[" + std::to_string(i) + "]";
    out.push_back({p.str(a[i], "item", wi), p.integer(a[i], "count", wi, 1, false)});
  }
  return out;
}

}  // namespace

void parse_quests(Parser& p, const ContentDocs& docs, Content& c) {
  const auto* list = p.list(docs.quests, "quests", "quests.json");
  if (!list) return;
  for (std::size_t i = 0; i < list->size(); ++i) {
    const auto& j = (*list)[i];
    const std::string w = "quests[" + std::to_string(i) + "]";
    QuestDef q;
    q.id = p.str(j, "id", w);
    q.name = p.str(j, "name", w);
    q.giver = p.str(j, "giver", w);
    q.turn_in = p.str(j, "turn_in", w, false);
    q.level = p.integer(j, "level", w, 1);
    q.min_level = p.integer(j, "min_level", w, std::max(1, q.level - 5), false);
    if (j.is_object() && j.contains("classes")) q.classes = p.str_list(j, "classes", w);
    if (j.is_object() && j.contains("prereqs")) q.prereqs = p.str_list(j, "prereqs", w);
    q.repeatable = p.boolean(j, "repeatable", w, false, false);
    q.epic = p.boolean(j, "epic", w, false, false);
    q.zone = p.str(j, "zone", w, false);
    q.offer = p.str(j, "offer", w);
    q.accepted = p.str(j, "accepted", w, false);
    q.progress = p.str(j, "progress", w, false);
    q.complete = p.str(j, "complete", w);
    q.give = reward_items(p, j, "give", w);
    if (j.is_object() && j.contains("steps") && j.at("steps").is_array()) {
      const auto& st = j.at("steps");
      for (std::size_t k = 0; k < st.size(); ++k) {
        const std::string ws = w + ".steps[" + std::to_string(k) + "]";
        QuestStepDef s;
        const std::string type = p.str(st[k], "type", ws);
        if (type == "kill") s.type = QuestStepType::Kill;
        else if (type == "collect") s.type = QuestStepType::Collect;
        else if (type == "talk" || type == "deliver") s.type = QuestStepType::Talk;
        else p.error(ws + ": type must be kill, collect, talk or deliver");
        if (st[k].is_object() && st[k].contains("mobs")) s.mobs = p.str_list(st[k], "mobs", ws);
        if (st[k].is_object() && st[k].contains("drops_from")) s.drops_from = p.str_list(st[k], "drops_from", ws);
        s.item = p.str(st[k], "item", ws, false);
        s.count = p.integer(st[k], "count", ws, 1, false);
        s.npc = p.str(st[k], "npc", ws, false);
        s.chance = p.opt(st[k], "chance", ws, 0.5f);
        s.text = p.str(st[k], "text", ws);
        s.say = p.str(st[k], "say", ws, false);
        q.steps.push_back(std::move(s));
      }
    } else {
      p.error(w + ": steps must be a non-empty array");
    }
    if (j.is_object() && j.contains("rewards")) {
      const auto& r = j.at("rewards");
      const std::string wr = w + ".rewards";
      q.xp_levels = p.dbl(r, "xp", wr, 0.0);
      q.coin = p.integer(r, "coin", wr, 0, false);
      q.items = reward_items(p, r, "items", wr);
      if (r.is_object() && r.contains("faction")) {
        const auto& f = r.at("faction");
        if (!f.is_object()) p.error(wr + ": faction must be an object of faction -> change");
        for (auto it = f.begin(); f.is_object() && it != f.end(); ++it) q.faction.emplace_back(it.key(), p.integer(f, it.key().c_str(), wr + ".faction", 0));
      }
    }
    c.quests.push_back(std::move(q));
  }
}

void validate_quests(const Content& c, std::vector<std::string>& e) {
  std::set<std::string> ids;
  auto quest_npc = [&](const std::string& id, const std::string& w, const char* what) {
    const NpcDef* n = c.find_npc(id);
    if (!n) e.push_back(w + what + " '" + id + "' is not an NPC");
    else if (n->role != "questgiver") e.push_back(w + what + " '" + id + "' must have role questgiver");
  };
  for (const auto& q : c.quests) {
    const std::string w = "quest '" + q.id + "': ";
    if (!ids.insert(q.id).second) e.push_back(w + "defined twice");
    if (q.id.size() > 48 || q.name.size() > 64) e.push_back(w + "id at most 48 and name at most 64 characters");
    quest_npc(q.giver, w, "giver");
    if (!q.turn_in.empty()) quest_npc(q.turn_in, w, "turn_in");
    if (q.level < 1 || q.level > c.rules.max_level || q.min_level < 1 || q.min_level > q.level) e.push_back(w + "levels must satisfy 1 <= min_level <= level <= max_level");
    for (const auto& cl : q.classes)
      if (!c.find_class(cl)) e.push_back(w + "unknown class '" + cl + "'");
    for (const auto& pre : q.prereqs) {
      const QuestDef* pq = c.find_quest(pre);
      if (!pq) e.push_back(w + "unknown prerequisite '" + pre + "'");
      else if (pq->repeatable) e.push_back(w + "prerequisite '" + pre + "' is repeatable, which never counts as done");
    }
    if (q.steps.empty() || q.steps.size() > 12) e.push_back(w + "needs 1..12 steps");
    if (q.offer.size() > 1000 || q.complete.size() > 1000 || q.progress.size() > 1000 || q.accepted.size() > 1000) e.push_back(w + "texts are at most 1000 characters");
    for (const auto& s : q.steps) {
      if (s.text.empty() || s.text.size() > 120) e.push_back(w + "each step needs a journal text of at most 120 characters");
      if (s.count < 1 || s.count > 100) e.push_back(w + "step counts must be 1..100");
      switch (s.type) {
        case QuestStepType::Kill:
          if (s.mobs.empty()) e.push_back(w + "a kill step needs mobs");
          for (const auto& m : s.mobs)
            if (!c.find_mob(m)) e.push_back(w + "kill step names unknown mob '" + m + "'");
          break;
        case QuestStepType::Collect: {
          const ItemDef* it = c.find_item(s.item);
          if (!it) e.push_back(w + "collect step names unknown item '" + s.item + "'");
          else if (s.count > it->stack * 8) e.push_back(w + "collect count cannot fit in a pack");
          for (const auto& m : s.drops_from)
            if (!c.find_mob(m)) e.push_back(w + "drops_from names unknown mob '" + m + "'");
          if (!s.drops_from.empty() && it && it->kind != "quest") e.push_back(w + "only quest items drop from 'drops_from' (kind quest)");
          if (s.chance <= 0.0f || s.chance > 1.0f) e.push_back(w + "drop chance must be (0, 1]");
          break;
        }
        case QuestStepType::Talk:
          quest_npc(s.npc, w, "talk step npc");
          if (!s.item.empty() && !c.find_item(s.item)) e.push_back(w + "delivery names unknown item '" + s.item + "'");
          break;
      }
    }
    for (const auto& r : q.give) {
      const ItemDef* it = c.find_item(r.item);
      if (!it) e.push_back(w + "gives unknown item '" + r.item + "'");
      else if (it->kind != "quest") e.push_back(w + "only quest items are handed out on accepting");
    }
    for (const auto& r : q.items) {
      if (!c.find_item(r.item)) e.push_back(w + "rewards unknown item '" + r.item + "'");
      if (r.count < 1 || r.count > 100) e.push_back(w + "reward counts must be 1..100");
    }
    if (q.items.size() > 4) e.push_back(w + "at most 4 reward items");
    for (const auto& [f, v] : q.faction) {
      if (!c.find_faction(f)) e.push_back(w + "rewards standing with unknown faction '" + f + "'");
      if (v < -500 || v > 500) e.push_back(w + "faction rewards must be -500..500");
    }
    if (q.xp_levels < 0.0 || q.xp_levels > 3.0 || q.coin < 0 || q.coin > 10000000) e.push_back(w + "xp must be 0..3 levels and coin 0..10000000");
  }
  // Prerequisites never form a loop.
  std::map<std::string, int> state;  // 0 unvisited, 1 visiting, 2 done
  std::function<bool(const QuestDef&)> visit = [&](const QuestDef& q) {
    int& s = state[q.id];
    if (s == 1) return false;
    if (s == 2) return true;
    s = 1;
    for (const auto& pre : q.prereqs)
      if (const QuestDef* pq = c.find_quest(pre); pq && !visit(*pq)) return false;
    state[q.id] = 2;
    return true;
  };
  for (const auto& q : c.quests)
    if (!visit(q)) {
      e.push_back("quest '" + q.id + "': its prerequisites loop back to it");
      break;
    }
  for (const auto& n : c.npcs)
    if (n.role == "questgiver" && (!n.classes.empty() || !n.sells.empty() || !n.stock.empty() || !n.travel.empty()))
      e.push_back("npc '" + n.id + "': quest givers teach, sell and carry no one");
}

}  // namespace content_detail

}  // namespace ef::game
