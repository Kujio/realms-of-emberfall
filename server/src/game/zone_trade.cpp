// Secure two-party trades. While a trade window is open both players' items are locked (no moves,
// sales, uses, or loot), so an offer can only change through the trade itself; completion re-checks
// everything (trade.cpp) and the game server saves both characters in one transaction. Cancelling,
// walking away, dying, or disconnecting closes the window with nothing exchanged.
#include <algorithm>
#include <cmath>

#include "game/zone.hpp"

namespace ef::game {

namespace {

std::uint32_t ms_of(std::uint32_t ticks, float dt) {
  return static_cast<std::uint32_t>(std::lround(static_cast<double>(ticks) * static_cast<double>(dt) * 1000.0));
}

}  // namespace

Zone::TradeSession* Zone::trade_of(const PlayerEntity& p) {
  if (p.trade == 0) return nullptr;
  auto it = trades_.find(p.trade);
  return it == trades_.end() ? nullptr : &it->second;
}

void Zone::trade_request(EntityId player, EntityId target, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  auto say = [&](const std::string& text) { send(sink, p, proto::ChatMessage{kChatSystem, "", text}); };
  if (items_locked(p, sink)) return;
  PlayerEntity* t = find(target);
  if (!t || t->id == p.id || !std::binary_search(p.known.begin(), p.known.end(), target)) return say("Target another player to trade with.");
  if (t->dead) return say(t->name + " is dead.");
  if (t->trade != 0) return say(t->name + " is busy trading.");
  if (horizontal_distance(p.pos, t->pos) > rules_.economy.trade_range) return say(t->name + " is too far away to trade.");
  for (auto& [id, s] : trades_)
    if (!s.open && (s.who[0] == p.id || s.who[1] == t->id)) {
      if (s.who[0] == p.id) return say("You are already waiting for an answer.");
      return say(t->name + " is considering another trade.");
    }
  TradeSession s;
  s.id = next_trade_++;
  if (next_trade_ == 0) next_trade_ = 1;
  s.who = {p.id, t->id};
  s.invite_expires = tick_ + ticks(rules_.economy.trade_invite_seconds);
  trades_.emplace(s.id, s);
  send(sink, *t, proto::TradeInvite{p.id, p.name, ms_of(ticks(rules_.economy.trade_invite_seconds), dt_)});
  say("You ask " + t->name + " to trade.");
}

void Zone::trade_respond(EntityId player, EntityId from, bool accept, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  TradeSession* s = nullptr;
  for (auto& [id, t] : trades_)
    if (!t.open && t.who[0] == from && t.who[1] == p.id) s = &t;
  if (!s) return send(sink, p, proto::ChatMessage{kChatSystem, "", "That trade offer is gone."});
  PlayerEntity* a = find(from);
  if (!accept || !a) {
    if (a) send(sink, *a, proto::ChatMessage{kChatSystem, "", p.name + " declines to trade."});
    trades_.erase(s->id);
    return;
  }
  std::string why;
  if (p.dead || a->dead) why = "Someone in the trade is dead.";
  else if (p.trade != 0 || a->trade != 0) why = "Someone in the trade is already trading.";
  else if (horizontal_distance(p.pos, a->pos) > rules_.economy.trade_range) why = "You are too far apart to trade.";
  if (!why.empty()) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", why});
    trades_.erase(s->id);
    return;
  }
  s->open = true;
  a->trade = s->id;
  p.trade = s->id;
  // Open windows elsewhere close: nothing else may touch the items now.
  for (PlayerEntity* x : {a, &p}) {
    if (x->looting != 0) close_loot(*x, sink, "");
    x->vendor = 0;
  }
  send_trade_state(*s, sink);
}

void Zone::send_trade_state(TradeSession& t, ZoneSink& sink) {
  for (int i = 0; i < 2; ++i) {
    PlayerEntity* me = find(t.who[static_cast<std::size_t>(i)]);
    PlayerEntity* them = find(t.who[static_cast<std::size_t>(1 - i)]);
    if (!me || !them) continue;
    proto::TradeState m;
    m.trade_id = t.id;
    m.revision = t.revision;
    m.partner_id = them->id;
    m.partner_name = them->name;
    auto entries = [&](const TradeSide& side, std::vector<proto::TradeEntry>& out) {
      for (std::size_t k = 0; k < kTradeSlots; ++k) {
        if (side.slots[k] == kNoSlot) continue;
        const auto& st = side.offered[k];
        const std::uint32_t left = st.expires_tick > tick_ ? st.expires_tick - tick_ : 0;
        out.push_back({static_cast<std::uint8_t>(k), st.item_id, static_cast<std::uint16_t>(std::clamp(st.count, 0, 65535)),
                       st.expires_tick == 0 ? 0u : std::max<std::uint32_t>(1, ms_of(left, dt_))});
      }
    };
    entries(t.side[static_cast<std::size_t>(i)], m.mine);
    entries(t.side[static_cast<std::size_t>(1 - i)], m.theirs);
    m.my_coin = t.side[static_cast<std::size_t>(i)].coin;
    m.their_coin = t.side[static_cast<std::size_t>(1 - i)].coin;
    m.my_accept = t.side[static_cast<std::size_t>(i)].accepted;
    m.their_accept = t.side[static_cast<std::size_t>(1 - i)].accepted;
    send(sink, *me, m);
  }
}

