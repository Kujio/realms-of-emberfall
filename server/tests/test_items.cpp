// Phase 5: items, equipment, loot, vendors, bank and trade. Pure rules first, then zone behavior.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <random>
#include <set>

#include "game/auction.hpp"
#include "game/items.hpp"
#include "game/loot_rules.hpp"
#include "game/trade.hpp"
#include "zone_support.hpp"

using namespace zt;
using Catch::Approx;

namespace {

template <class F>
int run_until(Zone& z, RecordingSink& sink, std::uint32_t& t, int limit, F done) {
  for (int i = 0; i < limit; ++i) {
    if (done()) return i;
    z.tick(++t, sink);
  }
  return limit;
}

void run(Zone& z, RecordingSink& sink, std::uint32_t& t, int n) {
  for (int i = 0; i < n; ++i) z.tick(++t, sink);
}

const ItemDef& item(const char* id) {
  const ItemDef* it = content().find_item(id);
  if (!it) throw std::runtime_error(std::string("no item ") + id);
  return *it;
}

Wearer wearer(const char* race, const char* cls, int level) {
  const ClassDef* c = content().find_class(cls);
  const SkillCap* dw = c->skill("dual_wield");
  return {content().find_race(race), c, level, dw && skill_cap(*dw, level) > 0};
}

MoveContext ctx_for(const char* cls, int level, bool bank = false) {
  MoveContext m;
  m.wearer = wearer("highlander", cls, level);
  m.bank_open = bank;
  return m;
}

int total_of(const Inventory& inv, const std::string& id) { return count_item(inv, id); }

// Coin plus every stack, keyed by item id.
std::map<std::string, long long> holdings(const PlayerEntity& p) {
  std::map<std::string, long long> h;
  h["(coin)"] = static_cast<long long>(p.coin);
  p.inv.for_each([&](SlotCode, const ItemStackState& s) { h[s.item_id] += s.count; });
  return h;
}

std::map<std::string, long long> sum(std::map<std::string, long long> a, const std::map<std::string, long long>& b) {
  for (const auto& [k, v] : b) a[k] += v;
  for (auto it = a.begin(); it != a.end();) it = it->second == 0 ? a.erase(it) : std::next(it);
  return a;
}

const ZoneDef& flats() { return *content().find_zone("proving_flats"); }

EntityId npc_with_role(Zone& zone, const char* role) {
  EntityId id = 0;
  zone.for_each_npc([&](const NpcEntity& n) {
    if (id == 0 && n.def->role == role) id = n.id;
  });
  return id;
}

// Puts the player next to an NPC and lets interest catch up so it is "seen".
void walk_to(Zone& zone, RecordingSink& sink, std::uint32_t& t, EntityId p, EntityId npc) {
  const NpcEntity* n = zone.find_npc(npc);
  zone.find(p)->pos = {n->pos.x + 2.0f, 0, n->pos.z};
  for (int i = 0; i < 3; ++i) zone.tick(++t, sink);
}

}  // namespace

// ---------------------------------------------------------------- content

TEST_CASE("shipped itemization: 600+ items across every kind, armor progressions for each weight and slot", "[items][content]") {
  int non_scroll = 0;
  std::map<std::string, int> kinds;
  for (const auto& it : content().items)
    if (it.kind != "scroll") {
      ++non_scroll;
      ++kinds[it.kind];
    }
  CHECK(non_scroll >= 600);
  for (const char* k : {"weapon", "armor", "shield", "jewelry", "ammo", "consumable", "bag", "loot"}) CHECK(kinds[k] > 0);
  // Every armor weight has a piece for every armor slot at every tier.
  for (const char* w : kArmorTypes)
    for (const char* slot : {"head", "shoulders", "chest", "arms", "wrist", "hands", "waist", "legs", "feet"}) {
      std::set<int> levels;
      for (const auto& it : content().items)
        if (it.kind == "armor" && it.armor == w && it.source.empty() && std::find(it.slots.begin(), it.slots.end(), slot) != it.slots.end())
          levels.insert(it.req_level);
      CHECK(levels.size() >= 8);
    }
  int named = 0;
  for (const auto& it : content().items)
    if (it.source.rfind("dungeon:", 0) == 0) ++named;
  CHECK(named >= 40);
}

TEST_CASE("item content validation catches bad items", "[items][content]") {
  auto check = [](const char* patch, const char* expect) {
    Content c = content();
    ItemDef bad = item("longsword_t3");
    bad.id = "bad_item";
    if (std::string(patch) == "shield_primary") {
      bad = item("buckler_t1");
      bad.id = "bad_item";
      bad.slot_mask = 1u << static_cast<unsigned>(EquipSlot::Primary);
    } else if (std::string(patch) == "unknown_proc") {
      bad.proc = {"no_such_effect", 0.05f};
    } else if (std::string(patch) == "no_delay") {
      bad.delay = 0.0f;
    } else if (std::string(patch) == "bag_slots") {
      bad = item("cloth_pouch");
      bad.id = "bad_item";
      bad.bag_slots = 40;
    }
    c.items.push_back(bad);
    c.index_items();
    const auto errs = validate_content(c);
    const bool found = std::any_of(errs.begin(), errs.end(), [&](const std::string& e) { return e.find(expect) != std::string::npos; });
    CHECK(found);
  };
  check("shield_primary", "shields go in the secondary slot only");
  check("unknown_proc", "unknown proc effect");
  check("no_delay", "weapons need damage");
  check("bag_slots", "bag_slots must be 1..16");
}

// ---------------------------------------------------------------- restrictions and stats

TEST_CASE("wear restrictions: armor weight, weapon training, level, race and class lists, the off hand", "[items][rules]") {
  std::string why;
  CHECK(can_wear(item("plate_t1_chest"), EquipSlot::Chest, wearer("highlander", "warrior", 1), why));
  CHECK_FALSE(can_wear(item("plate_t1_chest"), EquipSlot::Chest, wearer("highlander", "wizard", 1), why));
  CHECK(why.find("plate") != std::string::npos);
  CHECK(can_wear(item("cloth_t1_chest"), EquipSlot::Chest, wearer("highlander", "warrior", 1), why));  // lighter is fine
  CHECK_FALSE(can_wear(item("plate_t1_chest"), EquipSlot::Legs, wearer("highlander", "warrior", 1), why));
  CHECK_FALSE(can_wear(item("greatsword_t1"), EquipSlot::Primary, wearer("highlander", "cleric", 1), why));  // no 2h slash training
  CHECK(why.find("not trained") != std::string::npos);
  CHECK_FALSE(can_wear(item("plate_t3_chest"), EquipSlot::Chest, wearer("highlander", "warrior", 10), why));
  CHECK(why.find("level 15") != std::string::npos);
  // A weapon in the off hand needs dual wield (warriors at 13).
  CHECK_FALSE(can_wear(item("longsword_t1"), EquipSlot::Secondary, wearer("highlander", "warrior", 5), why));
  CHECK(can_wear(item("longsword_t1"), EquipSlot::Secondary, wearer("highlander", "warrior", 13), why));
  CHECK(can_wear(item("buckler_t1"), EquipSlot::Secondary, wearer("highlander", "warrior", 1), why));
  ItemDef elvish = item("chain_t1_chest");
  elvish.races = {"vaelith"};
  elvish.classes = {"ranger"};
  CHECK_FALSE(can_wear(elvish, EquipSlot::Chest, wearer("highlander", "ranger", 5), why));
  CHECK(can_wear(elvish, EquipSlot::Chest, wearer("vaelith", "ranger", 5), why));
  CHECK_FALSE(can_wear(elvish, EquipSlot::Chest, wearer("vaelith", "warrior", 5), why));
}

