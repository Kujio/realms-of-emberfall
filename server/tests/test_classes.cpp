// Phase 4: class mechanics. Pure rules (positionals, dual wield, charm breaks, threat drops,
// tracking, proficiencies) and zone behaviour: pets (summon, commands, tanking, death), Bard songs
// (pulses, twisting, silencing), backstab positioning, charm and its breaks, feign death, evade,
// hide/sneak, damage shields, lifetaps and drains, summoned items, illusions, tanks holding aggro,
// and every class and race entering the world with its kit.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "game/class_rules.hpp"
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

int events(const RecordingSink& s, EntityId to, CombatKind kind) {
  int n = 0;
  for (const auto& e : s.all<proto::CombatEvent>(to))
    if (e.kind == static_cast<std::uint8_t>(kind)) ++n;
  return n;
}

bool has_buff(const std::vector<ActiveBuff>& b, std::string_view id) {
  return std::any_of(b.begin(), b.end(), [&](const ActiveBuff& x) { return x.ability_id == id; });
}

// A sturdy target that hits like a wet noodle and never resists more than the floor.
MobDef dummy(int level, float hp_mult = 50.0f, float aggro = 0.0f) {
  MobDef m = mob("road_brigand");
  m.id = "test_dummy";
  m.name = "a practice dummy";
  m.level = level;
  m.hp_mult = hp_mult;
  m.damage_mult = 0.01f;
  m.aggro_radius = aggro;
  m.assist = false;
  m.flee_hp_pct = 0;
  m.wander_radius = 0;
  m.loot = "";
  m.resists = {0, 0, 0, 0, 0, 0};
  return m;
}

// Waits out the GCD and recast, casts from a gem, and waits for the cast to end. Returns true if
// the spell went off.
bool cast(Zone& z, RecordingSink& s, std::uint32_t& t, EntityId p, int gem, EntityId target) {
  auto ready = [&] {
    const PlayerEntity& me = *z.find(p);
    if (me.cast.casting() || z.now() < me.gcd_until) return false;
    for (const auto& [id, at] : me.cooldowns)
      if (id == me.gems[static_cast<std::size_t>(gem)] && z.now() < at) return false;
    return true;
  };
  run_until(z, s, t, 20 * 60, ready);
  const auto oks = s.all<proto::AbilityResult>(p).size();
  z.cast_spell(p, gem, target, s);
  run_until(z, s, t, 20 * 30, [&] { return !z.find(p) || !z.find(p)->cast.casting(); });
  const auto r = s.all<proto::AbilityResult>(p);
  return r.size() > oks && r.back().ok;
}

// Uses an ability once its GCD and cooldown allow. Returns the AbilityResult.
proto::AbilityResult use(Zone& z, RecordingSink& s, std::uint32_t& t, EntityId p, const std::string& id, EntityId target) {
  run_until(z, s, t, 20 * 60, [&] {
    const PlayerEntity& me = *z.find(p);
    if (z.now() < me.gcd_until) return false;
    for (const auto& [cid, at] : me.cooldowns)
      if (cid == id && z.now() < at) return false;
    return true;
  });
  z.use_ability(p, id, target, s);
  return *s.last<proto::AbilityResult>(p);
}

ZonePlayerInit caster(const char* cls, int level, std::vector<std::string> gems, float x = 0, float zpos = 0, const char* name = "Caster") {
  ZonePlayerInit i = with_spells(as_class(at(x, zpos, name, level), cls), std::move(gems));
  return i;
}

}  // namespace

// ---------------------------------------------------------------- pure rules

TEST_CASE("behind checks use the target's facing and the configured arc", "[classes][rules]") {
  const Vec3 t{0, 0, 0};
  // yaw 0 faces -Z, so +Z is behind.
  CHECK(is_behind(t, 0.0f, {0, 0, 3}, 120.0f));
  CHECK_FALSE(is_behind(t, 0.0f, {0, 0, -3}, 120.0f));
  CHECK_FALSE(is_behind(t, 0.0f, {3, 0, 0}, 120.0f));   // beside: 90 degrees off the back
  CHECK(is_behind(t, 0.0f, {3, 0, 0}, 200.0f));         // ...which a wide arc accepts
  CHECK(is_behind(t, 0.0f, {1.5f, 0, 3}, 120.0f));      // ~27 degrees off the back
  CHECK(is_behind(t, kPi, {0, 0, -3}, 120.0f));         // turned around: -Z is now behind
  CHECK(is_behind(t, 0.0f, {0, 0, 0}, 120.0f));         // on top of it counts
}

TEST_CASE("dual wield, enhanced damage, stealth and charm chances stay in range and move the right way", "[classes][rules]") {
  const Rules& r = content().rules;
  CHECK(dual_wield_chance(0, r.combat) == 0.0f);
  CHECK(dual_wield_chance(100, r.combat) < dual_wield_chance(200, r.combat));
  CHECK(dual_wield_chance(1000, r.combat) == Approx(r.combat.max_dual_wield_chance));
  CHECK(enhanced_damage_mult(0, r.combat) == 1.0f);
  CHECK(enhanced_damage_mult(250, r.combat) == Approx(1.0f + r.combat.enhanced_damage_max));
  CHECK(hide_chance(0, r.stealth) < hide_chance(200, r.stealth));
  CHECK(feign_chance(200, 0, r.stealth) > feign_chance(200, 10, r.stealth));
  CHECK(feign_chance(10000, 0, r.stealth) == Approx(r.stealth.max_chance));
  CHECK(pick_lock_chance(0, 50, r.stealth) == Approx(r.stealth.min_chance));
  // Charms break more often on higher-level targets and less often for charismatic casters.
  CHECK(charm_break_chance(30, 35, 75, r.pets) > charm_break_chance(30, 25, 75, r.pets));
  CHECK(charm_break_chance(30, 30, 150, r.pets) < charm_break_chance(30, 30, 75, r.pets));
  CHECK(charm_break_chance(1, 60, 0, r.pets) == Approx(r.pets.max_charm_break));
  CHECK(charm_break_chance(60, 1, 255, r.pets) == Approx(r.pets.min_charm_break));
}

