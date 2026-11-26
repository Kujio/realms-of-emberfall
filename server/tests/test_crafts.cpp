// Phase 13: tradeskills. The pure combine rules, the pack checks, the shipped recipes and stations,
// combining at a city's forge, failures, and craft skills saved with the character. Phase 16: combines take
// time and stop when the crafter moves or fights, recipe books, and quests that count what you make.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>

#include "game/crafting.hpp"
#include "server_support.hpp"
#include "zone_support.hpp"

using namespace zt;
using Catch::Approx;

namespace {

// A city zone with these rules, and the id of its station of `kind`.
struct Yard {
  Content c;
  std::unique_ptr<Zone> zone;
  RecordingSink sink;
  std::uint32_t t = 0;
  EntityId station = 0;
  Vec3 spot;  // where the station stands

  explicit Yard(const CraftRules& rules, const char* city = "emberhold", const char* kind = "forge", std::uint64_t seed = 11) : c(content()) {
    c.crafts.rules = rules;
    const ZoneDef* z = c.find_zone(city);
    REQUIRE(z);
    zone = std::make_unique<Zone>(*z, c, kDt, seed);
    zone->for_each_npc([&](const NpcEntity& n) {
      if (n.def->role == "station" && n.def->station == kind) {
        station = n.id;
        spot = n.pos;
      }
    });
    REQUIRE(station != 0);
  }
  EntityId crafter(const char* name = "Smith") {
    const EntityId p = zone->add_player(at(spot.x + 1.5f, spot.z, name, 20));
    zone->send_initial_state(p, sink);
    tick(2);
    return p;
  }
  void tick(int n = 1) {
    for (int i = 0; i < n; ++i) zone->tick(++t, sink);
  }
  // Phase 16: a combine started; the work finishes after combine_seconds.
  void work() { tick(static_cast<int>(c.crafts.rules.combine_seconds / kDt) + 2); }
  // Starts a combine and works it to the end.
  void make(EntityId p, const char* recipe, std::uint16_t request = 0) {
    zone->craft_combine(p, station, recipe, sink, request);
    work();
  }
  int pack(EntityId p, const char* item) { return count_in_pack(zone->find(p)->inv, item); }
};

CraftRules certain() {
  CraftRules r;
  r.success_min = r.success_max = 1.0f;
  r.skill_up_base = r.skill_up_max = 1.0f;
  return r;
}

}  // namespace

TEST_CASE("combine rules: success and skill-up chances by distance to the trivial", "[crafts][rules]") {
  const CraftRules r;
  CHECK(combine_chance(100, 100, r) == Approx(0.95f));
  CHECK(combine_chance(200, 100, r) == Approx(0.95f));
  CHECK(combine_chance(90, 100, r) == Approx(0.95f - 10 * 0.012f));
  CHECK(combine_chance(0, 290, r) == Approx(0.05f));  // the floor
  CHECK(craft_skill_up_chance(100, 100, 300, r) == 0.0f);  // trivial: nothing to learn
  CHECK(craft_skill_up_chance(150, 100, 300, r) == 0.0f);
  CHECK(craft_skill_up_chance(99, 100, 300, r) == Approx(0.2f));
  CHECK(craft_skill_up_chance(90, 100, 300, r) == Approx(0.29f));
  CHECK(craft_skill_up_chance(0, 290, 300, r) == Approx(0.6f));  // the most a combine teaches
  CHECK(craft_skill_up_chance(300, 310, 300, r) == 0.0f);        // at the cap
  Rng rng(3);
  CHECK(units_lost(5, 0.0f, rng) == 0);
  CHECK(units_lost(5, 1.0f, rng) == 5);
  CHECK(units_lost(0, 0.5f, rng) == 0);
  const int half = units_lost(2000, 0.5f, rng);
  CHECK(half > 850);
  CHECK(half < 1150);
}

