// Secure two-party trade: each side offers up to kTradeSlots pack items and some coin; both must accept
// the same offer, and the swap then happens all at once or not at all. Pure (works on Inventory values);
// the zone owns the sessions and the game server saves both characters in one transaction.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "game/items.hpp"

namespace ef::game {

struct TradeSide {
  std::array<SlotCode, kTradeSlots> slots{};    // kNoSlot = empty trade slot
  std::array<ItemStackState, kTradeSlots> offered{};  // what each slot held when offered (verified at completion)
  std::uint64_t coin = 0;
  bool accepted = false;

  TradeSide() { slots.fill(kNoSlot); }
  bool offers(SlotCode s) const;
  int item_count() const;
};

// Whether the stack at `slot` may be offered: a pack or pack-bag item, not worn or banked, tradeable,
// not a bag with something in it, and not already offered.
bool can_offer(const Inventory& inv, const Content& c, const TradeSide& side, SlotCode slot, std::string& why);

// Checks every offer still matches the inventories and both sides have room (and no lore clash), then
// moves the items and coin. Either everything happens and true is returned, or nothing changes.
bool execute_trade(Inventory& a, std::uint64_t& coin_a, const TradeSide& offer_a, Inventory& b, std::uint64_t& coin_b,
                   const TradeSide& offer_b, const Content& c, std::string& why);

}  // namespace ef::game