TEST_CASE("threat scaling, threat drops, compass directions, and proficiencies", "[classes][rules]") {
  CHECK(threat_scale(1.5f, 0.0f) == Approx(1.5));
  CHECK(threat_scale(1.0f, 100.0f) == Approx(2.0));
  CHECK(threat_after_drop(100.0, 60.0f) == Approx(40.0));
  CHECK(threat_after_drop(100.0, 150.0f) == 0.0);
  CHECK(std::string(compass_direction(0, -10)) == "north");
  CHECK(std::string(compass_direction(10, 0)) == "east");
  CHECK(std::string(compass_direction(7, 7)) == "south-east");
  CHECK(std::string(compass_direction(-7, -7)) == "north-west");
  const Content& c = content();
  const ClassDef* war = c.find_class("warrior");
  const ClassDef* wiz = c.find_class("wizard");
  const ClassDef* monk = c.find_class("monk");
  REQUIRE((war && wiz && monk));
  CHECK(can_wear_armor(*war, "plate"));
  CHECK(can_wear_armor(*war, "cloth"));
  CHECK_FALSE(can_wear_armor(*wiz, "leather"));
  CHECK_FALSE(can_wear_armor(*monk, "chain"));
  CHECK_FALSE(can_wear_armor(*war, "mithril"));
  CHECK(can_use_weapon(*monk, "hand_to_hand"));
  CHECK_FALSE(can_use_weapon(*wiz, "2h_slash"));
  CHECK(war->combat.threat_mult > wiz->combat.threat_mult);
}

TEST_CASE("a pet template becomes a mob at the spell's level", "[classes][pets][rules]") {
  const PetDef* fire = content().find_pet("ember_servant");
  REQUIRE(fire);
  const MobDef m = make_pet_def(*fire, 20, "Kael");
  CHECK(m.name == "Kael's ember servant");
  CHECK(m.level == 20);
  CHECK(m.aggro_radius == 0.0f);
  CHECK(m.charm_immune);
  CHECK(pet_damage_shield(*fire, 20) == 10);
  const MobStats lo = mob_stats(make_pet_def(*fire, 10, ""), content().rules.mob_scaling);
  const MobStats hi = mob_stats(make_pet_def(*fire, 40, ""), content().rules.mob_scaling);
  CHECK(hi.max_hp > lo.max_hp * 3);
  CHECK(hi.hit.max > lo.hit.max);
}

// ---------------------------------------------------------------- pets

TEST_CASE("a magician summons a pet that follows, attacks on command, tanks, and earns its owner the kill", "[classes][pets][zone]") {
  Zone zone(test_zone(), content(), kDt, 41);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("magician", 22, {"call_blaze_servant", "flame_bolt"}));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, p, 0, 0));
  const EntityId pet = zone.find(p)->pet;
  REQUIRE(pet != 0);
  const MobEntity* pm = zone.find_mob(pet);
  REQUIRE(pm);
  CHECK(pm->owner == p);
  CHECK(pm->def->level == 22);
  CHECK(pm->def->name == "Caster's ember servant");
  // A second pet is refused.
  run(zone, sink, t, 20 * 7);  // the recast timer
  zone.cast_spell(p, 0, 0, sink);
  CHECK(sink.last<proto::AbilityResult>(p)->message == "You already have a pet.");
  // The owner sees a pet window, and the spawn names the owner.
  auto st = sink.last<proto::PetStatus>(p);
  REQUIRE(st);
  CHECK(st->pet_id == pet);
  bool saw_owner = false;
  for (const auto& sp : sink.all<proto::EntitySpawn>(p))
    if (sp.entity_id == pet) saw_owner = sp.owner_id == p && (sp.flags2 & kFlag2Pet) != 0;
  CHECK(saw_owner);

  // Follow: walk away and the pet keeps up.
  zone.find(p)->pos = {30, 0, 0};
  run(zone, sink, t, 20 * 8);
  CHECK(horizontal_distance(zone.find_mob(pet)->pos, zone.find(p)->pos) < 6.0f);

  // Attack on command: the mob turns on the pet, not the owner.
  MobDef d = dummy(18, 1.2f);
  d.damage_mult = 0.6f;
  const auto m = zone.spawn_mob(d, {40, 0, 0});
  zone.tick(++t, sink);
  zone.pet_command(p, PetOrder::Attack, m, sink);
  CHECK(sink.chat_count(p, "Attacking") == 1);
  run_until(zone, sink, t, 20 * 10, [&] { return zone.find_mob(m)->threat.contains(pet); });
  CHECK(zone.find_mob(m)->threat.top() == pet);
  CHECK_FALSE(zone.find_mob(m)->threat.contains(p));
  const auto xp_before = zone.find(p)->xp;
  run_until(zone, sink, t, 20 * 120, [&] { const MobEntity* mm = zone.find_mob(m); return !mm || !mm->alive(); });
  const MobEntity* dead = zone.find_mob(m);
  CHECK((!dead || !dead->alive()));
  CHECK(zone.find(p)->xp > xp_before);  // the pet's kill is its owner's
  CHECK(zone.find(p)->hp == zone.find(p)->stats.max_hp);  // the owner was never touched

  // Back off and guard.
  zone.pet_command(p, PetOrder::Guard, 0, sink);
  CHECK(zone.find_mob(pet)->pet_mode == PetMode::Guard);
  zone.pet_command(p, PetOrder::BackOff, 0, sink);
  CHECK(zone.find_mob(pet)->pet_target == 0);

  // Dismiss: the pet is gone and the window clears.
  zone.pet_command(p, PetOrder::Dismiss, 0, sink);
  CHECK(zone.find(p)->pet == 0);
  run(zone, sink, t, 3);
  CHECK(sink.last<proto::PetStatus>(p)->pet_id == 0);
  CHECK(sink.despawned(p, pet));
}