TEST_CASE("the pack: materials count and come from pack slots and bags, never the bank or worn gear", "[crafts][rules]") {
  const Content& c = content();
  Inventory inv;
  inv.general[0] = {"copper_ore", 3, 0};
  inv.general[1] = {"cloth_pouch", 1, 0};
  inv.general_bags[1][0] = {"copper_ore", 4, 0};
  inv.bank[0] = {"copper_ore", 10, 0};
  CHECK(count_in_pack(inv, "copper_ore") == 7);
  CHECK(in_pack(general_code(0)));
  CHECK(in_pack(bag_code(false, 1, 0)));
  CHECK_FALSE(in_pack(bank_code(0)));
  CHECK_FALSE(in_pack(equip_code(EquipSlot::Primary)));
  CHECK(take_from_pack(inv, "copper_ore", 5) == 5);
  CHECK(inv.general_bags[1][0].empty());  // the bag first
  CHECK(inv.general[0].count == 2);
  CHECK(inv.bank[0].count == 10);         // the bank is never touched
  CHECK(take_from_pack(inv, "copper_ore", 9) == 2);
  CHECK(inv.general[0].empty());
  (void)c;
}

TEST_CASE("check_combine: missing ingredients and tools, and room for what is made", "[crafts][rules]") {
  const Content& c = content();
  const RecipeDef* bar = c.crafts.find_recipe("copper_bar");
  const RecipeDef* coif = c.crafts.find_recipe("chain_t1_head");
  REQUIRE(bar);
  REQUIRE(coif);
  Inventory inv;
  auto plan = check_combine(inv, c, *bar);
  CHECK(plan.check == CombineCheck::MissingIngredient);
  CHECK(plan.item == "copper_ore");
  CHECK(plan.need == 1);
  // A full pack whose ore and coal are used up: their slots make room for the bar.
  inv.general[0] = {"copper_ore", 1, 0};
  inv.general[1] = {"forge_coal", 1, 0};
  for (std::size_t i = 2; i < kGeneralSlots; ++i) inv.general[i] = {"training_sword", 1, 0};
  CHECK(check_combine(inv, c, *bar).check == CombineCheck::Ok);
  // The same pack with ore and coal left over: no room.
  inv.general[0].count = 2;
  inv.general[1].count = 2;
  plan = check_combine(inv, c, *bar);
  CHECK(plan.check == CombineCheck::NoRoom);
  // A coif needs the smithing hammer, which is never used up.
  Inventory smith;
  smith.general[0] = {"copper_bar", 5, 0};
  plan = check_combine(smith, c, *coif);
  CHECK(plan.check == CombineCheck::MissingTool);
  CHECK(plan.item == "smithing_hammer");
  smith.general[1] = {"smithing_hammer", 1, 0};
  CHECK(check_combine(smith, c, *coif).check == CombineCheck::Ok);
  // A lore result the character already owns (here in the bank) is refused before anything is used.
  Content lore = c;
  for (auto& it : lore.items)
    if (it.id == "chain_t1_head") it.lore = true;
  lore.index_items();
  smith.bank[0] = {"chain_t1_head", 1, 0};
  plan = check_combine(smith, lore, *coif);
  CHECK(plan.check == CombineCheck::Lore);
  CHECK(plan.item == "chain_t1_head");
}