TEST_CASE("worn gear adds AC, health, mana, stats and resists; the hastiest item counts", "[items][rules]") {
  Inventory inv;
  inv.equip[static_cast<std::size_t>(EquipSlot::Chest)] = {"plate_t5_chest", 1, 0};
  inv.equip[static_cast<std::size_t>(EquipSlot::Legs)] = {"plate_t5_legs", 1, 0};
  inv.equip[static_cast<std::size_t>(EquipSlot::Hands)] = {"plate_t8_hands", 1, 0};
  inv.equip[static_cast<std::size_t>(EquipSlot::Wrist1)] = {"plate_t7_wrist", 1, 0};
  const GearTotals g = gear_totals(inv, content());
  CHECK(g.ac == item("plate_t5_chest").ac + item("plate_t5_legs").ac + item("plate_t8_hands").ac + item("plate_t7_wrist").ac);
  CHECK(g.hp == item("plate_t5_chest").hp + item("plate_t5_legs").hp + item("plate_t8_hands").hp + item("plate_t7_wrist").hp);
  CHECK(g.haste == std::max(item("plate_t8_hands").haste, item("plate_t7_wrist").haste));
  CHECK(g.haste > 0);
  const DerivedStats base = derive_stats(*content().find_race("highlander"), *content().find_class("warrior"), 40);
  const DerivedStats d = apply_gear(base, g);
  CHECK(d.ac == base.ac + g.ac);
  CHECK(d.max_hp == base.max_hp + g.hp);
  CHECK(d.stats.sta == std::min(255, base.stats.sta + g.stats.sta));
  // Haste: item and spell haste do not stack (the larger wins), slows still apply.
  CHECK(swing_delay(3.0f, 20, 0.0f) == Approx(2.5f));
  CHECK(swing_delay(3.0f, 20, 40.0f) == Approx(3.0f / 1.4f));
  CHECK(swing_delay(3.0f, 20, -30.0f) == Approx(3.0f / 0.9f));
  // Procs: slower weapons proc more per swing.
  CHECK(proc_chance(0.05f, 3.0f) == Approx(0.05f));
  CHECK(proc_chance(0.05f, 4.5f) == Approx(0.075f));
}

TEST_CASE("an equipped weapon replaces bare fists, scaled by the class", "[items][rules][zone]") {
  Zone zone(test_zone(), content(), kDt, 11);
  ZonePlayerInit w = at(0, 0, "Fist", 20);
  const auto p = zone.add_player(w);
  const float fists = zone.find(p)->stats.weapon_damage;
  ZonePlayerInit armed = at(5, 0, "Armed", 20);
  armed.items.push_back({equip_code(EquipSlot::Primary), {"longsword_t5", 1, 0}});
  const auto q = zone.add_player(armed);
  CHECK(zone.find(q)->stats.weapon_damage == Approx(static_cast<float>(item("longsword_t5").damage) * content().find_class("warrior")->combat.weapon_mult));
  CHECK(zone.find(q)->stats.weapon_delay == Approx(item("longsword_t5").delay));
  CHECK(zone.find(q)->stats.weapon_damage > fists * 1.5f);
  // A monk's fists are a real weapon.
  const auto m = zone.add_player(as_class(at(-5, 0, "Monk", 20), "monk"));
  CHECK(zone.find(m)->stats.weapon_damage > fists * 2.0f);
  CHECK(zone.find(m)->stats.offhand_damage > 0.0f);
}

TEST_CASE("vendor prices never allow a buy-low, sell-high loop", "[items][rules]") {
  const auto& E = content().rules.economy;
  for (int value : {1, 2, 3, 7, 10, 99, 1000, 123457})
    for (int cha = 1; cha <= 255; cha += 7) {
      const auto buy = vendor_buy_price(value, cha, E);
      const auto sell = vendor_sell_price(value, cha, E);
      CHECK(buy >= 1);
      CHECK(sell < buy);
    }
  CHECK(vendor_sell_price(0, 75, E) == 0);
  CHECK(vendor_buy_price(1000, 200, E) < vendor_buy_price(1000, 75, E));  // charisma helps
  CHECK(auction_fee(1000, E) == 50);
  CHECK(auction_fee(1, E) == static_cast<std::uint64_t>(E.auction_min_fee));
}

// ---------------------------------------------------------------- moving items