TEST_CASE("a pet that dies leaves no corpse; an owner who dies or leaves loses the pet", "[classes][pets][zone]") {
  Zone zone(test_zone(), content(), kDt, 43);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("necromancer", 10, {"raise_bone_thrall", "creeping_blight"}));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, p, 0, 0));
  const EntityId pet = zone.find(p)->pet;
  REQUIRE(pet != 0);
  CHECK(zone.find_mob(pet)->def->body == "undead");
  // A brute that hits very hard kills the pet.
  MobDef brute = dummy(30, 5.0f);
  brute.damage_mult = 6.0f;
  const auto m = zone.spawn_mob(brute, {6, 0, 0});
  zone.tick(++t, sink);
  const auto corpses_before = zone.corpse_count();
  zone.pet_command(p, PetOrder::Attack, m, sink);
  run_until(zone, sink, t, 20 * 60, [&] { return zone.find(p)->pet == 0; });
  CHECK(zone.find(p)->pet == 0);
  CHECK(zone.corpse_count() == corpses_before);
  CHECK(sink.chat_count(p, "has been destroyed") == 1);
  CHECK_FALSE(zone.find_mob(m)->threat.contains(pet));

  // A new pet is released when its owner leaves.
  zone.remove_player(p, sink);
  const auto p2 = zone.add_player(caster("necromancer", 10, {"raise_bone_thrall"}, -100, -100, "Other"));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, p2, 0, 0));
  const EntityId pet2 = zone.find(p2)->pet;
  REQUIRE(pet2 != 0);
  zone.remove_player(p2, sink);
  run(zone, sink, t, 2);
  const MobEntity* gone = zone.find_mob(pet2);
  CHECK((!gone || !gone->alive()));
}

TEST_CASE("heals and buffs land on a pet; group buffs reach it too", "[classes][pets][zone]") {
  Zone zone(test_zone(), content(), kDt, 47);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("magician", 30, {"call_blaze_servant", "mend_servant", "elemental_fury"}));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, p, 0, 0));
  const EntityId pet = zone.find(p)->pet;
  REQUIRE(pet != 0);
  MobEntity* pm = zone.find_mob(pet);
  pm->hp = pm->stats.max_hp / 3;
  const int before = pm->hp;
  REQUIRE(cast(zone, sink, t, p, 1, 0));
  CHECK(zone.find_mob(pet)->hp > before);
  REQUIRE(cast(zone, sink, t, p, 2, 0));
  CHECK(has_buff(zone.find_mob(pet)->buffs, "elemental_fury"));
}

// ---------------------------------------------------------------- songs

TEST_CASE("a Bard song pulses while sung, songs twist, and silencing the bard stops the song", "[classes][songs][zone]") {
  Zone zone(test_zone(), content(), kDt, 53);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("bard", 10, {"hymn_of_mending", "anthem_of_swiftblades", "chant_of_embers"}));
  zone.tick(++t, sink);
  // Songs cost nothing and land at once.
  REQUIRE(cast(zone, sink, t, p, 0, 0));
  CHECK(zone.find(p)->song.active());
  CHECK(has_buff(zone.find(p)->buffs, "hymn_of_mending"));
  CHECK(sink.last<proto::SongUpdate>(p)->song_id == "hymn_of_mending");
  CHECK((zone.find(p)->stats.max_mana == 0));
  // It keeps pulsing: three pulses in 13 seconds, refreshing the effect.
  const int pulses0 = events(sink, p, CombatKind::SongPulse);
  run(zone, sink, t, 20 * 13);
  CHECK(events(sink, p, CombatKind::SongPulse) - pulses0 == 2);
  // Twisting: start the anthem, and the hymn's effect stays up until it runs out.
  REQUIRE(cast(zone, sink, t, p, 1, 0));
  CHECK(zone.find(p)->song.ability->id == "anthem_of_swiftblades");
  CHECK(has_buff(zone.find(p)->buffs, "hymn_of_mending"));
  CHECK(has_buff(zone.find(p)->buffs, "anthem_of_swiftblades"));
  run(zone, sink, t, 20 * 20);
  CHECK_FALSE(has_buff(zone.find(p)->buffs, "hymn_of_mending"));  // no longer sung: it faded
  CHECK(has_buff(zone.find(p)->buffs, "anthem_of_swiftblades"));
  // Moving does not stop a song.
  zone.find(p)->pos = {5, 0, 5};
  run(zone, sink, t, 5);
  CHECK(zone.find(p)->song.active());
  // A stun does.
  zone.find(p)->stunned_until = zone.now() + 40;
  run(zone, sink, t, 2);
  CHECK_FALSE(zone.find(p)->song.active());
  CHECK(sink.last<proto::SongUpdate>(p)->song_id.empty());
}