TEST_CASE("shipped tradeskills: stations in every city, tools for sale, every ingredient obtainable, a steady ladder", "[crafts][content]") {
  const Content& c = content();
  REQUIRE(c.crafts.tradeskills.size() == 6);
  const std::vector<std::string> tools{"smithing_hammer", "sewing_needle", "fletching_knife", "jewelers_loupe"};
  for (const auto& city : c.cities) {
    INFO(city.id);
    std::set<std::string> kinds;
    bool supplier = false;
    for (const auto& n : c.npcs) {
      if (n.zone != city.zone) continue;
      if (n.role == "station") kinds.insert(n.station);
      if (n.role == "vendor" && std::all_of(tools.begin(), tools.end(), [&](const std::string& t) {
            return std::any_of(n.stock.begin(), n.stock.end(), [&](const VendorStock& s) { return s.item == t; });
          }))
        supplier = true;
    }
    for (const auto& t : c.crafts.tradeskills) CHECK(kinds.contains(t.station));
    CHECK(supplier);
  }
  // What a player can get: vendor stock, the loot of monsters the zones spawn (and their named rares), and what
  // recipes make (repeated until nothing new).
  std::set<std::string> have;
  for (const auto& n : c.npcs)
    for (const auto& s : n.stock) have.insert(s.item);
  std::set<std::string> spawned;
  for (const auto& z : c.zones)
    for (const auto& s : z.spawns) {
      spawned.insert(s.mob);
      if (!s.rare.empty()) spawned.insert(s.rare);
    }
  std::set<std::string> awarded;
  for (const auto& m : c.mobs)
    if (spawned.contains(m.id)) awarded.insert(m.loot);
  for (const auto& t : c.loot_tables) {
    if (!awarded.contains(t.id) || t.max_drops <= 0) continue;
    for (const auto& d : t.drops)
      if (d.chance > 0.0f) have.insert(d.item);
    for (const auto& p : t.pools)
      if (p.chance > 0.0f && p.rolls > 0)
        for (const auto& e : p.entries)
          if (e.weight > 0.0f) have.insert(e.item);
  }
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& r : c.crafts.recipes) {
      const bool can = std::all_of(r.ingredients.begin(), r.ingredients.end(), [&](const ItemCount& i) { return have.contains(i.item); }) &&
                       std::all_of(r.tools.begin(), r.tools.end(), [&](const std::string& t) { return have.contains(t); });
      if (can && have.insert(r.result.item).second) grew = true;
    }
  }
  for (const auto& r : c.crafts.recipes) {
    INFO(r.id);
    // Every recipe can be made from what the world offers: each input, even when a vendor sells the result.
    for (const auto& i : r.ingredients) CHECK(have.contains(i.item));
    for (const auto& tool : r.tools) CHECK(have.contains(tool));
  }
  // No tradeskill leaves a stretch of more than 60 points without a recipe to learn from, and each can be taught to
  // the cap (a recipe teaches only below its trivial).
  for (const auto& t : c.crafts.tradeskills) {
    std::vector<int> trivials{0};
    for (const auto& r : c.crafts.recipes)
      if (r.tradeskill == t.id) trivials.push_back(r.trivial);
    std::sort(trivials.begin(), trivials.end());
    INFO(t.id);
    CHECK(trivials.size() > 5);
    for (std::size_t i = 1; i < trivials.size(); ++i) CHECK(trivials[i] - trivials[i - 1] <= 60);
    CHECK(trivials.back() >= c.crafts.rules.max_skill);
    // A skill just below the cap can still rise at the top recipe, and nothing teaches past the cap.
    const RecipeDef* best = nullptr;
    for (const auto& r : c.crafts.recipes)
      if (r.tradeskill == t.id && (best == nullptr || r.trivial > best->trivial)) best = &r;
    REQUIRE(best != nullptr);
    const int cap = c.crafts.rules.max_skill;
    CHECK(craft_skill_up_chance(cap - 1, best->trivial, cap, c.crafts.rules) > 0.0f);
    CHECK(craft_skill_up_chance(cap, best->trivial, cap, c.crafts.rules) == 0.0f);
  }
}