TEST_CASE("moving items: equip, swap, stacks, two-handers, bags and the bank", "[items][inventory]") {
  Inventory inv;
  std::string why;
  const auto& C = content();
  auto warrior = ctx_for("warrior", 20);
  inv.general[0] = {"longsword_t3", 1, 0};
  inv.general[1] = {"greatsword_t3", 1, 0};
  inv.general[2] = {"buckler_t2", 1, 0};
  // Equip a sword and a shield.
  REQUIRE(move_item(inv, C, warrior, general_code(0), equip_code(EquipSlot::Primary), 0, why) == MoveOutcome::Moved);
  REQUIRE(move_item(inv, C, warrior, general_code(2), equip_code(EquipSlot::Secondary), 0, why) == MoveOutcome::Moved);
  // A two-hander cannot go in while the shield is worn.
  CHECK(move_item(inv, C, warrior, general_code(1), equip_code(EquipSlot::Primary), 0, why) == MoveOutcome::Refused);
  CHECK(why.find("off hand") != std::string::npos);
  CHECK(inv.equip[static_cast<std::size_t>(EquipSlot::Primary)].item_id == "longsword_t3");  // unchanged
  // Shield off, then the two-hander swaps with the sword.
  REQUIRE(move_item(inv, C, warrior, equip_code(EquipSlot::Secondary), general_code(5), 0, why) == MoveOutcome::Moved);
  REQUIRE(move_item(inv, C, warrior, general_code(1), equip_code(EquipSlot::Primary), 0, why) == MoveOutcome::Moved);
  CHECK(inv.general[1].item_id == "longsword_t3");
  // Now the shield cannot go back on.
  CHECK(move_item(inv, C, warrior, general_code(5), equip_code(EquipSlot::Secondary), 0, why) == MoveOutcome::Refused);
  // Armor in the wrong slot, or a scroll as a helmet.
  inv.general[3] = {"plate_t1_chest", 1, 0};
  CHECK(move_item(inv, C, warrior, general_code(3), equip_code(EquipSlot::Head), 0, why) == MoveOutcome::Refused);

  // Stacks split and merge.
  inv.general[4] = {"trail_rations", 15, 0};
  REQUIRE(move_item(inv, C, warrior, general_code(4), general_code(6), 5, why) == MoveOutcome::Moved);
  CHECK(inv.general[4].count == 10);
  CHECK(inv.general[6].count == 5);
  REQUIRE(move_item(inv, C, warrior, general_code(6), general_code(4), 0, why) == MoveOutcome::Moved);
  CHECK(inv.general[4].count == 15);
  CHECK(inv.general[6].empty());
  inv.general[6] = {"trail_rations", 10, 0};
  REQUIRE(move_item(inv, C, warrior, general_code(6), general_code(4), 0, why) == MoveOutcome::Moved);  // tops up to 20
  CHECK(inv.general[4].count == 20);
  CHECK(inv.general[6].count == 5);
  CHECK(total_of(inv, "trail_rations") == 25);

  // Bags: contents go with the bag; bags never nest; a bag cannot go inside itself.
  inv.general[7] = {"small_sack", 1, 0};
  REQUIRE(move_item(inv, C, warrior, general_code(6), bag_code(false, 7, 0), 0, why) == MoveOutcome::Moved);
  CHECK(inv.general_bags[7][0].item_id == "trail_rations");
  CHECK_FALSE(slot_valid(inv, bag_code(false, 7, 6), C, 16));  // a six-slot sack
  inv.general[0] = {"cloth_pouch", 1, 0};
  CHECK(move_item(inv, C, warrior, general_code(0), bag_code(false, 7, 1), 0, why) == MoveOutcome::Refused);
  CHECK(move_item(inv, C, warrior, general_code(7), bag_code(false, 7, 1), 0, why) == MoveOutcome::Refused);
  // The bank needs a banker.
  CHECK(move_item(inv, C, warrior, general_code(7), bank_code(0), 0, why) == MoveOutcome::Refused);
  auto at_bank = ctx_for("warrior", 20, true);
  REQUIRE(move_item(inv, C, at_bank, general_code(7), bank_code(0), 0, why) == MoveOutcome::Moved);
  CHECK(inv.bank[0].item_id == "small_sack");
  CHECK(inv.bank_bags[0][0].item_id == "trail_rations");
  CHECK(inv.general_bags[7][0].empty());
  CHECK(total_of(inv, "trail_rations") == 25);
}

TEST_CASE("give_item tops up stacks, then fills the pack, then bags; never a bag in a bag", "[items][inventory]") {
  Inventory inv;
  inv.general[0] = {"leather_satchel", 1, 0};
  for (std::size_t g = 1; g < kGeneralSlots; ++g) inv.general[g] = {"rat_whisker", 20, 0};
  ItemStackState food{"trail_rations", 30, 0};
  CHECK(give_item(inv, content(), food));
  CHECK(inv.general_bags[0][0].count == 20);
  CHECK(inv.general_bags[0][1].count == 10);
  ItemStackState bag{"cloth_pouch", 1, 0};
  CHECK_FALSE(can_give(inv, content(), bag));  // the pack is full and bags do not go in bags
  CHECK_FALSE(give_item(inv, content(), bag));
  CHECK(bag.count == 1);
}

// ---------------------------------------------------------------- loot rules

TEST_CASE("loot tables: weighted pools follow their weights, independent drops their chances, and max_drops caps", "[items][loot]") {
  LootTable t;
  t.max_drops = 3;
  t.drops = {{"rat_whisker", 0.3f, 1, 1}};
  LootPool pool;
  pool.chance = 1.0f;
  pool.entries = {{"a", 6.0f, 1, 1}, {"b", 3.0f, 1, 1}, {"c", 1.0f, 1, 1}};
  t.pools = {pool};
  Rng rng(42);
  std::map<std::string, int> seen;
  const int n = 20000;
  for (int i = 0; i < n; ++i)
    for (const auto& s : roll_loot(t, rng)) ++seen[s.item_id];
  CHECK(seen["rat_whisker"] == Approx(0.3 * n).epsilon(0.05));
  CHECK(seen["a"] == Approx(0.6 * n).epsilon(0.05));
  CHECK(seen["b"] == Approx(0.3 * n).epsilon(0.05));
  CHECK(seen["c"] == Approx(0.1 * n).epsilon(0.08));
  // "Nothing" entries and the cap.
  LootTable cap;
  cap.max_drops = 2;
  LootPool many;
  many.rolls = 5;
  many.entries = {{"x", 1.0f, 1, 1}, {"", 1.0f, 1, 1}};
  cap.pools = {many};
  int total = 0;
  int maxed = 0;
  for (int i = 0; i < 5000; ++i) {
    const auto got = roll_loot(cap, rng);
    REQUIRE(got.size() <= 2);
    total += static_cast<int>(got.size());
    if (got.size() == 2) ++maxed;
  }
  CHECK(maxed > 3000);  // five 50% rolls nearly always reach the cap of two
  CHECK(total > 0);
}

TEST_CASE("every shipped loot table drops level-appropriate gear", "[items][loot][content]") {
  Rng rng(7);
  for (const auto& m : content().mobs) {
    const LootTable* t = content().find_loot(m.loot);
    if (!t) continue;
    int gear = 0;
    for (int i = 0; i < 400; ++i)
      for (const auto& s : roll_loot(*t, rng)) {
        const ItemDef& d = item(s.item_id.c_str());
        if (d.equippable()) {
          ++gear;
          CHECK(d.req_level <= m.level);
          CHECK(d.req_level >= std::max(0, m.level - 8));
        }
        CHECK_FALSE(d.summoned);
      }
    CHECK(gear > 40);  // roughly a fifth of kills or better
  }
}

