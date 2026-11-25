#include "game/crafting.hpp"

#include <algorithm>

namespace ef::game {

float combine_chance(int skill, int trivial, const CraftRules& r) {
  const int short_by = std::max(0, trivial - skill);
  return std::clamp(r.success_max - static_cast<float>(short_by) * r.success_per_point, r.success_min, r.success_max);
}

float craft_skill_up_chance(int skill, int trivial, int cap, const CraftRules& r) {
  if (skill >= trivial || skill >= cap) return 0.0f;
  const int beyond_first = trivial - skill - 1;
  return std::clamp(r.skill_up_base + static_cast<float>(beyond_first) * r.skill_up_per_point, r.skill_up_base, r.skill_up_max);
}

int units_lost(int count, float loss_chance, Rng& rng) {
  if (count <= 0 || loss_chance <= 0.0f) return 0;
  if (loss_chance >= 1.0f) return count;
  int lost = 0;
  for (int i = 0; i < count; ++i)
    if (rng.chance(loss_chance)) ++lost;
  return lost;
}

bool in_pack(SlotCode s) { return slot_container(s) == kContGeneral || (is_bag_content(s) && !in_bank(s)); }

int count_in_pack(const Inventory& inv, std::string_view item_id) {
  int n = 0;
  inv.for_each([&](SlotCode s, const ItemStackState& st) {
    if (in_pack(s) && st.item_id == item_id) n += st.count;
  });
  return n;
}

int take_from_pack(Inventory& inv, std::string_view item_id, int count) {
  int taken = 0;
  // Bag contents first, then the pack slots: the pack keeps its loose stacks as long as it can.
  for (const bool bags : {true, false})
    inv.for_each_mut([&](SlotCode s, ItemStackState& st) {
      if (taken >= count || !in_pack(s) || is_bag_content(s) != bags || st.item_id != item_id) return;
      const int take = std::min(count - taken, st.count);
      st.count -= take;
      taken += take;
      if (st.count <= 0) st = {};
    });
  return taken;
}

CombinePlan check_combine(const Inventory& inv, const Content& c, const RecipeDef& r) {
  CombinePlan plan;
  for (const auto& in : r.ingredients) {
    const int have = count_in_pack(inv, in.item);
    if (have < in.count) return {CombineCheck::MissingIngredient, in.item, have, in.count};
  }
  for (const auto& tool : r.tools)
    if (count_in_pack(inv, tool) < 1) return {CombineCheck::MissingTool, tool, 0, 1};
  if (const ItemDef* d = c.find_item(r.result.item); d && lore_conflict(inv, *d)) return {CombineCheck::Lore, r.result.item, 1, 0};
  // Room for the result once the ingredients are gone (they may free a slot).
  Inventory after = inv;
  for (const auto& in : r.ingredients) take_from_pack(after, in.item, in.count);
  if (!can_give(after, c, ItemStackState{r.result.item, r.result.count, 0})) return {CombineCheck::NoRoom, r.result.item, 0, r.result.count};
  return plan;
}

}  // namespace ef::game