TEST_CASE("combining at the Emberhold forge: the window, a bar made, a skill gained, the pace, the wrong station, walking away",
          "[crafts][zone]") {
  Yard y(certain());
  const EntityId p = y.crafter();
  y.zone->give(p, "copper_ore", 4, y.sink);
  y.zone->give(p, "forge_coal", 4, y.sink);
  y.zone->craft_open(p, y.station, y.sink);
  auto win = y.sink.last<proto::CraftWindow>(p);
  REQUIRE(win);
  CHECK(win->npc_id == y.station);
  CHECK(win->name == "Forge");
  CHECK(win->tradeskill == "smithing");
  // Every tradeskill's value arrived on entering the world.
  auto skills = y.sink.all<proto::CraftSkills>(p);
  REQUIRE_FALSE(skills.empty());
  CHECK(skills.front().skills.size() == 6);

  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 7);
  auto started = y.sink.last<proto::CraftStarted>(p);
  REQUIRE(started);
  CHECK(started->recipe_id == "copper_bar");
  CHECK(started->request == 7);
  CHECK(started->work_ms == static_cast<std::uint32_t>(std::lround(y.c.crafts.rules.combine_seconds * 1000.0f)));
  CHECK_FALSE(y.sink.last<proto::CraftResult>(p));  // nothing made yet
  CHECK(y.pack(p, "copper_ore") == 4);
  y.work();
  auto res = y.sink.last<proto::CraftResult>(p);
  REQUIRE(res);
  CHECK(res->success);
  CHECK(res->made == 1);
  CHECK(res->request == 7);  // the client's number comes back
  CHECK_FALSE(res->refused);
  CHECK(y.pack(p, "copper_bar") == 1);
  CHECK(y.pack(p, "copper_ore") == 3);
  CHECK(y.pack(p, "forge_coal") == 3);
  CHECK(y.zone->craft_skill(*y.zone->find(p), "smithing") == 1);
  auto up = y.sink.last<proto::CraftSkills>(p);
  REQUIRE(up);
  CHECK(up->announce);
  REQUIRE(up->skills.size() == 1);
  CHECK(up->skills[0].id == "smithing");
  CHECK(up->skills[0].value == 1);
  CHECK(y.sink.chat_count(p, "You have made Copper Bar.") == 1);

  // One combine at a time. A refusal is answered too, so the window can send again at once.
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 8);
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 9);
  CHECK(y.sink.chat_count(p, "still at work") == 1);
  CHECK(y.pack(p, "copper_bar") == 1);
  auto refused = y.sink.last<proto::CraftResult>(p);
  REQUIRE(refused);
  CHECK(refused->refused);
  CHECK_FALSE(refused->success);
  CHECK(refused->request == 9);
  CHECK(refused->recipe_id == "copper_bar");
  y.work();
  CHECK(y.sink.last<proto::CraftResult>(p)->request == 8);
  CHECK(y.pack(p, "copper_bar") == 2);
  CHECK(y.zone->craft_skill(*y.zone->find(p), "smithing") == 2);

  // A recipe of another tradeskill, and one without its tool: refused, nothing used.
  y.zone->give(p, "flax_bundle", 2, y.sink);
  y.zone->craft_combine(p, y.station, "linen_bolt", y.sink);
  CHECK(y.sink.chat_count(p, "Linen Bolt is not made at the Forge; it needs a Loom.") == 1);
  CHECK(y.pack(p, "flax_bundle") == 2);
  y.zone->give(p, "copper_bar", 2, y.sink);
  y.zone->craft_combine(p, y.station, "chain_t1_head", y.sink);
  CHECK(y.sink.chat_count(p, "You need a Smithing Hammer in your pack for that.") == 1);
  CHECK(y.pack(p, "copper_bar") == 4);
  y.zone->craft_combine(p, y.station, "no_such_recipe", y.sink, 9);
  CHECK(y.sink.chat_count(p, "There is no such recipe.") == 1);
  auto unknown = y.sink.last<proto::CraftResult>(p);
  REQUIRE(unknown);
  CHECK((unknown->refused && unknown->request == 9));

  // At the trivial nothing more is learned.
  y.zone->find(p)->crafts = {{"smithing", 10}};  // the bar's trivial
  y.make(p, "copper_bar");
  CHECK(y.pack(p, "copper_bar") == 5);
  CHECK(y.zone->craft_skill(*y.zone->find(p), "smithing") == 10);

  // Walking away closes the window; combining needs the station in reach.
  y.zone->find(p)->pos = {y.spot.x + 40.0f, y.spot.y, y.spot.z};
  y.tick(40);
  auto closed = y.sink.last<proto::CraftWindow>(p);
  REQUIRE(closed);
  CHECK(closed->npc_id == 0);
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink);
  CHECK(y.sink.chat_count(p, "You must stand at a crafting station to do that.") == 1);
  CHECK(y.pack(p, "copper_bar") == 5);
}