TEST_CASE("need beats greed, the highest roll wins, and all-pass leaves the item free", "[items][loot]") {
  Rng rng(3);
  for (int i = 0; i < 200; ++i) {
    std::vector<RollEntry> e{{1, RollChoice::Greed, 0}, {2, RollChoice::Need, 0}, {3, RollChoice::Greed, 0}, {4, RollChoice::Pass, 0}};
    const int w = resolve_rolls(e, rng);
    CHECK(w == 1);
    CHECK(e[3].roll == 0);
  }
  for (int i = 0; i < 200; ++i) {
    std::vector<RollEntry> e{{1, RollChoice::Greed, 0}, {2, RollChoice::Greed, 0}};
    const int w = resolve_rolls(e, rng);
    REQUIRE(w >= 0);
    CHECK(e[static_cast<std::size_t>(w)].roll >= e[static_cast<std::size_t>(1 - w)].roll);
  }
  std::vector<RollEntry> pass{{1, RollChoice::Pass, 0}, {2, RollChoice::Pass, 0}};
  CHECK(resolve_rolls(pass, rng) == -1);
  CHECK(round_robin(5, {10, 20, 30}, 1) == std::vector<std::int64_t>{20, 30, 10, 20, 30});
  CHECK(split_coin(10, 3) == std::vector<std::uint64_t>{4, 3, 3});
}

// ---------------------------------------------------------------- trade (pure)

TEST_CASE("a trade either completes entirely or changes nothing", "[items][trade]") {
  const auto& C = content();
  Inventory a;
  Inventory b;
  std::uint64_t ca = 100;
  std::uint64_t cb = 50;
  a.general[0] = {"longsword_t2", 1, 0};
  a.general[1] = {"trail_rations", 5, 0};
  b.general[0] = {"plate_t1_chest", 1, 0};
  TradeSide sa;
  TradeSide sb;
  std::string why;
  REQUIRE(can_offer(a, C, sa, general_code(0), why));
  sa.slots[0] = general_code(0);
  sa.offered[0] = a.general[0];
  sa.coin = 30;
  sb.slots[0] = general_code(0);
  sb.offered[0] = b.general[0];
  CHECK_FALSE(execute_trade(a, ca, sa, b, cb, sb, C, why));  // not accepted yet
  sa.accepted = sb.accepted = true;
  // B's pack is full of junk except the offered slot: A's sword fits in the freed slot.
  for (std::size_t g = 1; g < kGeneralSlots; ++g) b.general[g] = {"rat_whisker", 20, 0};
  REQUIRE(execute_trade(a, ca, sa, b, cb, sb, C, why));
  CHECK(ca == 70);
  CHECK(cb == 80);
  CHECK(total_of(a, "plate_t1_chest") == 1);
  CHECK(total_of(b, "longsword_t2") == 1);
  CHECK(total_of(a, "longsword_t2") == 0);
  // Two stacks into a pack with no room for both: refused, and nothing moves.
  const Inventory a_before = a;
  const Inventory b_before = b;
  TradeSide two;
  two.slots[0] = general_code(0);
  two.offered[0] = a.general[0];
  two.slots[1] = general_code(1);
  two.offered[1] = a.general[1];
  two.accepted = true;
  for (std::size_t g = 0; g < kGeneralSlots; ++g)
    if (b.general[g].empty()) b.general[g] = {"bone_chips", 20, 0};
  const Inventory b_full = b;
  TradeSide none;
  none.accepted = true;
  ca = 70;
  cb = 80;
  CHECK_FALSE(execute_trade(a, ca, two, b, cb, none, C, why));
  CHECK(why.find("room") != std::string::npos);
  CHECK(total_of(a, "plate_t1_chest") == total_of(a_before, "plate_t1_chest"));
  CHECK(total_of(a, "trail_rations") == 5);
  CHECK(total_of(b, "bone_chips") == total_of(b_full, "bone_chips"));
  CHECK(ca == 70);
  CHECK(cb == 80);
  (void)b_before;
  // An offer that no longer matches the pack is refused.
  TradeSide stale = two;
  stale.offered[1].count = 99;
  CHECK_FALSE(execute_trade(a, ca, stale, b, cb, none, C, why));
  // No-trade items and bags with things in them cannot be offered.
  Inventory c;
  c.general[0] = {"rattlebone_ring", 1, 0};
  TradeSide sc;
  if (item("obsidian_ring").no_trade) {
    c.general[0] = {"obsidian_ring", 1, 0};
    CHECK_FALSE(can_offer(c, C, sc, general_code(0), why));
  }
  c.general[1] = {"small_sack", 1, 0};
  c.general_bags[1][0] = {"rat_whisker", 1, 0};
  CHECK_FALSE(can_offer(c, C, sc, general_code(1), why));
  c.equip[static_cast<std::size_t>(EquipSlot::Chest)] = {"plate_t1_chest", 1, 0};
  CHECK_FALSE(can_offer(c, C, sc, equip_code(EquipSlot::Chest), why));
}

TEST_CASE("lore items: one per character, enforced on trades", "[items][trade]") {
  const auto& C = content();
  const ItemDef& lore = item("rattlebone_blade");
  REQUIRE(lore.lore);
  Inventory a;
  Inventory b;
  a.general[0] = {lore.id, 1, 0};
  b.general[0] = {lore.id, 1, 0};
  std::uint64_t ca = 0, cb = 0;
  TradeSide sa;
  sa.slots[0] = general_code(0);
  sa.offered[0] = a.general[0];
  sa.accepted = true;
  TradeSide sb;
  sb.accepted = true;
  std::string why;
  CHECK_FALSE(execute_trade(a, ca, sa, b, cb, sb, C, why));
  CHECK(why.find("lore") != std::string::npos);
  CHECK(total_of(a, lore.id) == 1);
}

// ---------------------------------------------------------------- zone: trade, fuzzed