TEST_CASE("a detrimental song burns its target each pulse and stops when the target dies", "[classes][songs][zone]") {
  Zone zone(test_zone(), content(), kDt, 59);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("bard", 12, {"chant_of_embers", "hymn_of_mending"}));
  const auto m = zone.spawn_mob(dummy(10, 0.6f), {0, 0, -10});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  REQUIRE(cast(zone, sink, t, p, 0, m));
  CHECK(has_buff(zone.find_mob(m)->buffs, "chant_of_embers"));
  run_until(zone, sink, t, 20 * 120, [&] { const MobEntity* mm = zone.find_mob(m); return !mm || !mm->alive(); });
  CHECK(events(sink, p, CombatKind::DotTick) > 3);
  run(zone, sink, t, 20 * 7);
  CHECK_FALSE(zone.find(p)->song.active());
  // The same song gem again toggles a song off.
  REQUIRE(cast(zone, sink, t, p, 1, 0));
  CHECK(zone.find(p)->song.active());
  run(zone, sink, t, 40);
  zone.cast_spell(p, 1, 0, sink);
  CHECK_FALSE(zone.find(p)->song.active());
}

// ---------------------------------------------------------------- positionals

TEST_CASE("backstab needs the rogue behind its target", "[classes][positional][zone]") {
  Zone zone(test_zone(), content(), kDt, 61);
  RecordingSink sink;
  std::uint32_t t = 0;
  // The dummy faces -Z (yaw 0): a rogue at -Z is in front of it, at +Z behind it.
  const auto m = zone.spawn_mob(dummy(10), {0, 0, 0});
  const auto p = zone.add_player(as_class(at(0, -2.5f, "Sly", 15), "rogue"));
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  auto r = use(zone, sink, t, p, "backstab", m);
  CHECK_FALSE(r.ok);
  CHECK(r.message.find("behind") != std::string::npos);
  zone.find(p)->pos = {0, 0, 2.5f};
  zone.find_mob(m)->yaw = 0.0f;
  r = use(zone, sink, t, p, "backstab", m);
  CHECK(r.ok);
  CHECK(events(sink, p, CombatKind::AbilityHit) + events(sink, p, CombatKind::AbilityMiss) == 1);
}

// ---------------------------------------------------------------- charm

TEST_CASE("charm makes a mob a pet until it breaks, then it turns on the charmer", "[classes][charm][zone]") {
  Content c = content();
  c.rules.pets.charm_break_base = 1.0f;  // every check breaks it
  c.rules.pets.min_charm_break = 1.0f;
  c.rules.pets.max_charm_break = 1.0f;
  Zone zone(test_zone(), c, kDt, 67);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Charmer", 20), "enchanter"), {"coax_will", "mind_spike"}));
  MobDef d = dummy(14, 1.0f);
  d.damage_mult = 0.2f;
  const auto m = zone.spawn_mob(d, {0, 0, -8});
  zone.tick(++t, sink);
  zone.find_mob(m)->npc_damage = 500;  // guards had been fighting it before (Phase 12)
  zone.set_target(p, m, sink);
  bool charmed = false;
  for (int tries = 0; tries < 10 && !charmed; ++tries) {
    cast(zone, sink, t, p, 0, m);
    charmed = zone.find_mob(m)->owner == p;
  }
  REQUIRE(charmed);
  CHECK(zone.find(p)->pet == m);
  CHECK(zone.find_mob(m)->charmed);
  CHECK(zone.find_mob(m)->threat.empty());
  // That fight is forgotten, the guards' share included: it cannot claim a later kill.
  CHECK(zone.find_mob(m)->npc_damage == 0);
  CHECK_FALSE(zone.find_mob(m)->npc_kill);
  // Charmed mobs are pets: not valid enemies.
  zone.set_auto_attack(p, true, sink);
  CHECK_FALSE(zone.find(p)->auto_attack);
  // The first check breaks the charm, and the mob attacks its charmer.
  run_until(zone, sink, t, 20 * 10, [&] { return zone.find_mob(m)->owner == 0; });
  CHECK(zone.find_mob(m)->owner == 0);
  CHECK(zone.find(p)->pet == 0);
  CHECK(zone.find_mob(m)->threat.contains(p));
  CHECK(sink.chat_count(p, "has broken") == 1);
  CHECK(events(sink, p, CombatKind::CharmBreak) == 1);
}

TEST_CASE("charm respects level caps and charm-immune mobs", "[classes][charm][zone]") {
  Zone zone(test_zone(), content(), kDt, 71);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(with_spells(as_class(at(0, 0, "Charmer", 20), "enchanter"), {"coax_will"}));
  const auto big = zone.spawn_mob(dummy(19), {0, 0, -8});  // coax_will works up to level 16
  MobDef named = dummy(12);
  named.charm_immune = true;
  const auto boss = zone.spawn_mob(named, {5, 0, -8});
  zone.tick(++t, sink);
  for (EntityId target : {big, boss}) {
    zone.set_target(p, target, sink);
    for (int i = 0; i < 4; ++i) cast(zone, sink, t, p, 0, target);
    CHECK(zone.find_mob(target)->owner == 0);
  }
  CHECK(events(sink, p, CombatKind::Immune) >= 1);
}