TEST_CASE("a failed combine loses materials by chance and never its tools", "[crafts][zone]") {
  CraftRules doomed;
  doomed.success_min = doomed.success_max = 0.0f;
  doomed.fail_loss_chance = 1.0f;
  {
    Yard y(doomed);
    const EntityId p = y.crafter();
    y.zone->give(p, "copper_bar", 2, y.sink);
    y.zone->give(p, "smithing_hammer", 1, y.sink);
    y.make(p, "chain_t1_head");
    auto res = y.sink.last<proto::CraftResult>(p);
    REQUIRE(res);
    CHECK_FALSE(res->success);
    CHECK(res->made == 0);
    CHECK(y.pack(p, "copper_bar") == 0);
    CHECK(y.pack(p, "smithing_hammer") == 1);
    CHECK(y.pack(p, "chain_t1_head") == 0);
    CHECK(y.sink.chat_count(p, "You lacked the skill to make Rusted Ring Coif. You lost 2 x Copper Bar.") == 1);
  }
  doomed.fail_loss_chance = 0.0f;
  {
    Yard y(doomed);
    const EntityId p = y.crafter();
    y.zone->give(p, "copper_bar", 2, y.sink);
    y.zone->give(p, "smithing_hammer", 1, y.sink);
    y.make(p, "chain_t1_head");
    CHECK(y.pack(p, "copper_bar") == 2);
    CHECK(y.sink.chat_count(p, "Your materials are unharmed.") == 1);
  }
}

TEST_CASE("a combine in progress stops when the crafter moves, fights or casts, and uses nothing", "[crafts][zone]") {
  Yard y(certain());
  const EntityId p = y.crafter();
  y.zone->give(p, "copper_ore", 4, y.sink);
  y.zone->give(p, "forge_coal", 4, y.sink);
  // Walking off.
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 1);
  y.tick(5);
  y.zone->find(p)->pos.x += 1.0f;
  y.tick();
  auto stopped = y.sink.last<proto::CraftResult>(p);
  REQUIRE(stopped);
  CHECK((stopped->refused && stopped->request == 1));
  CHECK(y.sink.chat_count(p, "You moved, and stop working on Copper Bar.") == 1);
  y.work();
  CHECK(y.pack(p, "copper_bar") == 0);
  CHECK(y.pack(p, "copper_ore") == 4);
  // A fight starting, even on the very tick the work began.
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 2);
  y.zone->mark_in_combat(p);
  y.tick();
  CHECK(y.sink.chat_count(p, "The fight pulls you away from Copper Bar.") == 1);
  CHECK(y.sink.last<proto::CraftResult>(p)->request == 2);
  // Nobody starts a combine mid-fight.
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 3);
  CHECK(y.sink.chat_count(p, "You cannot craft while you are fighting.") == 1);
  CHECK(y.pack(p, "copper_ore") == 4);
  // Once the fight is over, it goes through; a trade opening mid-work stops it (its offer is never spent).
  y.tick(static_cast<int>(y.c.rules.combat.in_combat_seconds / kDt) + 2);
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 3);
  y.tick(5);
  y.zone->find(p)->trade = 99;
  y.tick();
  CHECK(y.sink.chat_count(p, "You stop working on Copper Bar to trade.") == 1);
  y.zone->find(p)->trade = 0;
  CHECK(y.pack(p, "copper_ore") == 4);
  // Any ability that fires stops it too, even an instant one on yourself.
  y.zone->craft_combine(p, y.station, "copper_bar", y.sink, 6);
  y.tick(2);
  y.zone->use_ability(p, "second_wind", p, y.sink);
  REQUIRE(y.sink.last<proto::AbilityResult>(p)->ok);
  CHECK(y.sink.chat_count(p, "You stop working on Copper Bar.") >= 1);
  CHECK(y.sink.last<proto::CraftResult>(p)->request == 6);
  y.work();
  CHECK(y.pack(p, "copper_ore") == 4);
  y.make(p, "copper_bar", 4);
  CHECK(y.sink.last<proto::CraftResult>(p)->success);
  CHECK(y.pack(p, "copper_bar") == 1);
}

