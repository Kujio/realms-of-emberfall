// Quests (Phase 6): definitions come from content/quests.json; a character's progress is a QuestBook.
// Pure and unit-tested: the zone feeds it kills, items and conversations, and acts on what it says.
//
// A quest is a sequence of steps, done in order:
//   kill     slay `count` of the listed mobs (anyone in the kill's loot party counts it)
//   collect  carry `count` of `item` (counted from the pack; handed over when the step completes, at the
//            quest's next NPC). A quest item with `drops_from` falls only while you are on the step.
//   talk     speak to `npc` (deliver: also hand over `item`)
// After the last step the quest is turned in at `turn_in` (default: the giver) for its rewards.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <functional>
#include <vector>

namespace ef::game {

// Phase 16: Craft counts successful combines of a recipe (made, not bought).
enum class QuestStepType : std::uint8_t { Kill, Collect, Talk, Craft };

struct QuestStepDef {
  QuestStepType type = QuestStepType::Kill;
  std::vector<std::string> mobs;        // kill: any of these
  std::string item;                     // collect / deliver
  std::string recipe;                   // craft: the recipe whose successful combines count
  int count = 1;
  std::string npc;                      // talk / deliver
  std::vector<std::string> drops_from;  // collect: mobs that drop `item` while on the step
  float chance = 0.5f;                  // ... with this chance per kill
  std::string text;                     // the journal line ("Slay ember rats")
  std::string say;                      // what `npc` says when the step completes (talk)
};

struct QuestRewardItem {
  std::string item;
  int count = 1;
};

struct QuestDef {
  std::string id;
  std::string name;
  std::string giver;      // npc id
  std::string turn_in;    // npc id ("" = the giver)
  int level = 1;          // recommended level (the journal shows it; XP rewards scale from it)
  int min_level = 1;
  std::vector<std::string> classes;  // empty = any class (epic chains name one)
  std::vector<std::string> prereqs;  // quests that must be done first
  bool repeatable = false;
  std::string zone;       // where it happens (journal hint)
  std::string offer;      // the giver's pitch
  std::string accepted;   // said on accepting
  std::string progress;   // said by the turn-in NPC while unfinished
  std::string complete;   // said on turning in
  std::vector<QuestRewardItem> give;  // handed over on accepting (letters, keys)
  std::vector<QuestStepDef> steps;
  // Rewards
  double xp_levels = 0.0;  // XP reward as a fraction of the XP needed to level at `level` (0.5 = half a level)
  int coin = 0;
  std::vector<QuestRewardItem> items;
  std::vector<std::pair<std::string, int>> faction;
  bool epic = false;      // a class epic chain's step
  const std::string& turn_in_npc() const { return turn_in.empty() ? giver : turn_in; }
};

struct ActiveQuest {
  std::string id;
  int step = 0;   // index of the current step (== steps.size(): ready to turn in)
  int count = 0;  // kills (or combines) so far on a kill (or craft) step
};

enum class QuestRefusal : std::uint8_t { None, Unknown, Level, Class, Prereq, Active, Done, Full };

class QuestBook {
 public:
  static constexpr std::size_t kMaxActive = 20;

  const std::vector<ActiveQuest>& active() const { return active_; }
  const std::vector<std::string>& done() const { return done_; }
  const ActiveQuest* find(std::string_view id) const;
  bool is_done(std::string_view id) const;

  // Why a character of `level` and `class_id` may not take the quest (None = they may).
  QuestRefusal can_accept(const QuestDef& q, int level, std::string_view class_id) const;
  bool accept(const QuestDef& q);
  bool abandon(std::string_view id);
  // A kill of `mob_id`: advances every kill step it counts for. Returns the quests that moved.
  std::vector<std::string> on_kill(const std::vector<const QuestDef*>& defs, std::string_view mob_id);
  // Phase 16: a successful combine of `recipe_id`: advances every craft step it counts for.
  std::vector<std::string> on_craft(const std::vector<const QuestDef*>& defs, std::string_view recipe_id);
  // Collect steps are satisfied by what the player carries; returns true if the step moved on.
  bool try_collect(const QuestDef& q, int carried);
  // Talking to `npc_id`: completes a talk step naming it (deliveries need `has_item`). True if it moved.
  bool try_talk(const QuestDef& q, std::string_view npc_id, bool has_item);
  // Every step done: ready to turn in at q.turn_in_npc().
  bool ready(const QuestDef& q) const;
  // Turns the quest in (it must be ready). Repeatable quests do not count as done.
  bool complete(const QuestDef& q);

  // {"active":[{"id":..,"step":..,"count":..}],"done":[..]}. Parsing drops anything malformed.
  std::string to_json() const;
  static QuestBook from_json(std::string_view text);

 private:
  // Bumps the count of every active quest whose current step `counts` (finishing the step at its count).
  std::vector<std::string> count_up(const std::vector<const QuestDef*>& defs, const std::function<bool(const QuestStepDef&)>& counts);
  ActiveQuest* find_mut(std::string_view id);
  std::vector<ActiveQuest> active_;
  std::vector<std::string> done_;
};

// The XP a quest grants: xp_levels of the step from its level to the next.
std::uint64_t quest_xp(double xp_levels, std::uint64_t xp_for_next_level);

}  // namespace ef::game