// ---------------------------------------------------------------- aggro drops and stealth

TEST_CASE("feign death clears every hate list and the mob walks home without healing", "[classes][aggro][zone]") {
  Zone zone(test_zone(), content(), kDt, 73);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("necromancer", 20, {"deathly_stillness"}));
  MobDef d = dummy(18, 2.0f);
  const auto m = zone.spawn_mob(d, {0, 0, -3});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  zone.set_auto_attack(p, true, sink);
  run(zone, sink, t, 20 * 4);
  REQUIRE(zone.find_mob(m)->threat.contains(p));
  zone.find_mob(m)->hp = zone.find_mob(m)->stats.max_hp / 2;
  const int hurt = zone.find_mob(m)->hp;
  bool fooled = false;
  for (int i = 0; i < 6 && !fooled; ++i) {
    cast(zone, sink, t, p, 0, 0);
    fooled = zone.find(p)->feigned;
  }
  REQUIRE(fooled);
  CHECK_FALSE(zone.find(p)->auto_attack);
  CHECK_FALSE(zone.find_mob(m)->threat.contains(p));
  run_until(zone, sink, t, 20 * 30, [&] { return zone.find_mob(m)->state == AiState::Idle; });
  CHECK(zone.find_mob(m)->state == AiState::Idle);
  CHECK(zone.find_mob(m)->hp <= hurt + zone.find_mob(m)->stats.max_hp / 20);  // it did not reset to full
  // A feigning player is invisible to proximity aggro; moving stands them up.
  MobDef watcher = dummy(18, 1.0f, 20.0f);
  const auto w = zone.spawn_mob(watcher, {0, 0, 6});
  run(zone, sink, t, 20 * 2);
  CHECK_FALSE(zone.find_mob(w)->threat.contains(p));
  zone.queue_input(p, 1, MoveIntent{1.0f, 0.0f, 0.0f}, zone.find(p)->pos);
  run(zone, sink, t, 2);
  CHECK_FALSE(zone.find(p)->feigned);
}