TEST_CASE("trading never creates or destroys items or coin, under random offers, cancels, moves and disconnects", "[items][trade][fuzz][zone]") {
  const std::vector<std::string> pool{"longsword_t2", "trail_rations", "plate_t1_chest", "small_sack", "rat_whisker", "minor_draught", "cloth_pouch"};
  std::mt19937 rng(2024);
  for (int round = 0; round < 60; ++round) {
    Zone zone(test_zone(), content(), kDt, 500 + static_cast<std::uint64_t>(round));
    RecordingSink sink;
    std::uint32_t t = 0;
    auto make = [&](const char* name, float x) {
      ZonePlayerInit i = at(x, 0, name, 10);
      i.coin = 1000;
      for (std::size_t g = 0; g < kGeneralSlots; ++g)
        if (rng() % 3 != 0) {
          const std::string& id = pool[rng() % pool.size()];
          const ItemDef& d = item(id.c_str());
          i.items.push_back({general_code(g), {id, 1 + static_cast<int>(rng() % static_cast<unsigned>(d.stack)), 0}});
        }
      return zone.add_player(i);
    };
    EntityId a = make("Alpha", 0);
    EntityId b = make("Beta", 3);
    zone.tick(++t, sink);
    const auto start = sum(holdings(*zone.find(a)), holdings(*zone.find(b)));
    std::map<std::string, long long> gone;  // what a disconnected player took with them
    int completed = 0;
    for (int step = 0; step < 80; ++step) {
      PlayerEntity* pa = zone.find(a);
      PlayerEntity* pb = zone.find(b);
      if (!pa || !pb) break;
      const EntityId who = rng() % 2 ? a : b;
      const EntityId other = who == a ? b : a;
      switch (rng() % 12) {
        case 0: zone.trade_request(who, other, sink); break;
        case 1: zone.trade_respond(who, other, rng() % 4 != 0, sink); break;
        case 2:
        case 3: zone.trade_set_item(who, static_cast<int>(rng() % kTradeSlots), general_code(rng() % kGeneralSlots), sink); break;
        case 4: zone.trade_set_coin(who, rng() % 1500, sink); break;
        case 5:
        case 6:
        case 7: {
          const bool before = zone.in_trade(a);
          zone.trade_accept(who, rng() % 5 != 0, sink);
          if (before && !zone.in_trade(a) && sink.last<proto::TradeClosed>(a) && sink.last<proto::TradeClosed>(a)->completed) ++completed;
          break;
        }
        case 8: zone.trade_cancel(who, "", sink); break;
        case 9: zone.move_item(who, general_code(rng() % kGeneralSlots), general_code(rng() % kGeneralSlots), 0, sink); break;
        case 10: zone.trade_set_item(who, static_cast<int>(rng() % kTradeSlots), kNoSlot, sink); break;
        case 11:
          if (rng() % 6 == 0) {  // disconnect mid-whatever
            gone = sum(gone, holdings(*zone.find(who)));
            zone.remove_player(who, sink);
          }
          break;
      }
      zone.tick(++t, sink);
      std::map<std::string, long long> now = gone;
      if (const PlayerEntity* x = zone.find(a)) now = sum(now, holdings(*x));
      if (const PlayerEntity* x = zone.find(b)) now = sum(now, holdings(*x));
      REQUIRE(now == start);
    }
    (void)completed;
  }
}

TEST_CASE("a trade over the zone: both accept, items and coin swap, and both are saved together", "[items][trade][zone]") {
  struct TradeSink final : ZoneSink {
    RecordingSink inner;
    int trades = 0;
    void zone_send(const PlayerEntity& to, std::uint8_t ch, std::span<const std::uint8_t> p) override { inner.zone_send(to, ch, p); }
    void zone_correction(const PlayerEntity&, const char*) override {}
    void zone_violation(const PlayerEntity&, const char*) override {}
    void zone_trade_completed(const PlayerEntity&, const PlayerEntity&) override { ++trades; }
  } sink;
  Zone zone(test_zone(), content(), kDt, 77);
  std::uint32_t t = 0;
  ZonePlayerInit ia = at(0, 0, "Seller", 10);
  ia.coin = 10;
  ia.items.push_back({general_code(0), {"longsword_t2", 1, 0}});
  ZonePlayerInit ib = at(4, 0, "Buyer", 10);
  ib.coin = 500;
  const auto a = zone.add_player(ia);
  const auto b = zone.add_player(ib);
  zone.tick(++t, sink);
  zone.trade_request(a, b, sink);
  REQUIRE(sink.inner.last<proto::TradeInvite>(b));
  zone.trade_respond(b, a, true, sink);
  CHECK(zone.in_trade(a));
  CHECK(zone.in_trade(b));
  // Items are locked while trading.
  zone.move_item(a, general_code(0), general_code(3), 0, sink);
  CHECK(zone.find(a)->inv.general[0].item_id == "longsword_t2");
  zone.trade_set_item(a, 0, general_code(0), sink);
  zone.trade_set_coin(b, 300, sink);
  zone.trade_accept(a, true, sink);
  // Changing the offer clears both accepts.
  zone.trade_set_coin(b, 250, sink);
  auto st = sink.inner.last<proto::TradeState>(a);
  REQUIRE(st);
  CHECK_FALSE(st->my_accept);
  CHECK(st->their_coin == 250);
  zone.trade_accept(a, true, sink);
  zone.trade_accept(b, true, sink);
  CHECK(sink.trades == 1);
  CHECK_FALSE(zone.in_trade(a));
  CHECK(zone.find(a)->coin == 260);
  CHECK(zone.find(b)->coin == 250);
  CHECK(count_of(*zone.find(b), "longsword_t2") == 1);
  CHECK(count_of(*zone.find(a), "longsword_t2") == 0);
  // Walking away cancels an open trade.
  zone.trade_request(a, b, sink);
  zone.trade_respond(b, a, true, sink);
  REQUIRE(zone.in_trade(a));
  zone.find(b)->pos = {200, 0, 0};
  for (int i = 0; i < 12; ++i) zone.tick(++t, sink);
  CHECK_FALSE(zone.in_trade(a));
  CHECK(sink.inner.chat_count(a, "too far") + static_cast<int>(sink.inner.all<proto::TradeClosed>(a).size()) >= 1);
}

// ---------------------------------------------------------------- zone: equipment, use, procs, archery

TEST_CASE("equipping through the zone updates stats, the character sheet and what others see held", "[items][zone]") {
  Zone zone(test_zone(), content(), kDt, 21);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = at(0, 0, "Gear", 20);
  i.items.push_back({general_code(0), {"plate_t3_chest", 1, 0}});
  i.items.push_back({general_code(1), {"longsword_t5", 1, 0}});
  i.items.push_back({general_code(2), {"wizard_robe_nonexistent", 1, 0}});  // unknown items are dropped on load
  const auto p = zone.add_player(i);
  const auto watcher = zone.add_player(at(3, 0, "Watcher", 20));
  zone.tick(++t, sink);
  const int ac0 = zone.find(p)->stats.ac;
  zone.use_item(p, general_code(0), 0, sink);  // "use" on armor equips it
  CHECK(zone.find(p)->inv.equip[static_cast<std::size_t>(EquipSlot::Chest)].item_id == "plate_t3_chest");
  CHECK(zone.find(p)->stats.ac == ac0 + item("plate_t3_chest").ac);
  zone.move_item(p, general_code(1), equip_code(EquipSlot::Primary), 0, sink);
  zone.tick(++t, sink);
  auto sheet = sink.last<proto::CharacterSheet>(p);
  REQUIRE(sheet);
  CHECK(sheet->ac == zone.find(p)->stats.ac);
  CHECK(sheet->weapon_damage == static_cast<int>(std::lround(zone.find(p)->stats.weapon_damage)));
  auto held = sink.last<proto::EntityEquipment>(watcher);
  REQUIRE(held);
  CHECK(held->entity_id == p);
  CHECK(held->main_hand == "longsword_t5");
  CHECK(sink.last<proto::Inventory>(p));
  CHECK(zone.find(p)->save_dirty);
  CHECK(count_of(*zone.find(p), "wizard_robe_nonexistent") == 0);
}

