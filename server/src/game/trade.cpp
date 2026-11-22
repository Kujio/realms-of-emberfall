#include "game/trade.hpp"

#include <algorithm>
#include <vector>

namespace ef::game {

bool TradeSide::offers(SlotCode s) const { return std::find(slots.begin(), slots.end(), s) != slots.end(); }

int TradeSide::item_count() const {
  return static_cast<int>(std::count_if(slots.begin(), slots.end(), [](SlotCode s) { return s != kNoSlot; }));
}

bool can_offer(const Inventory& inv, const Content& c, const TradeSide& side, SlotCode slot, std::string& why) {
  const std::uint8_t cont = slot_container(slot);
  const bool pack = cont == kContGeneral || (is_bag_content(slot) && !in_bank(slot));
  if (!pack || !slot_valid(inv, slot, c, static_cast<int>(kBankSlots))) return why = "You can only trade items from your pack.", false;
  const ItemStackState* s = inv.at(slot);
  if (!s || s->empty()) return why = "There is nothing there to trade.", false;
  if (side.offers(slot)) return why = "You are already offering that.", false;
  const ItemDef* d = c.find_item(s->item_id);
  if (!d) return why = "That item cannot be traded.", false;
  if (d->no_trade) return why = d->name + " cannot be traded.", false;
  if (d->is_bag() && bag_has_items(inv, slot)) return why = "Empty the bag before trading it.", false;
  return true;
}

namespace {

// Takes each offered stack out of `inv` (after checking it is still what was offered).
bool take_offer(Inventory& inv, const TradeSide& side, std::vector<ItemStackState>& out, std::string& why) {
  for (std::size_t i = 0; i < kTradeSlots; ++i) {
    if (side.slots[i] == kNoSlot) continue;
    ItemStackState* s = inv.at(side.slots[i]);
    const auto& want = side.offered[i];
    if (!s || s->empty() || s->item_id != want.item_id || s->count != want.count) return why = "The trade changed; please check it again.", false;
    out.push_back(*s);
    *s = {};
  }
  return true;
}

bool receive(Inventory& inv, std::vector<ItemStackState>& items, const Content& c, std::string& why, const char* who) {
  for (auto& st : items) {
    const ItemDef* d = c.find_item(st.item_id);
    if (d && d->lore && count_item(inv, d->id) > 0) return why = std::string(who) + " already has " + d->name + " (lore).", false;
    if (!give_item(inv, c, st)) return why = std::string(who) + " does not have room for everything.", false;
  }
  return true;
}

}  // namespace

bool execute_trade(Inventory& a, std::uint64_t& coin_a, const TradeSide& offer_a, Inventory& b, std::uint64_t& coin_b,
                   const TradeSide& offer_b, const Content& c, std::string& why) {
  if (!offer_a.accepted || !offer_b.accepted) return why = "Both sides must accept.", false;
  if (coin_a < offer_a.coin || coin_b < offer_b.coin) return why = "Someone no longer has the coin they offered.", false;
  // Work on copies: the originals change only when every step succeeded.
  Inventory ta = a;
  Inventory tb = b;
  std::vector<ItemStackState> from_a;
  std::vector<ItemStackState> from_b;
  if (!take_offer(ta, offer_a, from_a, why) || !take_offer(tb, offer_b, from_b, why)) return false;
  // Two lore copies of the same item in one direction would also clash.
  for (const auto* list : {&from_a, &from_b})
    for (std::size_t i = 0; i < list->size(); ++i)
      for (std::size_t k = i + 1; k < list->size(); ++k)
        if ((*list)[i].item_id == (*list)[k].item_id)
          if (const ItemDef* d = c.find_item((*list)[i].item_id); d && d->lore) return why = d->name + " is lore: only one may be held.", false;
  if (!receive(tb, from_a, c, why, "Your trade partner") || !receive(ta, from_b, c, why, "You")) {
    return false;
  }
  a = std::move(ta);
  b = std::move(tb);
  coin_a = coin_a - offer_a.coin + offer_b.coin;
  coin_b = coin_b - offer_b.coin + offer_a.coin;
  return true;
}

}  // namespace ef::game