TEST_CASE("evade drops most of a rogue's threat; a warrior's taunt and hate discipline hold aggro", "[classes][aggro][zone]") {
  Zone zone(test_zone(), content(), kDt, 79);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto r = zone.add_player(as_class(at(0, 2, "Sly", 20), "rogue"));
  const auto m = zone.spawn_mob(dummy(18, 3.0f), {0, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(r, m, sink);
  zone.set_auto_attack(r, true, sink);
  run(zone, sink, t, 20 * 6);
  const double before = zone.find_mob(m)->threat.threat_of(r);
  REQUIRE(before > 0.0);
  REQUIRE(use(zone, sink, t, r, "evade", m).ok);
  CHECK(zone.find_mob(m)->threat.threat_of(r) <= before * 0.4 + 30.0);
  CHECK(events(sink, r, CombatKind::ThreatDrop) == 1);
}

TEST_CASE("a warrior holds aggro against a wizard nuking the same mob", "[classes][aggro][threat][zone]") {
  Zone zone(test_zone(), content(), kDt, 83);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto war = zone.add_player(at(0, 2, "Tank", 40));
  const auto wiz = zone.add_player(caster("wizard", 40, {"inferno_spike", "pyre_lance"}, 0, 25, "Nuker"));
  MobDef d = dummy(40, 6.0f);
  d.damage_mult = 0.3f;
  const auto m = zone.spawn_mob(d, {0, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(war, m, sink);
  zone.set_target(wiz, m, sink);
  zone.set_auto_attack(war, true, sink);
  zone.use_ability(war, "iron_challenge", m, sink);
  run(zone, sink, t, 20 * 3);
  int ticks = 0;
  int tank_on_top = 0;
  for (int i = 0; i < 20 * 60 && zone.find_mob(m) && zone.find_mob(m)->alive(); ++i) {
    PlayerEntity& w = *zone.find(war);
    if (zone.now() >= w.gcd_until) {
      // The tank's priority: taunt back if it lost the top, else its threat strikes.
      const char* pick = zone.find_mob(m)->threat.top() != war ? "iron_challenge" : "goading_strike";
      bool ready = true;
      for (const auto& [id, at] : w.cooldowns)
        if (id == pick && zone.now() < at) ready = false;
      if (!ready) pick = "cleaving_blow";
      zone.use_ability(war, pick, m, sink);
      zone.use_ability(war, "iron_hate", m, sink);
    }
    PlayerEntity& z2 = *zone.find(wiz);
    z2.mana = z2.stats.max_mana;  // the nuker never runs dry: the worst case for the tank
    if (!z2.cast.casting() && zone.now() >= z2.gcd_until && z2.mana > 100) zone.cast_spell(wiz, 0, m, sink);
    zone.tick(++t, sink);
    if (const MobEntity* mm = zone.find_mob(m); mm && mm->alive()) {
      ++ticks;
      if (mm->threat.top() == war) ++tank_on_top;
    }
  }
  REQUIRE(ticks > 20 * 10);
  // The wizard really was nuking: the tank held the mob against landed spells, not an idle caster.
  int nukes = 0;
  for (const auto& e : sink.all<proto::CombatEvent>(wiz))
    if (e.source_id == wiz && e.target_id == m && e.kind == static_cast<std::uint8_t>(CombatKind::SpellHit)) ++nukes;
  REQUIRE(nukes >= 5);
  INFO("tank on top " << tank_on_top << " of " << ticks << " ticks");
  CHECK(tank_on_top >= ticks * 4 / 5);
}

TEST_CASE("hidden and sneaking players are noticed less; acting ends hiding", "[classes][stealth][zone]") {
  Zone zone(test_zone(), content(), kDt, 89);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(as_class(at(0, 0, "Sly", 30), "rogue"));
  zone.find(p)->skills.push_back({"hide", 150});
  for (auto& s : zone.find(p)->skills)
    if (s.id == "hide" || s.id == "sneak") s.value = 150;
  zone.tick(++t, sink);
  bool hidden = false;
  for (int i = 0; i < 10 && !hidden; ++i) {
    use(zone, sink, t, p, "melt_away", 0);
    hidden = zone.find(p)->hidden;
  }
  REQUIRE(hidden);
  // An aggressive mob right next to a hidden player does not notice it.
  const auto m = zone.spawn_mob(dummy(28, 1.0f, 20.0f), {0, 0, -5});
  run(zone, sink, t, 20 * 3);
  CHECK_FALSE(zone.find_mob(m)->threat.contains(p));
  // Sneaking keeps hiding while moving; walking without sneaking ends it.
  REQUIRE(use(zone, sink, t, p, "shadow_stride", 0).ok);
  CHECK(zone.find(p)->sneaking);
  CHECK(zone.find(p)->hidden);
  zone.queue_input(p, 1, MoveIntent{1.0f, 0.0f, 0.0f}, zone.find(p)->pos);
  run(zone, sink, t, 3);
  CHECK(zone.find(p)->hidden);
  // Sneaking alone: a sneaking (not hidden) player outside half the aggro radius goes unnoticed.
  zone.find(p)->hidden = false;
  zone.find(p)->pos = {0, 0, 8};  // 13 m from the mob: inside 20 m, outside 10 m
  run(zone, sink, t, 20 * 2);
  CHECK_FALSE(zone.find_mob(m)->threat.contains(p));
  // Attacking ends sneaking.
  zone.find(p)->pos = {0, 0, -3};
  zone.set_target(p, m, sink);
  use(zone, sink, t, p, "quick_jab", m);
  CHECK_FALSE(zone.find(p)->sneaking);
  // And hide fails while a mob is after you.
  run(zone, sink, t, 20);
  auto res = use(zone, sink, t, p, "melt_away", 0);
  CHECK(res.ok);
  CHECK_FALSE(zone.find(p)->hidden);
  CHECK(sink.chat_count(p, "enemies are watching") >= 1);
}

// ---------------------------------------------------------------- damage shields, lifetaps, drains

TEST_CASE("damage shields burn attackers; lifetaps and drains heal their caster", "[classes][spells][zone]") {
  Zone zone(test_zone(), content(), kDt, 97);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto mage = zone.add_player(caster("magician", 20, {"flame_mantle"}));
  MobDef d = dummy(18, 3.0f);
  d.damage_mult = 0.5f;
  const auto m = zone.spawn_mob(d, {0, 0, -2});
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, mage, 0, mage));
  REQUIRE(has_buff(zone.find(mage)->buffs, "flame_mantle"));
  zone.set_target(mage, m, sink);
  zone.set_auto_attack(mage, true, sink);
  run(zone, sink, t, 20 * 20);
  CHECK(events(sink, mage, CombatKind::DamageShield) >= 1);

  const auto sk = zone.add_player(caster("shadow_knight", 25, {"vampiric_touch"}, 50, 0, "Knight"));
  const auto n = zone.add_player(caster("necromancer", 30, {"life_drinker"}, 60, 0, "Necro"));
  const auto m2 = zone.spawn_mob(dummy(20, 20.0f), {55, 0, -5});
  zone.tick(++t, sink);
  zone.find(sk)->hp = zone.find(sk)->stats.max_hp / 2;
  zone.find(n)->hp = zone.find(n)->stats.max_hp / 2;
  const int sk_hp = zone.find(sk)->hp;
  const int n_hp = zone.find(n)->hp;
  zone.set_target(sk, m2, sink);
  zone.set_target(n, m2, sink);
  for (int i = 0; i < 4; ++i) cast(zone, sink, t, sk, 0, m2);
  CHECK(events(sink, sk, CombatKind::Lifetap) >= 1);
  CHECK(zone.find(sk)->hp > sk_hp);
  for (int i = 0; i < 3 && !has_buff(zone.find_mob(m2)->buffs, "life_drinker"); ++i) cast(zone, sink, t, n, 0, m2);
  REQUIRE(has_buff(zone.find_mob(m2)->buffs, "life_drinker"));
  run(zone, sink, t, 20 * 10);
  CHECK(events(sink, n, CombatKind::DrainTick) >= 2);
  CHECK(zone.find(n)->hp > n_hp);
}

// ---------------------------------------------------------------- summons and illusions

TEST_CASE("summoned items appear in the pack, fade on time, and dissolve on death", "[classes][items][zone]") {
  Zone zone(test_zone(), content(), kDt, 101);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("magician", 20, {"conjure_bread", "conjure_blade"}));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, p, 0, 0));
  const SlotCode bread_slot = slot_of(*zone.find(p), "conjured_bread");
  REQUIRE(bread_slot != kNoSlot);
  const ItemStackState* bread = zone.find(p)->inv.at(bread_slot);
  CHECK(bread->count == 4);
  CHECK(bread->expires_tick > zone.now());
  CHECK(zone.item_seconds_left(*bread) == Approx(3600.0f).margin(10.0f));
  auto items = sink.last<proto::Inventory>(p)->items;
  CHECK(std::any_of(items.begin(), items.end(), [](const proto::ItemStack& s) { return s.item_id == "conjured_bread" && s.expires_ms > 3000000; }));
  // Time passes: the bread fades.
  zone.find(p)->inv.at(bread_slot)->expires_tick = zone.now() + 5;
  run(zone, sink, t, 40);
  CHECK(count_of(*zone.find(p), "conjured_bread") == 0);
  CHECK(sink.chat_count(p, "fades away") == 1);
  // A summoned blade dissolves when its owner dies instead of going into the corpse.
  REQUIRE(cast(zone, sink, t, p, 1, 0));
  *zone.find(p)->inv.at(general_code(7)) = {"rat_whisker", 2, 0};
  MobDef killer = dummy(40, 5.0f, 10.0f);
  killer.damage_mult = 20.0f;
  zone.spawn_mob(killer, {0, 0, -2});
  run_until(zone, sink, t, 20 * 60, [&] { return zone.find(p)->dead; });
  REQUIRE(zone.find(p)->dead);
  bool corpse_has_blade = false;
  bool corpse_has_whisker = false;
  zone.for_each_corpse([&](const CorpseEntity& c) {
    for (const auto& s : c.items) {
      corpse_has_blade |= s.stack.item_id == "conjured_blade";
      corpse_has_whisker |= s.stack.item_id == "rat_whisker";
    }
  });
  CHECK_FALSE(corpse_has_blade);
  CHECK(corpse_has_whisker);
  CHECK(sink.chat_count(p, "summoned items dissolve") == 1);
}