TEST_CASE("consumables: a potion heals and is used up, food is a lasting buff, bandages take a cast", "[items][zone]") {
  Zone zone(test_zone(), content(), kDt, 23);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = at(0, 0, "Thirsty", 30);
  i.items.push_back({general_code(0), {"greater_draught", 3, 0}});
  i.items.push_back({general_code(1), {"hearty_stew", 2, 0}});
  i.items.push_back({general_code(2), {"silk_bandage", 2, 0}});
  const auto p = zone.add_player(i);
  zone.tick(++t, sink);
  PlayerEntity& u = *zone.find(p);
  u.hp = 10;
  zone.use_item(p, general_code(0), 0, sink);
  CHECK(zone.find(p)->hp > 10);
  CHECK(zone.find(p)->inv.general[0].count == 2);
  zone.use_item(p, general_code(0), 0, sink);  // recast
  CHECK(zone.find(p)->inv.general[0].count == 2);
  zone.use_item(p, general_code(1), 0, sink);
  CHECK(std::any_of(zone.find(p)->buffs.begin(), zone.find(p)->buffs.end(), [](const ActiveBuff& b) { return b.stack_group == "well_fed"; }));
  CHECK(zone.find(p)->inv.general[1].count == 1);
  run(zone, sink, t, 40);
  zone.find(p)->hp = 10;
  zone.use_item(p, general_code(2), 0, sink);
  CHECK(zone.find(p)->cast.casting());
  CHECK(zone.find(p)->inv.general[2].count == 2);  // used up when the cast lands
  run_until(zone, sink, t, 200, [&] { return !zone.find(p)->cast.casting(); });
  CHECK(zone.find(p)->inv.general[2].count == 1);
  CHECK(zone.find(p)->hp > 10);
}

