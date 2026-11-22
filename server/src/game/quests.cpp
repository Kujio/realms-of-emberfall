#include "game/quests.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace ef::game {

const ActiveQuest* QuestBook::find(std::string_view id) const {
  for (const auto& a : active_)
    if (a.id == id) return &a;
  return nullptr;
}

ActiveQuest* QuestBook::find_mut(std::string_view id) {
  for (auto& a : active_)
    if (a.id == id) return &a;
  return nullptr;
}

bool QuestBook::is_done(std::string_view id) const { return std::find(done_.begin(), done_.end(), id) != done_.end(); }

QuestRefusal QuestBook::can_accept(const QuestDef& q, int level, std::string_view class_id) const {
  if (find(q.id)) return QuestRefusal::Active;
  if (!q.repeatable && is_done(q.id)) return QuestRefusal::Done;
  // Class and prerequisites first: a "come back at level N" hint is only for quests you could take then.
  if (!q.classes.empty() && std::find(q.classes.begin(), q.classes.end(), class_id) == q.classes.end()) return QuestRefusal::Class;
  for (const auto& pre : q.prereqs)
    if (!is_done(pre)) return QuestRefusal::Prereq;
  if (level < q.min_level) return QuestRefusal::Level;
  if (active_.size() >= kMaxActive) return QuestRefusal::Full;
  return QuestRefusal::None;
}

bool QuestBook::accept(const QuestDef& q) {
  if (find(q.id) || active_.size() >= kMaxActive) return false;
  active_.push_back({q.id, 0, 0});
  return true;
}

bool QuestBook::abandon(std::string_view id) {
  const auto before = active_.size();
  std::erase_if(active_, [&](const ActiveQuest& a) { return a.id == id; });
  return active_.size() != before;
}

std::vector<std::string> QuestBook::on_kill(const std::vector<const QuestDef*>& defs, std::string_view mob_id) {
  std::vector<std::string> moved;
  for (auto& a : active_) {
    const QuestDef* q = nullptr;
    for (const QuestDef* d : defs)
      if (d && d->id == a.id) q = d;
    if (!q || a.step >= static_cast<int>(q->steps.size())) continue;
    const QuestStepDef& s = q->steps[static_cast<std::size_t>(a.step)];
    if (s.type != QuestStepType::Kill || std::find(s.mobs.begin(), s.mobs.end(), mob_id) == s.mobs.end()) continue;
    ++a.count;
    if (a.count >= s.count) {
      ++a.step;
      a.count = 0;
    }
    moved.push_back(a.id);
  }
  return moved;
}

bool QuestBook::try_collect(const QuestDef& q, int carried) {
  ActiveQuest* a = find_mut(q.id);
  if (!a || a->step >= static_cast<int>(q.steps.size())) return false;
  const QuestStepDef& s = q.steps[static_cast<std::size_t>(a->step)];
  if (s.type != QuestStepType::Collect || carried < s.count) return false;
  ++a->step;
  a->count = 0;
  return true;
}

bool QuestBook::try_talk(const QuestDef& q, std::string_view npc_id, bool has_item) {
  ActiveQuest* a = find_mut(q.id);
  if (!a || a->step >= static_cast<int>(q.steps.size())) return false;
  const QuestStepDef& s = q.steps[static_cast<std::size_t>(a->step)];
  if (s.type != QuestStepType::Talk || s.npc != npc_id) return false;
  if (!s.item.empty() && !has_item) return false;
  ++a->step;
  a->count = 0;
  return true;
}

bool QuestBook::ready(const QuestDef& q) const {
  const ActiveQuest* a = find(q.id);
  return a && a->step >= static_cast<int>(q.steps.size());
}

bool QuestBook::complete(const QuestDef& q) {
  if (!ready(q)) return false;
  abandon(q.id);
  if (!q.repeatable && !is_done(q.id)) done_.push_back(q.id);
  return true;
}

std::string QuestBook::to_json() const {
  nlohmann::json j = nlohmann::json::object();
  j["active"] = nlohmann::json::array();
  for (const auto& a : active_) j["active"].push_back({{"id", a.id}, {"step", a.step}, {"count", a.count}});
  j["done"] = done_;
  return j.dump();
}

QuestBook QuestBook::from_json(std::string_view text) {
  QuestBook b;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return b;
  if (j.contains("active") && j.at("active").is_array())
    for (const auto& a : j.at("active")) {
      if (!a.is_object() || !a.contains("id") || !a.at("id").is_string()) continue;
      ActiveQuest q;
      q.id = a.at("id").get<std::string>();
      if (q.id.empty() || q.id.size() > 48 || b.find(q.id)) continue;
      q.step = a.contains("step") && a.at("step").is_number_integer() ? std::clamp(a.at("step").get<int>(), 0, 64) : 0;
      q.count = a.contains("count") && a.at("count").is_number_integer() ? std::clamp(a.at("count").get<int>(), 0, 10000) : 0;
      if (b.active_.size() < kMaxActive) b.active_.push_back(std::move(q));
    }
  if (j.contains("done") && j.at("done").is_array())
    for (const auto& d : j.at("done"))
      if (d.is_string() && !d.get<std::string>().empty() && b.done_.size() < 4096 && !b.is_done(d.get<std::string>())) b.done_.push_back(d.get<std::string>());
  return b;
}

std::uint64_t quest_xp(double xp_levels, std::uint64_t xp_for_next_level) {
  if (!(xp_levels > 0.0)) return 0;
  return static_cast<std::uint64_t>(std::llround(static_cast<double>(xp_for_next_level) * std::min(xp_levels, 5.0)));
}

}  // namespace ef::game