TEST_CASE("summoned items keep their time across a save", "[classes][items][zone]") {
  Zone zone(test_zone(), content(), kDt, 103);
  RecordingSink sink;
  ZonePlayerInit i = at(0, 0, "Keeper", 10);
  i.items.push_back({general_code(2), ItemStackState{"conjured_water", 3, 0}});
  i.item_seconds.push_back({general_code(2), 120.0f});
  i.items.push_back({general_code(3), ItemStackState{"rat_whisker", 1, 0}});
  const auto p = zone.add_player(i);
  const auto& inv = zone.find(p)->inv;
  CHECK(zone.item_seconds_left(*inv.at(general_code(2))) == Approx(120.0f).margin(0.1f));
  CHECK(zone.item_seconds_left(*inv.at(general_code(3))) == 0.0f);
}

TEST_CASE("an illusion changes how others see the player, then wears off", "[classes][illusion][zone]") {
  Zone zone(test_zone(), content(), kDt, 107);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto e = zone.add_player(caster("enchanter", 32, {"mask_of_stone", "guise_of_the_wolf"}));
  const auto watcher = zone.add_player(at(5, 0, "Watcher"));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, e, 0, 0));
  auto latest = [&] {
    proto::EntitySpawn last;
    for (const auto& s : sink.all<proto::EntitySpawn>(watcher))
      if (s.entity_id == e) last = s;
    return last;
  };
  CHECK(latest().race == "deepkin");
  CHECK(latest().model.empty());
  REQUIRE(cast(zone, sink, t, e, 1, 0));  // a model illusion replaces the race one (same line)
  CHECK(latest().model == "pf_wolf");
  CHECK(latest().race == "highlander");
  // Make it wear off now.
  for (auto& b : zone.find(e)->buffs)
    if (!b.illusion.empty()) b.illusion_expires = b.expires_tick = zone.now() + 2;
  run(zone, sink, t, 5);
  CHECK(latest().model.empty());
  CHECK(latest().race == "highlander");
}

// ---------------------------------------------------------------- utility skills

TEST_CASE("tracking lists nearby creatures; pick lock works on a lockbox", "[classes][utility][zone]") {
  ZoneDef zd = test_zone();
  zd.id = "proving_flats";  // the lockbox lives there
  Zone zone(zd, content(), kDt, 109);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto r = zone.add_player(as_class(at(0, 0, "Scout", 20), "ranger"));
  zone.spawn_mob(dummy(5), {0, 0, -50});
  zone.tick(++t, sink);
  REQUIRE(use(zone, sink, t, r, "trail_sense", 0).ok);
  CHECK(sink.chat_count(r, "You track") == 1);
  CHECK(sink.chat_count(r, "m to the north") >= 1);

  const auto rog = zone.add_player(as_class(at(3, -8, "Picker", 30), "rogue"));
  zone.tick(++t, sink);
  EntityId box = 0;
  zone.for_each_npc([&](const NpcEntity& n) {
    if (n.def->role == "lockbox") box = n.id;
  });
  REQUIRE(box != 0);
  zone.set_target(rog, box, sink);
  int tries = 0;
  while (tries < 20 && sink.chat_count(rog, "You pick the lock") == 0) {
    use(zone, sink, t, rog, "pick_lock", box);
    ++tries;
  }
  CHECK(sink.chat_count(rog, "You pick the lock") == 1);
  // A creature is not a lock.
  const auto m = zone.spawn_mob(dummy(5), {3, 0, -6});
  zone.tick(++t, sink);
  CHECK_FALSE(use(zone, sink, t, rog, "pick_lock", m).ok);
}