TEST_CASE("recipe books: later recipes are learned by reading their book, which is spent; known books are saved", "[crafts][zone]") {
  Yard y(certain());
  const EntityId p = y.crafter();
  const RecipeDef* iron = y.c.crafts.find_recipe("iron_bar");
  REQUIRE(iron);
  REQUIRE(iron->book == "smithing_book_2");
  REQUIRE(y.c.crafts.find_recipe("copper_bar")->book.empty());  // the first band is known from the start
  y.zone->give(p, "iron_ore", 2, y.sink);
  y.zone->give(p, "forge_coal", 2, y.sink);
  y.zone->find(p)->crafts = {{"smithing", 80}};
  y.zone->craft_combine(p, y.station, "iron_bar", y.sink, 5);
  CHECK(y.sink.chat_count(p, "You do not know how to make Iron Bar: it is taught in Forgemaster's Folio, Volume II.") == 1);
  CHECK(y.sink.last<proto::CraftResult>(p)->refused);
  // Reading the book: learned, the book spent, and the books read reach the client.
  y.zone->give(p, "smithing_book_2", 1, y.sink);
  SlotCode slot = kNoSlot;
  y.zone->find(p)->inv.for_each([&](SlotCode code, const ItemStackState& st) {
    if (st.item_id == "smithing_book_2") slot = code;
  });
  REQUIRE(slot != kNoSlot);
  y.zone->use_item(p, slot, 0, y.sink);
  CHECK(y.sink.chat_count(p, "You study Forgemaster's Folio, Volume II and learn") == 1);
  CHECK(y.pack(p, "smithing_book_2") == 0);
  auto known = y.sink.last<proto::CraftSkills>(p);
  REQUIRE(known);
  CHECK(known->books == std::vector<std::string>{"smithing_book_2"});
  CHECK(y.zone->knows_recipe(*y.zone->find(p), *iron));
  y.make(p, "iron_bar");
  CHECK(y.pack(p, "iron_bar") == 1);
  // A second copy teaches nothing new and is kept.
  y.zone->give(p, "smithing_book_2", 1, y.sink);
  y.zone->find(p)->inv.for_each([&](SlotCode code, const ItemStackState& st) {
    if (st.item_id == "smithing_book_2") slot = code;
  });
  y.zone->use_item(p, slot, 0, y.sink);
  CHECK(y.sink.chat_count(p, "You already know everything in Forgemaster's Folio, Volume II.") == 1);
  CHECK(y.pack(p, "smithing_book_2") == 1);
}

TEST_CASE("a crafting quest counts what you make, not what you buy, and rewards the next recipe book", "[crafts][quests]") {
  Yard y(certain());
  const EntityId p = y.crafter();
  const QuestDef* q = y.c.find_quest("craft_copper_bars");
  REQUIRE(q);
  REQUIRE(q->steps.size() == 1);
  REQUIRE(q->steps[0].type == QuestStepType::Craft);
  REQUIRE(q->steps[0].recipe == "copper_bar");
  REQUIRE(y.zone->find(p)->quests.accept(*q));
  y.zone->give(p, "copper_bar", 5, y.sink);  // bought or handed bars count for nothing
  CHECK(y.zone->find(p)->quests.find("craft_copper_bars")->count == 0);
  y.zone->give(p, "copper_ore", 5, y.sink);
  y.zone->give(p, "forge_coal", 5, y.sink);
  for (int i = 0; i < 5; ++i) y.make(p, "copper_bar");
  CHECK(y.sink.chat_count(p, "Fire and Copper:") == 5);
  CHECK(y.zone->find(p)->quests.ready(*q));
  bool rewards_book = false;
  for (const auto& r : q->items) rewards_book = rewards_book || r.item == "smithing_book_2";
  CHECK(rewards_book);
}

TEST_CASE("every station in each city's yard opens its own tradeskill", "[crafts][zone]") {
  for (const char* city : {"emberhold", "thornwood", "cragmouth"}) {
    INFO(city);
    for (const auto& t : content().crafts.tradeskills) {
      Yard y(certain(), city, t.station.c_str());
      const EntityId p = y.crafter();
      y.zone->craft_open(p, y.station, y.sink);
      auto win = y.sink.last<proto::CraftWindow>(p);
      REQUIRE(win);
      CHECK(win->tradeskill == t.id);
    }
  }
}