void Zone::trade_set_item(EntityId player, int index, SlotCode slot, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  TradeSession* t = trade_of(p);
  if (!t || !t->open) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You are not trading."});
  if (index < 0 || index >= static_cast<int>(kTradeSlots)) return;
  const std::size_t me = t->who[0] == p.id ? 0 : 1;
  TradeSide& side = t->side[me];
  const auto k = static_cast<std::size_t>(index);
  if (slot == kNoSlot) {
    side.slots[k] = kNoSlot;
    side.offered[k] = {};
  } else {
    std::string why;
    TradeSide probe = side;
    probe.slots[k] = kNoSlot;  // replacing this trade slot's own item is fine
    if (!can_offer(p.inv, content_, probe, slot, why)) return send(sink, p, proto::ChatMessage{kChatSystem, "", why});
    side.slots[k] = slot;
    side.offered[k] = *p.inv.at(slot);
  }
  t->side[0].accepted = t->side[1].accepted = false;  // any change needs both to look again
  ++t->revision;
  send_trade_state(*t, sink);
}

void Zone::trade_set_coin(EntityId player, std::uint64_t coin, ZoneSink& sink) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  TradeSession* t = trade_of(p);
  if (!t || !t->open) return;
  if (coin > p.coin) return send(sink, p, proto::ChatMessage{kChatSystem, "", "You do not have that much coin."});
  const std::size_t me = t->who[0] == p.id ? 0 : 1;
  t->side[me].coin = coin;
  t->side[0].accepted = t->side[1].accepted = false;
  ++t->revision;
  send_trade_state(*t, sink);
}

void Zone::trade_accept(EntityId player, bool accept, ZoneSink& sink, std::uint32_t trade_id, std::uint32_t revision) {
  PlayerEntity* pp = find(player);
  if (!pp) return;
  PlayerEntity& p = *pp;
  TradeSession* t = trade_of(p);
  if (!t || !t->open) return;
  // An accept for an offer the player did not see (it changed in flight) does not count.
  if (accept && trade_id != 0 && (trade_id != t->id || revision != t->revision)) {
    send(sink, p, proto::ChatMessage{kChatSystem, "", "The trade changed; look it over and accept again."});
    return send_trade_state(*t, sink);
  }
  const std::size_t me = t->who[0] == p.id ? 0 : 1;
  t->side[me].accepted = accept;
  if (t->side[0].accepted && t->side[1].accepted) return complete_trade(*t, sink);
  send_trade_state(*t, sink);
}

void Zone::complete_trade(TradeSession& t, ZoneSink& sink) {
  PlayerEntity* a = find(t.who[0]);
  PlayerEntity* b = find(t.who[1]);
  if (!a || !b) return close_trade(t, false, "Your trade partner is gone.", sink);
  if (horizontal_distance(a->pos, b->pos) > rules_.economy.trade_range) return close_trade(t, false, "You are too far apart to trade.", sink);
  std::string why;
  if (!execute_trade(a->inv, a->coin, t.side[0], b->inv, b->coin, t.side[1], content_, why)) {
    t.side[0].accepted = t.side[1].accepted = false;
    ++t.revision;
    for (PlayerEntity* x : {a, b}) send(sink, *x, proto::ChatMessage{kChatSystem, "", why});
    send_trade_state(t, sink);
    return;
  }
  mark_inventory(*a);
  mark_inventory(*b);
  a->save_dirty = b->save_dirty = false;  // the trade save below covers both
  close_trade(t, true, "The trade is complete.", sink);
  sink.zone_trade_completed(*a, *b);
}

void Zone::close_trade(TradeSession& t, bool completed, const std::string& message, ZoneSink& sink) {
  const std::uint32_t id = t.id;
  for (EntityId who : t.who) {
    PlayerEntity* x = find(who);
    if (!x) continue;
    if (x->trade == id) x->trade = 0;
    send(sink, *x, proto::TradeClosed{id, completed, message});
  }
  trades_.erase(id);
}

void Zone::trade_cancel(EntityId player, const std::string& message, ZoneSink& sink) {
  PlayerEntity* p = find(player);
  if (!p) return;
  if (TradeSession* t = trade_of(*p)) return close_trade(*t, false, message.empty() ? p->name + " cancelled the trade." : message, sink);
  // A pending invitation either way.
  for (auto it = trades_.begin(); it != trades_.end(); ++it)
    if (!it->second.open && (it->second.who[0] == player || it->second.who[1] == player)) {
      close_trade(it->second, false, message.empty() ? "The trade offer was withdrawn." : message, sink);
      return;
    }
}

void Zone::tick_trades(ZoneSink& sink) {
  std::vector<std::uint32_t> doomed;
  std::vector<std::string> why;
  for (auto& [id, t] : trades_) {
    PlayerEntity* a = find(t.who[0]);
    PlayerEntity* b = find(t.who[1]);
    std::string reason;
    if (!a || !b) reason = "Your trade partner is gone.";
    else if (a->dead || b->dead) reason = "The trade ends: someone died.";
    else if (horizontal_distance(a->pos, b->pos) > rules_.economy.trade_range * 1.25f) reason = "You moved too far apart to trade.";
    else if (!t.open && tick_ >= t.invite_expires) reason = "The trade offer expired.";
    if (!reason.empty()) {
      doomed.push_back(id);
      why.push_back(std::move(reason));
    }
  }
  for (std::size_t i = 0; i < doomed.size(); ++i)
    if (auto it = trades_.find(doomed[i]); it != trades_.end()) close_trade(it->second, false, why[i], sink);
}

}  // namespace ef::game