TEST_CASE("monks dual wield their fists", "[classes][skills][zone]") {
  Zone zone(test_zone(), content(), kDt, 113);
  RecordingSink sink;
  std::uint32_t t = 0;
  ZonePlayerInit i = as_class(at(0, 2, "Fists", 30), "monk");
  i.skills = {{"dual_wield", 150}, {"offense", 150}, {"double_attack", 1}};
  const auto p = zone.add_player(i);
  const auto m = zone.spawn_mob(dummy(28, 30.0f), {0, 0, 0});
  zone.tick(++t, sink);
  zone.set_target(p, m, sink);
  zone.set_auto_attack(p, true, sink);
  run(zone, sink, t, 20 * 30);
  int swings = 0;
  for (const auto& e : sink.all<proto::CombatEvent>(p))
    if (e.source_id == p && e.target_id == m && e.ability.empty() && e.kind <= static_cast<std::uint8_t>(CombatKind::Crit)) ++swings;
  const double rounds = 30.0 / static_cast<double>(zone.find(p)->stats.weapon_delay);
  CHECK(swings > rounds * 1.3);  // a second swing on most rounds
}

// ---------------------------------------------------------------- every class and race

TEST_CASE("every class and race enters the world with its kit, and every class has abilities to 60", "[classes][content]") {
  const Content& c = content();
  REQUIRE(c.races.size() >= 6);
  REQUIRE(c.races.size() <= 8);
  REQUIRE(c.classes.size() == 14);
  Zone zone(test_zone(), c, kDt, 127);
  RecordingSink sink;
  std::set<std::string> races_seen;
  float x = -400;
  for (const auto& r : c.races) {
    for (const auto& cls : r.allowed_classes) {
      ZonePlayerInit i = as_class(at(x, -400, "Hero", 1), cls.c_str());
      i.race = r.id;
      x += 4;
      const auto p = zone.add_player(i);
      zone.send_initial_state(p, sink);
      const PlayerEntity* pl = zone.find(p);
      REQUIRE(pl);
      CHECK(pl->race == r.id);
      CHECK(pl->class_id == cls);
      races_seen.insert(r.id);
      // Spell users start with spells memorized; everyone has something to press at level 1.
      const auto list = sink.last<proto::AbilityList>(p);
      const auto book = sink.last<proto::SpellBook>(p);
      REQUIRE((list && book));
      const bool has_level1 = std::any_of(list->abilities.begin(), list->abilities.end(), [](const auto& a) { return a.level == 1; }) ||
                              std::any_of(book->spells.begin(), book->spells.end(), [](const auto& s) { return s.level == 1; });
      CHECK(has_level1);
      zone.remove_player(p, sink);
    }
  }
  CHECK(races_seen.size() == c.races.size());
  for (const auto& cl : c.classes) {
    std::set<int> levels;
    for (const AbilityDef* a : c.abilities_for(cl.id)) levels.insert(a->level);
    for (const AbilityDef* a : c.spells_for(cl.id)) levels.insert(a->level);
    INFO(cl.id);
    CHECK(c.abilities_for(cl.id).size() + c.spells_for(cl.id).size() >= 25);
    CHECK((!levels.empty() && *levels.rbegin() >= 57));
    // Every class is playable by some race.
    CHECK(std::any_of(c.races.begin(), c.races.end(), [&](const RaceDef& r) {
      return std::find(r.allowed_classes.begin(), r.allowed_classes.end(), cl.id) != r.allowed_classes.end();
    }));
  }
}

TEST_CASE("pets drop mobs that leash home, and a guarding pet comes back when its owner leaves", "[classes][pets][zone]") {
  Zone zone(test_zone(), content(), kDt, 131);
  RecordingSink sink;
  std::uint32_t t = 0;
  const auto p = zone.add_player(caster("magician", 22, {"call_blaze_servant"}));
  zone.tick(++t, sink);
  REQUIRE(cast(zone, sink, t, p, 0, 0));
  const EntityId pet = zone.find(p)->pet;
  REQUIRE(pet != 0);
  // A mob that hit the pet, then gave up and walks home, is no longer the pet's fight.
  const auto m = zone.spawn_mob(dummy(18, 3.0f), {6, 0, 0});
  zone.tick(++t, sink);
  zone.find_mob(pet)->threat.add(m, 50.0);
  zone.find_mob(m)->state = AiState::Leash;
  run(zone, sink, t, 3);
  CHECK(zone.find_mob(pet)->pet_target == 0);
  CHECK_FALSE(zone.find_mob(pet)->threat.contains(m));
  // Guarding: it holds its spot while the owner is near...
  zone.pet_command(p, PetOrder::Guard, 0, sink);
  const Vec3 spot = zone.find_mob(pet)->pos;
  zone.find(p)->pos = {spot.x + 30, 0, spot.z};
  run(zone, sink, t, 20 * 3);
  CHECK(horizontal_distance(zone.find_mob(pet)->pos, spot) < 1.0f);
  // ...and comes back when the owner goes beyond the leash distance.
  zone.find(p)->pos = {spot.x + 200, 0, spot.z};
  run(zone, sink, t, 20 * 3);
  CHECK(zone.find_mob(pet)->pet_mode == PetMode::Follow);
  CHECK(horizontal_distance(zone.find_mob(pet)->pos, spot) > 10.0f);
}