TEST_CASE("craft skills are saved with the character and come back on entering the world", "[crafts][server]") {
  using namespace wire;
  Harness h(200, false, "");
  Client a(h);
  auto ew = a.login_and_enter("smith", "Smithy");
  Zone* z = h.server->zone(ew.zone_id);
  REQUIRE(z);
  PlayerEntity* p = z->find(ew.entity_id);
  REQUIRE(p);
  p->crafts = {{"smithing", 37}, {"brewing", 5}};
  p->books = {"brewing_book_3", "smithing_book_2"};  // Phase 16: books read are saved too
  p->save_dirty = true;
  const auto chars_before = h.received<proto::CharList>(a.peer).size();
  a.send(proto::Logout{});
  h.tick(5);
  auto list = h.last<proto::CharList>(a.peer);
  REQUIRE(list);
  REQUIRE(h.received<proto::CharList>(a.peer).size() > chars_before);
  REQUIRE_FALSE(list->characters.empty());
  a.send(proto::EnterWorld{list->characters[0].id});
  h.tick();
  auto again = h.last<proto::EnterWorldResult>(a.peer);
  REQUIRE(again);
  REQUIRE(again->ok);
  auto skills = h.last<proto::CraftSkills>(a.peer);
  REQUIRE(skills);
  std::map<std::string, int> got;
  for (const auto& s : skills->skills) got[s.id] = s.value;
  CHECK(got["smithing"] == 37);
  CHECK(got["brewing"] == 5);
  CHECK(got["tailoring"] == 0);
  CHECK(skills->books == std::vector<std::string>{"brewing_book_3", "smithing_book_2"});
}

TEST_CASE("every combine is answered, even one the rate limits drop", "[crafts][server]") {
  using namespace wire;
  Harness h(200, false, "");
  Client a(h);
  a.login_and_enter("spammer", "Spammy");
  // Past the packet burst (120 by default) in one instant: the item limit refuses some, the packet limit drops the
  // rest before any handler. Each still comes back refused with its own number, so the station window never waits
  // on a combine that went nowhere.
  const std::size_t before = h.received<proto::CraftResult>(a.peer).size();
  constexpr std::uint16_t kSent = 150;
  for (std::uint16_t i = 1; i <= kSent; ++i) a.send(proto::CraftCombine{0, "copper_bar", i});
  const auto results = h.received<proto::CraftResult>(a.peer);
  REQUIRE(results.size() - before == kSent);
  std::set<std::uint16_t> answered;
  for (std::size_t i = before; i < results.size(); ++i) {
    CHECK(results[i].refused);
    answered.insert(results[i].request);
  }
  CHECK(answered.size() == kSent);
  CHECK(h.transport.disconnecting.empty());  // not yet flooding enough to be kicked
}

TEST_CASE("a malformed combine dropped by the packet limit still counts as a protocol violation", "[crafts][server]") {
  using namespace wire;
  Harness h(200, false, "");
  Client a(h);
  a.login_and_enter("garbler", "Garbly");
  for (std::uint16_t i = 1; i <= 130; ++i) a.send(proto::CraftCombine{0, "copper_bar", i});  // past the burst
  REQUIRE(h.transport.disconnecting.empty());
  // A CraftCombine id with its body cut short, again and again: each is a violation, so the client is kicked after
  // max_violations of them, long before the flood kick.
  std::vector<std::uint8_t> bad = proto::encode_packet(proto::CraftCombine{0, "copper_bar", 1});
  bad.resize(4);
  for (std::uint32_t i = 0; i < h.cfg.max_violations + 1; ++i) h.server->on_packet(a.peer, 0, bad);
  auto kick = h.last<proto::Kick>(a.peer);
  REQUIRE(kick);
  CHECK(kick->reason == "Disconnected: too many protocol errors.");
}