TEST_CASE("weapon procs fire on landed hits through the spell engine", "[items][zone]") {
  Content c = content();
  for (auto& it : c.items)
    if (it.id == "longsword_t10") it.proc = {"proc_fire_45", 0.5f};
  c.index_items();
  REQUIRE(c.find_item_effect("proc_fire_45"));
  Zone zone(test_zone(), c, kDt, 29);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = at(0, 0, "Procer", 45);
  i.items.push_back({equip_code(EquipSlot::Primary), {"longsword_t10", 1, 0}});
  const auto p = zone.add_player(i);
  MobDef dummy_mob = mob("road_brigand");
  dummy_mob.level = 45;
  dummy_mob.hp_mult = 50.0f;
  dummy_mob.damage_mult = 0.01f;
  dummy_mob.aggro_radius = 0;
  dummy_mob.loot.clear();
  const auto m = zone.spawn_mob(dummy_mob, {2, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  zone.set_auto_attack(p, true, sink);
  run(zone, sink, t, 20 * 60);
  int procs = 0;
  int hits = 0;
  for (const auto& e : sink.all<proto::CombatEvent>(p)) {
    if (e.source_id == p && e.ability.empty() && (e.kind == static_cast<std::uint8_t>(CombatKind::Hit) || e.kind == static_cast<std::uint8_t>(CombatKind::Crit))) ++hits;
    if (e.ability == "Searing Burst") ++procs;
  }
  UNSCOPED_INFO("hits " << hits << " procs " << procs);
  CHECK(hits >= 6);
  CHECK(procs >= hits / 5);
  CHECK(procs <= hits);
}

TEST_CASE("archery shots need a bow, and the bow adds to them", "[items][zone]") {
  Zone zone(test_zone(), content(), kDt, 31);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(as_class(at(0, 0, "Archer", 10), "ranger"));
  MobDef target = mob("road_brigand");
  target.level = 10;
  target.aggro_radius = 0;
  target.hp_mult = 50.0f;
  const auto m = zone.spawn_mob(target, {20, 0, 0});
  zone.tick(++t, sink);
  zone.use_ability(p, "quick_shot", m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message.find("need a bow") != std::string::npos);
  *zone.find(p)->inv.at(equip_code(EquipSlot::Ranged)) = {"shortbow_t3", 1, 0};
  zone.use_ability(p, "quick_shot", m, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->ok);
}

// ---------------------------------------------------------------- zone: death and corpses

TEST_CASE("death puts worn gear and the pack on the corpse; the owner loots it back into place", "[items][zone][death]") {
  Zone zone(test_zone(), content(), kDt, 37);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = at(0, 0, "Fallen", 5);  // below the XP-loss level: no level lost to compare against
  i.items.push_back({equip_code(EquipSlot::Chest), {"plate_t1_chest", 1, 0}});
  i.items.push_back({equip_code(EquipSlot::Primary), {"longsword_t2", 1, 0}});
  i.items.push_back({general_code(0), {"small_sack", 1, 0}});
  i.items.push_back({bag_code(false, 0, 2), {"trail_rations", 7, 0}});
  i.items.push_back({bank_code(0), {"rat_whisker", 3, 0}});
  const auto p = zone.add_player(i);
  const int ac_dressed = zone.find(p)->stats.ac;
  MobDef killer = mob("road_brigand");
  killer.level = 40;
  killer.damage_mult = 30.0f;
  zone.spawn_mob(killer, {0, 0, -2});
  run_until(zone, sink, t, 20 * 60, [&] { return zone.find(p)->dead; });
  REQUIRE(zone.find(p)->dead);
  CHECK(zone.find(p)->inv.equip[static_cast<std::size_t>(EquipSlot::Chest)].empty());
  CHECK(zone.find(p)->stats.ac < ac_dressed);
  CHECK(count_of(*zone.find(p), "rat_whisker") == 3);  // the bank stays
  EntityId corpse = 0;
  zone.for_each_corpse([&](const CorpseEntity& c) {
    if (c.player_corpse) corpse = c.id;
  });
  REQUIRE(corpse != 0);
  zone.respawn(p, sink);
  zone.find(p)->pos = zone.find_corpse(corpse)->pos;
  zone.for_each_mob([&](const MobEntity&) {});
  zone.tick(++t, sink);
  zone.loot_open(p, corpse, sink);
  zone.loot_take(p, corpse, kLootAll, sink);
  CHECK(zone.find(p)->inv.equip[static_cast<std::size_t>(EquipSlot::Chest)].item_id == "plate_t1_chest");
  CHECK(zone.find(p)->inv.equip[static_cast<std::size_t>(EquipSlot::Primary)].item_id == "longsword_t2");
  CHECK(zone.find(p)->inv.general[0].item_id == "small_sack");
  CHECK(zone.find(p)->inv.general_bags[0][2].item_id == "trail_rations");
  CHECK(zone.find(p)->stats.ac == ac_dressed);
}

// ---------------------------------------------------------------- zone: group loot

namespace {
// Two players in one group (Phase 7: loot is shared by groups, not by everyone who hit the mob).
void group_up(Zone& zone, GroupRegistry& reg, EntityId lead, EntityId other) {
  const PlayerEntity* a = zone.find(lead);
  const PlayerEntity* b = zone.find(other);
  REQUIRE(reg.invite(a->char_id, a->name, b->char_id, 1e9).ok());
  REQUIRE(reg.accept(b->char_id, b->name).ok());
  zone.set_groups(&reg);
}
}  // namespace

TEST_CASE("group loot: two contributors split the coin and roll need before greed on gear", "[items][zone][loot]") {
  Content c = content();
  for (auto& t : c.loot_tables)
    if (t.id == "road_brigand_loot") {
      t.coin_min = t.coin_max = 101;
      t.drops.clear();
      t.pools = {LootPool{1, 1.0f, {{"plate_t1_chest", 1.0f, 1, 1}}}};
    }
  Zone zone(test_zone(), c, kDt, 41);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto war = zone.add_player(at(0, 0, "Tank", 10));
  const auto wiz = zone.add_player(as_class(at(1, 0, "Caster", 10), "wizard"));
  GroupRegistry groups;
  group_up(zone, groups, war, wiz);
  MobDef foe = mob("road_brigand");
  foe.level = 6;
  foe.aggro_radius = 0;
  foe.damage_mult = 0.1f;
  const auto m = zone.spawn_mob(foe, {2, 0, 0});
  zone.tick(++t, sink);
  for (EntityId who : {war, wiz}) {
    zone.set_target(who, m, sink);
    zone.set_auto_attack(who, true, sink);
  }
  zone.find(wiz)->pos = {2, 0, 1};
  run_until(zone, sink, t, 20 * 120, [&] { const MobEntity* x = zone.find_mob(m); return !x || !x->alive(); });
  const auto coin_w = zone.find(war)->coin;
  const auto coin_z = zone.find(wiz)->coin;
  CHECK(coin_w + coin_z == 101);
  CHECK(std::max(coin_w, coin_z) - std::min(coin_w, coin_z) <= 1);
  auto roll_w = sink.last<proto::LootRoll>(war);
  auto roll_z = sink.last<proto::LootRoll>(wiz);
  REQUIRE(roll_w);
  REQUIRE(roll_z);
  CHECK(roll_w->can_need);         // plate: the warrior can wear it
  CHECK_FALSE(roll_z->can_need);   // the wizard cannot
  zone.loot_roll_choice(wiz, roll_z->roll_id, RollChoice::Need, sink);  // downgraded to greed
  zone.loot_roll_choice(war, roll_w->roll_id, RollChoice::Need, sink);
  CHECK(count_of(*zone.find(war), "plate_t1_chest") == 1);
  CHECK(count_of(*zone.find(wiz), "plate_t1_chest") == 0);
  CHECK(sink.chat_count(wiz, "Tank wins") == 1);
}

TEST_CASE("group loot: round robin reserves each item for one member until it frees up", "[items][zone][loot]") {
  Content c = content();
  for (auto& t : c.loot_tables)
    if (t.id == "road_brigand_loot") {
      t.drops = {{"rat_whisker", 1.0f, 1, 1}, {"bone_chips", 1.0f, 1, 1}};
      t.pools.clear();
    }
  Zone zone(test_zone(), c, kDt, 43);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto a = zone.add_player(at(0, 0, "Lead", 10));
  const auto b = zone.add_player(at(1, 0, "Second", 10));
  GroupRegistry groups;
  group_up(zone, groups, a, b);
  REQUIRE(groups.set_loot_rule(zone.find(a)->char_id, LootRule::RoundRobin).ok());  // the leader's choice
  MobDef foe = mob("road_brigand");
  foe.level = 5;
  foe.aggro_radius = 0;
  foe.damage_mult = 0.1f;
  const auto m = zone.spawn_mob(foe, {2, 0, 0});
  zone.tick(++t, sink);
  // The lead does most of the damage.
  zone.set_target(a, m, sink);
  zone.set_auto_attack(a, true, sink);
  zone.set_target(b, m, sink);
  zone.set_auto_attack(b, true, sink);
  run_until(zone, sink, t, 20 * 120, [&] { const MobEntity* x = zone.find_mob(m); return !x || !x->alive(); });
  EntityId corpse = 0;
  zone.for_each_corpse([&](const CorpseEntity& cc) { corpse = cc.id; });
  REQUIRE(corpse != 0);
  zone.loot_open(a, corpse, sink);
  zone.loot_take(a, corpse, kLootAll, sink);
  zone.loot_close(a, corpse, sink);
  // One stack each: the lead could only take its own.
  CHECK(count_of(*zone.find(a), "rat_whisker") + count_of(*zone.find(a), "bone_chips") == 1);
  zone.loot_open(b, corpse, sink);
  zone.loot_take(b, corpse, kLootAll, sink);
  CHECK(count_of(*zone.find(b), "rat_whisker") + count_of(*zone.find(b), "bone_chips") == 1);
}

// ---------------------------------------------------------------- zone: vendors and bank

TEST_CASE("vendors: buy at the markup, sell for less, limited stock restocks, sold items can be bought back", "[items][zone][vendor]") {
  Zone zone(flats(), content(), kDt, 51);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = at(0, 0, "Shopper", 30);
  i.coin = 2000000;
  i.items.push_back({general_code(0), {"rat_whisker", 10, 0}});
  const auto p = zone.add_player(i);
  const EntityId varra = [&] {
    EntityId id = 0;
    zone.for_each_npc([&](const NpcEntity& n) {
      if (n.def->id == "armorer_varra") id = n.id;
    });
    return id;
  }();
  REQUIRE(varra != 0);
  zone.vendor_open(p, varra, sink);
  CHECK(sink.chat_count(p, "too far") + sink.chat_count(p, "not a merchant") == 1);
  walk_to(zone, sink, t, p, varra);
  zone.vendor_open(p, varra, sink);
  auto w = sink.last<proto::VendorWindow>(p);
  REQUIRE(w);
  REQUIRE_FALSE(w->entries.empty());
  const auto line = std::find_if(w->entries.begin(), w->entries.end(), [](const proto::VendorEntry& e) { return e.item_id == "plate_t5_chest"; });
  REQUIRE(line != w->entries.end());
  CHECK(line->stock == 2);
  const int cha = zone.find(p)->stats.stats.cha;
  CHECK(line->price == vendor_buy_price(item("plate_t5_chest").value, cha, content().rules.economy));
  const auto coin0 = zone.find(p)->coin;
  zone.vendor_buy(p, varra, "plate_t5_chest", 1, false, sink);
  zone.vendor_buy(p, varra, "plate_t5_chest", 1, false, sink);
  zone.vendor_buy(p, varra, "plate_t5_chest", 1, false, sink);
  CHECK(count_of(*zone.find(p), "plate_t5_chest") == 2);
  CHECK(sink.chat_count(p, "sold out") == 1);
  CHECK(zone.find(p)->coin == coin0 - 2 * line->price);
  // Restock: one every ten minutes.
  run(zone, sink, t, static_cast<int>(601.0f / kDt));
  zone.vendor_open(p, varra, sink);
  w = sink.last<proto::VendorWindow>(p);
  const auto again = std::find_if(w->entries.begin(), w->entries.end(), [](const proto::VendorEntry& e) { return e.item_id == "plate_t5_chest" && !e.buyback; });
  CHECK(again->stock == 1);
  // Sell whiskers, then buy one back at the vendor's price.
  const auto coin1 = zone.find(p)->coin;
  zone.vendor_sell(p, varra, general_code(0), 4, sink);
  CHECK(count_of(*zone.find(p), "rat_whisker") == 6);
  CHECK(zone.find(p)->coin == coin1 + 4 * vendor_sell_price(item("rat_whisker").value, cha, content().rules.economy));
  w = sink.last<proto::VendorWindow>(p);
  CHECK(std::any_of(w->entries.begin(), w->entries.end(), [](const proto::VendorEntry& e) { return e.item_id == "rat_whisker" && e.buyback && e.stock == 4; }));
  zone.vendor_buy(p, varra, "rat_whisker", 1, true, sink);
  CHECK(count_of(*zone.find(p), "rat_whisker") == 7);
  // Worn items cannot be sold straight off the body.
  zone.move_item(p, slot_of(*zone.find(p), "plate_t5_chest"), equip_code(EquipSlot::Chest), 0, sink);
  zone.vendor_sell(p, varra, equip_code(EquipSlot::Chest), 0, sink);
  CHECK(zone.find(p)->inv.equip[static_cast<std::size_t>(EquipSlot::Chest)].item_id == "plate_t5_chest");
}

TEST_CASE("the bank: only at a banker, holds items and coin, and closes when you walk away", "[items][zone][bank]") {
  Zone zone(flats(), content(), kDt, 53);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = at(0, 0, "Saver", 10);
  i.coin = 500;
  i.items.push_back({general_code(0), {"longsword_t2", 1, 0}});
  const auto p = zone.add_player(i);
  const EntityId banker = npc_with_role(zone, "banker");
  REQUIRE(banker != 0);
  zone.tick(++t, sink);
  zone.move_item(p, general_code(0), bank_code(0), 0, sink);
  CHECK(zone.find(p)->inv.bank[0].empty());
  CHECK(sink.chat_count(p, "banker") >= 1);
  walk_to(zone, sink, t, p, banker);
  zone.bank_open(p, banker, sink);
  REQUIRE(sink.last<proto::BankWindow>(p));
  CHECK(sink.last<proto::BankWindow>(p)->slots == 16);
  zone.move_item(p, general_code(0), bank_code(0), 0, sink);
  CHECK(zone.find(p)->inv.bank[0].item_id == "longsword_t2");
  zone.bank_coin(p, true, 300, sink);
  CHECK(zone.find(p)->coin == 200);
  CHECK(zone.find(p)->bank_coin == 300);
  zone.bank_coin(p, false, 1000, sink);  // clamps to what is there
  CHECK(zone.find(p)->coin == 500);
  zone.bank_coin(p, true, 100, sink);
  zone.find(p)->pos = {0, 0, 0};
  run(zone, sink, t, 20);
  CHECK(sink.last<proto::BankWindow>(p)->npc_id == 0);
  zone.move_item(p, bank_code(0), general_code(1), 0, sink);
  CHECK(zone.find(p)->inv.bank[0].item_id == "longsword_t2");
}

// ---------------------------------------------------------------- auction house (pure)

TEST_CASE("the auction board: search filters and sorts, expiry, and collection", "[items][auction]") {
  AuctionHouse ah;
  auto add = [&](std::int64_t id, std::int64_t seller, const char* item_id, std::uint64_t price, std::int64_t expires) {
    AuctionListing l;
    l.id = id;
    l.seller_char = seller;
    l.seller_name = "S" + std::to_string(seller);
    l.item_id = item_id;
    l.price = price;
    l.expires_at = expires;
    ah.add(l);
  };
  add(1, 10, "longsword_t2", 500, 100);
  add(2, 10, "longsword_t3", 300, 100);
  add(3, 11, "plate_t1_chest", 50, 100);
  add(4, 11, "trail_rations", 5, 50);
  AuctionQuery q;
  q.text = "LONGSWORD";
  auto page = ah.search(q, content(), 20);
  REQUIRE(page.total == 2);
  CHECK(page.rows[0]->id == 2);  // cheapest first
  q = {};
  q.kind = "armor";
  CHECK(ah.search(q, content(), 20).total == 1);
  q = {};
  q.min_level = 5;
  CHECK(ah.search(q, content(), 20).total == 2);  // longswords t2 (5) and t3 (10)
  q = {};
  q.page = 1;
  CHECK(ah.search(q, content(), 3).rows.size() == 1);
  CHECK(ah.count_for(10) == 2);
  const auto expired = ah.expire(60);
  REQUIRE(expired.size() == 1);
  CHECK(expired[0]->id == 4);
  q = {};
  CHECK(ah.search(q, content(), 20).total == 3);  // expired listings leave the board
  ah.find(1)->state = ListingState::Sold;
  CHECK(ah.collectable(10).size() == 1);
  CHECK(ah.collectable(11).size() == 1);
  q = {};
  q.mine = 10;
  CHECK(ah.search(q, content(), 20).total == 2);  // your own, any state
}

