# Decisions Log

Record significant decisions here, newest first.

Format: `## YYYY-MM-DD — Title` followed by **Decision**, **Rationale**, **Alternatives considered**.

## 2026-11-22 — The world map and how outdoor zones are generated (Phase 6b)

**Decision:**
- **Map:** three hubs, each with a starter zone outside its gate (1-8). The three starter zones meet in the **Saltmarsh Fens** (8-15), which leads north to **Duskmere Shore** (15-22). From there the world branches west (Hollowpine, Gloomfen) and east (Redrock), rejoining at the **Shattered Steppe** (30-38). The Steppe opens to Frostveil (35-45) and the Cinderspire Wastes (38-45), which lead to the Ashen Reach (45-52) and the Stormcrown Peaks (50-60). Ferries cross the Duskmere between the two river hubs and the Duskmere landing.
- **Links are data** (`LINKS` in `tools/gen_world.py`): a zone line on the named side 6 m from the edge, an arrival entry 40 m in, named `from_<zone>`. A hub end names its scene's gate entry; the scene's own line names the outdoor entry.
- **Camps** are placed at random (seeded) away from entries, lines, features and each other. Their level is interpolated across the zone's band by distance from the zone's first (lowest) entrance, and the mob is chosen from the content by level, preferring mobs tagged with the zone's biome. Content changes in 6d therefore re-populate the zones without editing them.
- **Hub services are clones** of the test zone's trainers and vendors (`HUB_ROSTER`): the same stock and spell lists, a new id (`<hub>_<template>`), a new name, `"origin": "world"`. They stand where the hub scene's NPC markers put them. Each hub teaches the spell-using classes its races can be. Higher-band vendors stand in outposts in their band's zones.
- **Druid rings and Wizard spires** are travel points in outdoor zones (one per band, roughly), reached by new portal spells, so the classic "port to the wilds" role exists without new mechanics.

**Rationale:** Generating outdoor zones from a few lines of spec (biome, band, links, features) keeps 13 zones maintainable and consistent, while hubs stay hand-authored scenes. Cloning services keeps one definition of what a trainer sells.

**Alternatives considered:**
- Hand-placing every camp. Too slow for 13 zones and brittle when 6d replaces the mobs.
- Moving the test zone's NPCs into the hubs. The test zone's scripted runs (bots, client e2e) depend on them where they are.

## 2026-11-21 — Zone pipeline: scenes are the source, the server collides with their export (Phase 6a)

**Decision:**
- **Zone files**, one per zone in `content/zones/<id>.json` (kinds hub, outdoor, dungeon, test). `zones.json` is gone.
- **Hand-authored hubs are Godot scenes** (`client/zones/<id>.tscn`) built from prop scenes. `client/tools/export_zones.gd` (headless Godot) exports their `StaticBody3D` shapes, zone-line `Area3D`s and `Marker3D`s (entries, NPC placements, spawns) to `content/zones/scenes/<id>.json`, which the server merges after the zone file's own lists. CI checks the export is fresh. The client draws the scene; collision always comes from the export.
- **Outdoor zones are generated** (`tools/gen_world.py`): hills, props scattered from the prop table (`tools/world_props.py`, which also writes the prop scenes, so a prop's collision is defined once), camps, entries and lines. The zone file lists `props` for the client and `obstacles`/`walls` for the server.
- **Collision is 2D** (circles, rotated boxes, wall tiles) with a deterministic push-out shared line for line by server and client; terrain is base + round hills. Prediction uses the same rule, so collisions cause no corrections.
- **Navigation** is a grid rasterized from the collision at load (the "navmesh"), with bounded A* and string pulling; mobs walk straight when they can.
- **A layout hash** of each zone's geometry travels in `EnterWorldResult`; the client warns on a mismatch (stale client data).
- **Dungeons** are seeded procedural layouts: integer-only xorshift32 generation in C++ and GDScript, pinned by a golden hash in both test suites. Rooms replace coordinates for spawns, entries and lines.

**Rationale:** The requirement is that server collision comes from the same source as the client scenes. Making the scene the source (and the server data an export of it) lets designers move a house in the Godot editor and re-export; mirroring a small 2D rule exactly is far simpler and more predictable than running physics on both sides. Generated outdoor zones and seeded dungeons share the same collision format, so one movement rule covers everything.

**Alternatives considered:**
- JSON as the only source with the client building scenes from it. Simpler, but hubs would not be editable as scenes.
- Godot physics on the client and a separate server collider. Two collision models drift apart; every difference is a correction.
- A real navmesh (Recast). Heavy for flat 2D content; a grid from the same shapes is enough for mobs.
- A Python copy of the dungeon generator for the validator. Not needed: the validator only needs a dungeon's size, which is a closed formula.

## 2026-11-21 — Zone instances, transfers and travel (Phase 6a)

**Decision:**
- **One process, many zones:** zones load on demand (hubs and the test zone at start; a zone holding a restored player corpse too) and unload after `zone_idle_seconds` empty (no players, no player corpses). Mob state is lost on unload; spawns refill on the next load.
- **Transfers** are queued by zones (zone lines, travel NPCs, teleport effects into another zone, a respawn at a bind point elsewhere) and carried out by the game server after every zone has ticked: snapshot the character (buffs, timers, items), remove it, add it at the destination, send a new `EnterWorldResult` with `zone_change`, then save (the "autosave on zone change" rule) together with all pending changes. Players are ignored by zone lines for 1 s after arriving. A summoned pet is summoned again beside its owner with the same health fraction; charmed pets do not follow. Debuffs on the player do not carry over (only beneficial buffs are saved).
- **Zone RNG seeds** come from the zone id (not its position in the content list), so adding a zone does not change another zone's pinned stream.
- **Travel:** soulbinders bind; waystone keepers teleport to a city or travel point for a fee; ferries are modelled as a boarding wait (pay and cross after `delay` seconds if still on the dock) rather than a moving boat. Portal spells reach cities and `content/travel.json` points in any zone.
- **Test zone:** The Proving Flats stays as `kind: test`, outside the reachability check. `EMBERFALL_START_ZONE` (config `start_zone`) starts new characters there (at an entry named after their city when one exists); the wire tests, bots and the Phase 1-5 client runs use it, so their camps, trainers and vendors stay where they were.
- **World NPC files:** three generators write parts of `content/npcs.json` (trainers, vendors, world NPCs); each keeps the others' rows, and all share one ordering and renderer (`tools/npc_order.py`) so their `--check`s agree.

**Rationale:** Moving a character between zones through the same snapshot path as saving keeps one definition of "the character", and doing it after the zones' tick avoids changing a zone's player list mid-iteration.

**Alternatives considered:**
- A process per zone. CLAUDE.md asks for a single process; cross-zone features (tells, groups, auctions) are simpler in one.
- Moving boats that players stand on. Deferred: it needs moving platforms in both the collision rule and prediction.
- Cross-zone resurrection (a rez reaching an owner standing in another zone). Deferred: the owner must still be in the corpse's zone (dead, or back at it).

## 2026-11-21 — Item model, slot codes and inventory layout (Phase 5, protocol v7, schema v6)

**Decision:**
- **One inventory per character** (`game/items.hpp: Inventory`): 21 worn slots, an 8-slot pack, a 16-slot bank (`rules.economy.bank_slots`), and the contents of every bag in a pack or bank slot (up to 16 each). Bag contents belong to the slot holding the bag and move with it. Bags never go inside bags.
- **Slot codes** are one `u16` on the wire and in the database: `(container << 8) | index`. Containers: `0` worn, `1` pack, `2` bank, `0x10 + g` the bag in pack slot `g`, `0x20 + b` the bag in bank slot `b`.
- **Item schema** (`content/items.json`): kind, slots (`wrist`/`finger`/`ear` fit either of a pair), armor weight, weapon type, damage/delay/range, AC, HP/mana/endurance, the seven stats, resists, haste, a `proc` (effect + rate), a `click` (effect + cast time + recast), rarity, `magic`/`lore`/`no_trade`, class/race lists, `req_level`, `bag_slots`, `stack`, `value`, and `source` for named placeholders.
- **Item effects** live in a separate `content/item_effects.json`. They are ordinary `AbilityDef`s (class `item`) resolved by the spell engine like spells (resists, no weapon roll). Procs fire on a landed melee hit with chance `rate x delay / 3 s`; clickies and consumables can have a cast time (bandages) and a recast.
- **Restrictions** (`can_wear`): slot, class list, race list, `req_level`, armor weight (a class wears its own and anything lighter), weapon proficiency (Phase 4 tables), and a weapon in the off hand needs dual wield. A two-hander needs the off hand empty. Lore items: one per character.
- **Stats:** base (race, class, level) + worn gear + buffs, recomputed on every change (`refresh_stats`). Item STA does not change the HP formula; items carry explicit HP. **Haste** from items and spells does not stack: the larger counts, slows apply on top.
- **Weapons:** an equipped weapon hits for `damage x class weapon_mult` (warrior 1.0 ... casters 0.5, from the old placeholder per-level ratios). Bare fists hit for the old placeholder formula `x unarmed_mult` (0.4; Monks 1.0, so Monk fists stay a real weapon). The off hand swings only with a weapon there (or two bare fists), for `offhand_damage_mult` of its own damage. Archery abilities need a bow in the ranged slot (`requires_weapon`) and add half its damage (`rules.combat.ranged_weapon_share`); arrows add a little and are not used up.
- **Migration v6:** old pack slots 0-7 become pack slots, 8-15 the first bank slots; `bank_coin`; the `auctions` table; old corpse items lose their (index) slot. Items whose saved slot no longer exists land in the pack, then the bank, never silently lost.

**Rationale:** One addressable layout lets every move, trade, sale and loot use the same validated code paths. Keeping weapon damage tied to the old placeholder curve (via `weapon_mult`) kept Phase 4's class balance intact when real weapons replaced it.

**Alternatives considered:**
- A cursor item (pick up, then place). Rejected: two-step moves are harder to make atomic; drag-and-drop sends one move.
- Per-instance item data (charges, durability). Deferred: no Phase 5 feature needs it.
- Consuming arrows per shot. Deferred: it adds bookkeeping with little play value before tradeskills.

## 2026-11-21 — Anti-dupe: every cross-character change is one transaction (Phase 5)

**Decision:**
- In-memory zone state is authoritative. A move inside one character only marks it dirty; the game server saves every dirty character once a second, in one transaction (any snapshot of one character is self-consistent).
- **Trades:** while a trade window is open, both players' items are locked (no moves, sales, uses, loot, scribing). Offers record what each trade slot held; completion re-checks every offer, room and lore on copies of both inventories, then swaps in one step (`execute_trade`). The game server saves both characters in one transaction. Cancel, distance, death, disconnect or zone exit close the window with nothing exchanged.
- **Corpse loot** saves the looter with the corpse (unchanged); a won need/greed item that goes straight into a pack saves that player at once.
- **Auctions:** listing (item out, fee paid), buying (coin out, item in, listing sold), collecting (proceeds or the unsold item) and cancelling are each one `apply_auction` transaction holding the listing change and the character. Deleting a listing that is not in the table fails the whole transaction.
- The DB worker is FIFO, so later snapshots always land after earlier ones.
- **One save path** for everything in the world: `flush_item_saves` writes every dirty character (all of them on autosave and shutdown), every listing changed since (as it is now, or a delete), in one transaction. Leaving the world, trades, auctions and other saves mark characters dirty and flush. Corpse writes (death, corpse loot) first flush anything pending.
- **Failures:** a batch is tried three times on the DB worker. If it still fails, it becomes the worker's *carry*, which is merged (newest copy per character and listing wins) into the next transaction, so a failed trade or listing is never stored apart from its counterpart, even if one side logs out meanwhile; the game server flushes again each second until it goes through. A character with an unsaved carry cannot be loaded ("still being saved"); a deleted character is dropped from the carry. A corpse write replaces its owner's carried copy (it is newer). An auction board that cannot be read completely is not opened; the read is retried every 10 s.
- A failure persisting across a crash still loses the unsaved changes, but on both sides of a transfer alike: no duplicates.

**Rationale:** Duplication bugs come from two records changing at different times. Making every multi-character change a single transaction, and freezing items during a trade, leaves no window in which an item exists twice. A fuzz test drives random offers, accepts, cancels, moves and disconnects and checks that items and coin are conserved after every step; a wire test checks what both players log back in with.

**Alternatives considered:**
- Saving after every move. Rejected: a drag spree would issue dozens of transactions; the dirty flush gives the same safety for single-character moves.
- An escrow table for trades. Rejected: unnecessary when both sides commit together.

## 2026-11-21 — Group loot before groups exist (Phase 5)

**Decision:** Groups arrive in Phase 7, so the **loot party** of a kill is every player who damaged the mob and is still in the zone (top damager first, at most `loot_party_max` = 6). With one member, loot works as before. With more:
- Coin is split at once (remainder one copper at a time to the first members).
- The top damager's rule (`/lootrule ng|rr`, default need before greed) decides items. **Need/greed:** wearable or uncommon+ items are rolled (30 s); need beats greed, ties go to the earlier entry, "need" on an item you cannot use counts as greed; the winner gets it straight into the pack when there is room, otherwise it is reserved for them on the corpse; all-pass frees it for the party. **Round robin:** each item is reserved for one member in turn; after `round_robin_free_seconds` (2 min) anyone in the party may take it.
- Phase 7 replaces "who damaged it" with the group roster; the rules and windows stay.

**Rationale:** The roll and round-robin machinery is testable and usable now (two players fighting the same mob share it), and Phase 7 only changes who is in the party.

## 2026-11-21 — Itemization formulas (Phase 5, tools/gen_items.py)

**Decision:** `tools/gen_items.py` generates 708 items besides spell scrolls (304 armor, 223 weapons, 25 shields, 88 jewelry, 6 ammo, 31 consumables, 7 bags, 24 trade loot), 63 item effects, loot pools for every mob, and 9 vendors/bankers/auctioneers.
- **Armor:** cloth/leather/chain/plate x 9 slots x 8 tiers (levels 1, 8, 15, 22, 30, 38, 46, 54). AC = weight (0.4/0.6/0.8/1.0) x slot share x (1.5 x level + 5). Tiers 1-3 common (no stats), 4-6 uncommon, 7-8 rare (stats from a budget `(2 + 0.35 x level) x rarity x share x 5`, split by weight: plate STA/STR/HP, cloth INT/WIS/mana, ...). Some rare hands/wrists carry haste.
- **Weapons:** 16 kinds x 12 tiers (every 5 levels). Damage = `(4 + 1.15 x (level + 2.5)) / 3 s x delay x kind multiplier x rarity`; two-handers x1.3-1.48, daggers and fists x0.95, bows x0.9. Rare weapons and every other uncommon one proc an elemental burst of their tier.
- **Jewelry and cloaks:** 5 slots x 2 flavors (mystic: INT/WIS/mana, martial: STR/STA/HP) x 8 tiers; resists from tier 4.
- **Named dungeon placeholders:** 6 lore items for each of 8 Phase 6 dungeons (rare; epic and no-trade for the two raid dungeons), `source: "dungeon:<id>"`, bound to bosses in Phase 6.
- **Value** grows with level squared; vendors charge `value x (1.3 - 0.002 x (CHA - 75))` (1.05..1.6) and pay `value x (0.25 + 0.001 x (CHA - 75))` (0.1..0.4). The validator requires `max_sell < min_buy`, so no buy-sell loop exists at any charisma.
- **Loot:** each mob's table keeps its trade drops and gains a gear pool (items of levels `mob - 8 .. mob`, weighted common 10 / uncommon 5 / rare 1.5, chance 18-35%), a consumables pool and a scroll pool (5%). The boss rolls its pool twice.
- **Vendors** by band: novice outfitter (levels 1-15), journeyman smith (15-30), veteran armorer (30-45, limited stock), master armsdealer (45-60, limited), provisioner, alchemist, jeweler. Limited lines restock one every 10 minutes; anything players sell is resold as buyback (no-trade items excepted).
- **Starting kits** (`classes.json starting_items`): class training weapons (now real items), a cloth pouch, 5 rations, 5 water; rangers also get a training bow and arrows.

**Rationale:** Formulas instead of hand entry keep 700 items consistent and retunable from one place; the weapon curve matches the old placeholder so balance carried over.

## 2026-11-21 — Balance with gear (Phase 5)

**Decision:** The balance simulator now equips every class with `recommended_kit` (the best common/uncommon armor, jewelry and weapons its class may use at that level; one-hander + shield, two-hander, or two one-handers for dual wielders; a bow for archers). `--gearless` runs the Phase 4 bare-handed comparison. With gear, all 14 classes win every fight at all nine levels and none is beyond 3x the median time to kill (no outliers). Melee classes got relatively stronger than casters (armor and stats help them most); the slowest kills are rogue (~19 s at 60), cleric (~19.5 s) and enchanter (~18.6 s), the fastest magician (~9.5 s). Tuning toward an even spread is left for Phase 8's full 1-60 pass.

## 2026-11-21 — Vendors, bank, auctions: rules (Phase 5)

**Decision:**
- NPC windows (vendor, bank, auctioneer) need the player within `npc_range` (12 m) and are closed when they walk away. Items cannot be sold from the body or the bank; summoned and worthless items cannot be sold.
- The bank holds items and coin (`BankCoin`); moves into or out of bank slots need an open bank.
- **Auctions** are global (owned by the game server, not a zone) and persist. Listing costs `max(5% of price, 10 copper)`, not refunded; listings last 48 h; at most 20 per character, counting sold and expired ones until collected. Proceeds and unsold items wait at the auctioneer: opening the board collects them (items only when there is room). A buyer must send the price they saw; a changed listing is refused. No-trade and summoned items cannot be listed.

**Alternatives considered:**
- Mailing proceeds to an offline seller's character directly. Rejected: writing to an online character from outside its zone would race its own saves; holding them on the listing keeps one writer per record.

## 2026-11-21 — Pets are mobs with an owner (Phase 4)

**Decision:** A pet is a `MobEntity` with `owner` set. A summoned pet is built from a `content/pets.json` template at the level its spell gives (`summon_pet`, level = `effect_amount`) and has no spawn group. A charmed pet is the wild mob itself, held by a `charm` control line.
- Pets run their own AI (`pet_think`): follow the owner or hold a guard spot; attack on command; otherwise defend, against whatever is hurting the pet first, then the nearest mob fighting its owner within `assist_radius`. "Back off" makes a pet passive for `back_off_seconds`.
- Wild mobs hate pets like players (threat lists hold entity ids), so pets tank.
- Damage a pet deals credits its owner for loot and XP; the threat goes to the pet, scaled by the template's `threat_mult` (earth servants hold aggro).
- Pets are never valid enemies for players, area spells, or other pets.
- A summoned pet that dies leaves no corpse. A charmed pet that dies is an ordinary corpse, and its charmer gets no XP for it.
- Owner death, logout, or zone exit releases the pet: a summoned pet vanishes, a charm breaks. Pets are not saved.
- One pet per player.

**Rationale:** Reusing mobs gives pets movement, melee, buffs, snapshots, and interest management for free. The only new code is the AI and owner bookkeeping. Charm needs a mob to become a pet in place anyway.

**Alternatives considered:**
- A separate `PetEntity` type. Rejected: it duplicates every mob code path.
- Persisting pets across logout. Deferred: EverQuest-style pet persistence (incl. zoning) arrives with zone travel in Phase 6.

## 2026-11-21 — Bard songs pulse; twisting falls out of durations (Phase 4)

**Decision:**
- A song is a spell with `song: true`: no cost, no cast time, resource none. It is learned from a scroll and memorized in a gem like any spell.
- Casting a song gem starts it. It applies its effects at once and then every `rules.songs.pulse_seconds` (6 s) until the Bard starts another song, presses the same gem again, stops (`/stopsong`, Esc), or is stunned, mesmerized, feared, killed, or feigns. Moving does not stop a song.
- Song effects last 18 s (three pulses), so alternating ("twisting") up to three songs keeps all of them up.
- `SongUpdate` drives the client's song indicator and its pulse bar. The `singing` skill improves on each pulse.

**Rationale:** This keeps the Bard identity (no mana, constant music, twisting) without a special effect engine: each pulse is the ordinary spell resolver.

**Alternatives considered:**
- Songs as channelled casts with a cast bar. Rejected: casts stop when the caster moves, and Bards must not.

## 2026-11-21 — Stealth, feign death and aggro drops (Phase 4)

**Decision:**
- **Hide:** a skill roll. It fails while any mob hates the player, and ends on any action. Movement ends it unless the player is sneaking.
- **Sneak:** a toggle. Movement drops to 50%, and proximity aggro needs half the usual radius. Attacking ends it.
- **Invisibility:** a buff stat. It hides the player from proximity aggro until they act against an enemy.
- **Feign death:** a skill roll (Monk), or a fixed chance in the spell (Necromancer 90%, Shadow Knight 75%). Success removes the player from every hate list, and mobs left with no foes walk home without resetting their health (`forgot`). The player lies still until they move or act. Failure tells them who was not fooled.
- **`threat_drop`:** removes a percentage of the user's threat on one target (Rogue Evade 60%, Feint 90%, Enchanter Fading Memory 100%), or on every mob within a radius (Vanish in Smoke).
- **Threat scaling:** every point of player threat is scaled by the class's `threat_mult` (Warrior 1.5, Paladin and Shadow Knight 1.35, pure casters 0.9, Rogue 0.9) and by `threat_pct` buffs (the hate disciplines).

A Catch2 test has a Warrior holding a mob against a Wizard that never runs out of mana; the Warrior stays on top for at least 80% of the fight.

**Rationale:** These are the classic interactions: split-pulling with feign death, rogues shedding aggro, tanks out-threatening nukers. Each is one small rule on the existing threat table.

## 2026-11-21 — Positionals, dual wield, skills and proficiencies (Phase 4)

**Decision:**
- **Positionals:** abilities with `"position": "behind"` (Backstab and its ranks, Garrote) require the user to stand within `behind_arc_degrees` (120°) of the target's back, judged by the mob's facing. Mobs face whoever they are fighting, so a solo rogue rarely gets behind; with a pet or a tank holding the mob, it does. There is no facing check for other melee yet.
- **Dual wield:** each round, a skill-based chance (skill / 250, at most 85%) of an off-hand swing for 60% of a main-hand hit. Monks dual wield their fists from level 1.
- **Enhanced damage:** up to +25% melee damage at skill 250.
- **New skills:** `pick_lock`, `tracking`, `sneak`, `hide`, `feign_death`, `backstab`, `singing`, `archery`, `throwing`, `mend`, `dual_wield`, `enhanced_damage`. All have per-class unlock levels and caps, and improve with use.
- **Pick lock:** works on a `lockbox` NPC (a practice strongbox in the Proving Flats) until doors and chests arrive in Phase 6.
- **Proficiencies:** each class lists its heaviest armor weight and its weapon categories (`classes.json`). They are validated now and enforced on equip in Phase 5 (`can_wear_armor`, `can_use_weapon`).

## 2026-11-21 — Summoned items expire; illusions are cosmetic buffs (Phase 4)

**Decision:**
- **Summoned items:** `summon_item` puts items flagged `summoned` into the pack with a lifetime. Schema v5 adds `character_items.seconds_left`, and time keeps running only while the character is online (like buffs). Summoned items cannot be loot, and they dissolve on death instead of going into the corpse.
- **Illusions:** an `illusion` effect names a race id (the player looks like that race, with their own gender and class) or a model id (a creature). `EntitySpawn` gains `model` (and `flags2`, `owner_id`), and the player's own client receives its own spawn so it can redraw itself.

**Rationale:** Expiring conjured food and blades are the classic Magician utility. Illusions need no gameplay effect for now: faction-based illusion benefits come with factions in Phase 6.

## 2026-11-21 — Class kits are generated; content shape is enforced (Phase 4)

**Decision:**
- `tools/gen_classes.py` generates every class's abilities and spells, plus the pets, scrolls, trainers and summoned items. It takes the Cleric and Wizard lines from `gen_spells.py`.
- Tiered lines are sized against the server's own curves (an even mob's health, the Warrior's health), as in Phase 3. Signature abilities (Hand of Mercy, Grave Touch, Backstab, Feign Death, charm, pets) are hand-written in the generator.
- CI runs `gen_classes.py --check`.
- `rules.json "content"` turns on two load-time checks, in both C++ and `validate_content.py`: at least 25 abilities and spells per class, and something new at level 1 and then at least every 3 levels to 60.
- Hybrid spells (Paladin, Shadow Knight, Ranger) start at level 9; every class has a level-1 ability.
- Counts: Warrior 30, Paladin 30, Shadow Knight 29, Ranger 31, Bard 32, Rogue 28, Monk 27, Cleric 33, Druid 38, Shaman 39, Wizard 33, Magician 36, Necromancer 35, Enchanter 38. That is 128 abilities and 331 spells and songs, with one trainer per spell-using class.

**Rationale:** "Adding content never requires code changes": the generator only writes JSON, and anyone can hand-edit the output. The shape checks turn the design rule "a new ability at least every 2-3 levels" into something CI enforces.

## 2026-11-21 — Final races (Phase 4)

**Decision:** There are eight races:
- Highlander (every class) → Emberhold;
- Vaelith → Thornwood;
- Deepkin → Cragmouth;
- Duskborn (grey cave-dwellers, dark arts) → Cragmouth;
- Hollowfolk (small burrow-dwellers) → Thornwood;
- Frostmane (northern clansfolk) → Emberhold;
- Morrok (hulking swamp brutes) → Emberhold;
- Tinkerling (tiny inventors) → Cragmouth.

All names are original. Stats lean on the classic archetypes (Morrok strong and sturdy, Tinkerling and Duskborn clever). Every class is open to at least two races, and Monk to Highlander and Frostmane. Looks are scale, tints and parts on the shared humanoid rigs (`appearance.json`). Existing characters keep their races.

## 2026-11-21 — Balance simulator v1 and the Phase 4 table

**Decision:** `tools/balance_sim` (`emberfall-balance-sim`) drives a real headless zone. For each class and level, one character fights 12 even-level mobs in a row, resting 60 s between them, so disciplines and once-an-hour abilities keep their real recast timers.
- **The autopilot's kit** comes from content alone: the best rank of each line the class knows at that level, memorized pet, heal, nuke, DoTs, slow and lifetap first, then pre-fight buffs; for Bards, a twist of their fire song and haste song.
- **How it plays:** it uses damage abilities and offensive disciplines whenever ready, heals below 45% health, and runs melee classes in at normal speed. Casters open from 25 m.
- **What it reports:** time to kill, win rate, health left, mana per kill, and mob health per mana point. It flags any class whose time to kill is beyond 3x the level's median, or whose win rate is below 50%.
- **Where it runs:** CI runs it and prints the table. A Catch2 smoke test fails only on extreme gaps: beyond 5x the median, or winning fewer than a quarter of fights.

Table (12 fights per cell, no gear, `--fights 12`):

```
Time to kill (s) / win rate    L1     L5    L10    L15    L20    L30    L40    L50    L60
warrior                       9.5   12.4   14.3   12.5    9.9    9.1    9.7   10.6   10.4
paladin                      10.7   17.1   19.2   20.7   17.2   22.2   20.2   27.7   23.5
shadow_knight                10.7   17.1   20.0   14.0   14.4   18.6   14.4   17.6   18.0
ranger                        8.2   11.9   14.9   14.0   12.6   12.3   12.2   12.7   14.8
bard                         12.1   11.6   15.5   16.0   15.1   18.5   16.9   16.4   17.9
rogue                        13.1   16.1   17.6   18.5   17.9   14.7   13.5   15.5   16.2
monk                         10.1   14.0   12.9   10.3   12.0   10.6   10.0   11.4   11.4
cleric                       11.6   13.5   13.5   15.1   15.5   16.8   16.8   20.0   21.8
druid                         8.5   10.9   10.3   12.4   11.7   11.6   12.1   12.8   12.4
shaman                        9.1   12.4   16.1   16.6   17.8   15.0   16.5   15.9   22.6
wizard                       10.0   10.6   10.0   13.5   11.2   15.0   12.2   13.0   17.3
magician                      7.3    8.0    9.6    9.5   10.7   11.1   10.5   11.0    9.2
necromancer                  17.6   15.7   13.8   15.1   15.1   15.9   13.8   15.6   13.2
enchanter                    10.6   12.4   17.0   18.4   14.0   16.4   16.3   17.2   18.8
(every cell: all 12 fights won)
```

No class is beyond 3x the median at any level: the widest gap is the Paladin at level 50, 27.7 s against a median of about 15 s (1.8x). These are flagged for later tuning rather than hidden:
- **Warrior and Monk kill fastest of the melee classes.** Their large strikes and disciplines make them stronger solo than a tank should be. Retune with gear in Phase 5.
- **Paladin is the slowest.** Its offensive spells are undead-only, and its heals are rarely needed against an even mob. It needs either a general smite or the group content where its identity (tank-healer) pays off.
- **Magician is the fastest caster at every level, and the Wizard is mid-pack.** That matches the classic solo picture, where pets carry, but it is off the "Wizard has the highest burst" identity. A burst-only metric (damage in the first 10 s) should be added to the sim before touching numbers. Pet damage per second was normalized (`damage_mult / attack_delay` near an even mob's) after the first run showed the fast air servant at twice a mob's damage.
- **The Rogue is middling solo.** Its backstab needs a tank or pet holding the mob, which the solo sim never has.
- **Mana:**
  - Cleric, Shaman and Druid spend 70-98% of their mana per kill above level 30. Their nukes are the inefficient part of their kits, as intended for healers.
  - The Necromancer has the best mana efficiency of the pure casters at low levels (DoTs and drains).

Every class wins every fight because these are even-level mobs, a character with skills at cap, and no gear. Real tuning needs gear (Phase 5), groups (Phase 7) and named dungeon mobs (Phase 6). A group simulator and a burst metric are next for v2.

**Alternatives considered:**
- A pure-math damage-per-second model. Rejected: it would miss resists, misses, travel time, pets tanking, threat and stuns. The zone-driven sim plays by the server's real rules, with no game logic duplicated in tooling.

## 2026-11-21 — Model pipeline: data-driven looks (Phase 3.5)

**Decision:** Everything about how an entity looks lives in content and is never in code:
- `content/models.json`:
  - `models`: scene path, scale, nameplate height, collision radius, yaw offset, placeholder shape and colour, swappable part groups, default parts, default tints, and attachment points (bone plus offset);
  - `animation_maps`: standard state → clip, with ground speeds;
  - `item_visuals`: held meshes by slot.
- `content/appearance.json`: race + gender → model, scale (uniform or per-axis), tints and parts; class → tints, parts and default held items.
- Items name an item visual (`"visual"`). Mobs and NPCs name a model id.

The client reads a generated copy, `client/data/visuals.json` (`tools/sync_client_content.py`; CI checks it is fresh), and resolves a look in layers: model defaults, then race and gender, then class. Several model ids can share one GLB with different tints, parts and scale:
- the generated humanoid rig (`emberfall_basic`) serves players, generic NPCs, skeletons, giants, imps, treants and wraiths;
- the generated quadruped rig serves rats, wolves, boars and drakes;
- the converted Quaternius RPG Character rigs serve the trainers, brigands, cultists and hollow knights;
- the Quaternius Universal Animation Library mannequin serves golems.

`tools/inspect_model.py` lists each file's clips and bones.

**Rationale:** "Swapping a model is a JSON edit only" (the phase's acceptance criterion), and 8 races × 14 classes must not need 112 models. Tints and part toggles on shared rigs give that variety for free. The same files will carry equipment visuals in Phase 5 and the art pass in Phase 9.

**Alternatives considered:**
- Per-race Godot scenes (`.tscn`) with variants baked in. Rejected: every new look would be editor work and harder to validate in CI.
- Appearance inside `races.json`/`classes.json`. Rejected: the client copy would then drag in gameplay data it does not need.

## 2026-11-21 — Gender on characters (protocol v5, schema v4)

**Decision:** Characters get a gender at creation (0 = female, 1 = male). `CharCreate`, `CharSummary` and `EntitySpawn` carry it (protocol v5). The `characters` table gains a `gender` column (schema v4, migrated in place, existing characters become female). The server rejects other values. Gender only picks the model: no stats, no class rules. Fresh player corpses now carry the owner's race, class and gender in their spawn, so they look like the character. Corpses restored from the database after a restart keep the generic `humanoid` model, because the corpses table does not store appearance.

**Rationale:** CLAUDE.md asks for models by race + gender. Appending the field to each message keeps the codecs simple, and the version bump makes old clients see "please update".

**Alternatives considered:** A free-form appearance blob (face, hair, colours) chosen at creation. Deferred: it needs a customization UI and validation rules, and Phase 9 owns appearance options.

## 2026-11-21 — Animation state machine and fallbacks

**Decision:** `client/scripts/world/anim_state.gd` is a pure state machine (no nodes, unit-tested headlessly). Every input is server state:
- **Speed:** ground speed from interpolated snapshot motion (remote entities) or from prediction (the local player).
- **Flags:** dead, sitting, stunned/mezzed, in combat.
- **Casting:** `CastStart`/`CastEnd`.
- **Combat events:** swings for the attacker; a hit reaction for whoever takes direct damage.

Priority is death > sit > one-shot (swing, hit, cast release) > cast loop > stunned idle > locomotion. Locomotion has hysteresis (start moving above 0.35 m/s, stop below 0.2, walk/run split at the map's `walk_run_threshold` ±10%).

Standard state names: idle, combat_idle, walk, walk_back, run, attack1, attack2, cast, hit, death, sit, swim, fall. A missing clip falls back along a fixed chain (for example run → walk → idle, attack2 ↔ attack1, cast → idle). A missing hit reaction is skipped. A missing death or sit becomes a procedural pose. Placeholders and models without clips get procedural motion for everything.

Swings alternate attack1/attack2, abilities use attack2, and a second swing in the same tick (double attack) does not restart the clip. Instant spells play a short cast release (they send no `CastStart`). A corpse that spawns within 2 s and 2 m of a death plays the death clip; one that already existed when we arrived shows the final pose.

**Rationale:** Driving animation only from what the server already sends keeps the client honest, and needs no protocol change beyond gender. Fallbacks let a pack with a dozen clips and a pack with three both work.

**Alternatives considered:**
- Godot `AnimationTree` state machines per model. Rejected for now: they live in scene resources, which defeats the "drop in a GLB and edit JSON" goal. AnimationPlayer with cross-fade blending is enough at this fidelity.
- A server-sent animation state field. Rejected: redundant with flags and events, and more bandwidth.

## 2026-11-21 — Foot sliding, facing and smoothing

**Decision:**
- **Foot sliding:** walk and run play at `actual speed / (ground_speed × model size)`, clamped to 0.4-2.5x. `ground_speed` is per clip in the animation map, and the model's size comes from all scales combined, so big models stride longer. Walking backwards without a `walk_back` clip plays walk in reverse.
- **Facing:** the visual turns smoothly toward its desired yaw (exponential, 12/s) instead of snapping, so the 20 Hz snapshot yaw and mouse-look never jerk the body. While standing, an entity faces the target of its current cast, or of its last swing or landed spell, for 4 s. While moving, it faces the way it moves. This is visual only: the entity's root yaw (camera, movement, server) is untouched.

**Rationale:** Matches the phase's interpolation and facing items without touching movement prediction or the server.

**Alternatives considered:**
- Root motion. Rejected: the server is authoritative for position, so animations must follow movement, not drive it.
- Turning the player's real yaw toward the target. Rejected: it would fight player input and the server's view of facing.

## 2026-11-21 — Attachment points and held items

**Decision:** Each model lists attachment points (`main_hand`, `off_hand`, `back`, `head`, plus custom ones such as `off_hand_held`), each a bone name and an optional offset. An item visual names a slot, and optionally a different attach point (an off-hand dagger goes in the hand, a shield on the forearm), plus its own offset. The client attaches it with a `BoneAttachment3D`.

Until equipment exists (Phase 5), each class holds placeholder "training" items (`kind: "gear"`) listed in `appearance.json`. Examples: sword and shield for Warriors, a staff for Wizards, a bow on the back for Rangers. Other players see them because the spawn carries the class.

A missing mesh or attachment point skips that item with a warning. Placeholders hold items at fixed points.

Held meshes inherit the body's non-uniform race scale (a Deepkin's sword is a little squat). Correcting for that needs bone-space compensation, which is left to the Phase 9 art pass.

**Rationale:** It proves the attachment path end to end now. Phase 5's equipment display then only changes *which* item fills a slot.

## 2026-11-21 — Collision radius and melee reach

**Decision:** A model's `collision_radius` (metres at entity scale 1) is used by the client for click picking, and by the server for mob melee reach: `melee_range + collision_radius × mob scale`. This replaces the old fixed `0.5 × scale`. The starter humanoids use 0.45-0.5 and the beasts 0.5-0.6, so reach changed by at most 0.1 m per unit of scale.

**Rationale:** Reach that follows body size is what the genre does, and it keeps the number in one place.

## 2026-11-21 — Starter art: generated models plus Quaternius CC0 packs

**Decision:** The phase asked for free CC0 packs (Quaternius, Kenney). Scripted downloads from the official hosts were blocked here: Quaternius' Google Drive folders returned "quota exceeded", and Kenney's download needs a signed link from a browser session, which the sandbox refused to script. Nothing was faked.

Two sources now make up the art:
- **Generated starter models.** `tools/gen_models.py` (standard-library Python) generates original low-poly GLB models, deterministic and committed (CI runs `--check`). They are ours and CC0:
  - a female and a male humanoid on one 18-bone rig, with 10 clips, 13 swappable part meshes and 8 named materials for tinting;
  - a quadruped with 6 clips and 3 optional parts;
  - six held items.

  Player characters (race × gender × class layering, attachments) and most mobs use them.
- **Quaternius packs from OpenGameArt.** The same packs are mirrored on OpenGameArt as plain file links. The user downloaded three into the git-ignored `downloads/`. Two are in the build, unchanged apart from format conversion:
  - **LowPoly RPG Characters** (FBX only, converted to GLB) for the trainers, brigands, cultists and hollow knights;
  - **Universal Animation Library** (a Godot GLB mannequin with 46 clips) for golems.

  The UAL 2 pack is not used yet: it is a different skeleton, its female mannequin has no clips, and both need retargeting (Phase 9). ASSETS.md lists everything with its source and license; the license files sit beside the models.

Beetle, snake, spider, bird and horror keep placeholder shapes (`"scene": ""`) on purpose: that path stays tested in real play.

**Rationale:** Real rigged data now, without license risk. The packs went in as content edits only, which proves the "swap by JSON" goal.

**Alternatives considered:**
- Staying with capsules only. Rejected: the pipeline would go untested on real skinned glTF.
- Scraping around download protections. Rejected.
- Using the RPG Characters for players. Rejected: each is a fixed class look with no gender or race variants, which the appearance layering needs.

## 2026-11-21 — Converting FBX with Godot, and Godot's import renaming

**Decision:**
- **FBX conversion:** packs without glTF are converted by Godot itself. `tools/convert_to_glb.sh` imports the files in a throwaway project, then exports each with `GLTFDocument`. That keeps one format in the repository, which the validator can read. The RPG Characters' FBX files reference textures by an absolute path from the author's machine, so the importer left their materials untextured. The converter binds `<material>.png`, `*<material>.png` or `<file>_Texture.png` from beside the source, and embeds it.
- **Renaming on import:**
  - Godot's FBX importer turns dots in bone names into underscores (`Foot.L` becomes `Foot_L`).
  - Godot's glTF importer drops a `_Loop`/`-Loop`/`_Cycle`/`-Cycle` suffix from clip names (`Walk_Loop` becomes `Walk`) and marks those clips looping.

  The client (`runtime_clip_name`), the validator and `inspect_model.py` apply the same clip rule, so a map can use the name as it appears in the file.
- **Extracted textures:** the `*_albedo.png` files that Godot extracts beside an imported GLB are git-ignored. They are importer output and `--import` regenerates them.

**Rationale:** Without these rules, a map written from the file's own clip list fails at runtime and passes validation. That was found by measuring the mannequin, not by guessing.

## 2026-11-21 — Measured ground speeds

**Decision:** `ground_speed` values come from `tools/godot_convert/measure.gd`. It plays each locomotion clip in Godot and tracks the feet, giving a planted-foot speed and a stride-range speed. The first hand-picked values were wrong:
- **Emberfall humanoid:** walk 1.5 and run 3.6 m/s (was 1.7/5.2).
- **Quadruped:** walk 0.35 and run 1.1 m/s at scale 1 (was 1.4/4.2).
- **RPG Characters:** walk 1.7 and run 4.3 m/s.
- **UAL mannequin:** walk 1.05 and jog 3.6 m/s.

Small, fast mobs (a 0.45-scale rat running at 5 m/s) still hit the 2.5× playback clamp, so their feet slide a little. That is accepted until the Phase 9 art pass gives beasts proper strides.

A CodeRabbit review also found that the generated walk and run lifted each knee during the wrong half of the stride, which made the planted foot slide forward. Fixed in `gen_models.py`.

## 2026-11-21 — Client tests and the client content copy

**Decision:**
- `client/tests/test_runner.gd` runs headless in CI (`godot --headless --path client --script res://tests/test_runner.gd`, after `--import`). It covers the state machine, fallbacks, speed matching, look layering, GLB loading, parts, tints, attachments, placeholders, swapping by data only, facing, and the preview. `tests/` is excluded from the export.
- The client's first bundled content copy, `client/data/visuals.json`, is generated and checked in CI. The export preset includes `data/*.json` explicitly.
- `tools/validate_content.py` now opens every referenced GLB and checks clip names, bone names, part nodes and tinted materials.
- The autotest gained `--gender`, `--expect-models` and `--expect-anims`, and `client_e2e.sh` uses them, so CI proves real models load and the main states play in live runs.

**Rationale:** Most failures in this pipeline are silent typos (a clip or bone name). Catching them in CI is the point.

## 2026-11-21 — Spell engine: casting model (protocol v4)

**Decision:** Abilities (`abilities.json`) and spells (`spells.json`) share one definition type and one effect resolver (`zone_spells.cpp`); spells are the ones learned from scrolls and cast from gems.
- **Cast state machine** (`spells.hpp`, pure): Idle → Casting (on `CastSpell`) → Idle. The cast completes when its time elapses, or is interrupted or cancelled (`StopCast`, Esc in the client).
- **Costs and timers:** mana is checked when the cast starts and paid when it lands, so an interrupted spell costs nothing. The recast timer and the 1.5 s global cooldown start when it lands.
- **Interrupts:**
  - moving more than 0.5 m;
  - being stunned, mesmerized, or feared;
  - a failed concentration check whenever damage lands (see "Resists and concentration").
- **No melee while casting:** auto-attack swings pause while a spell is being cast.
- **Range and line of sight:** checked when the cast starts, then again when it lands with 10% slack, so a target that drifts slightly out of range still gets hit. Line of sight is tested against zone `obstacles` (vertical cylinders). The test zone has none yet; zone geometry arrives with Phase 6.
- **Wire messages:**
  - client → server: `CastSpell`, `StopCast`, `MemorizeSpell`, `ScribeSpell`, `TrainerOpen`, `TrainerBuy`, `RezResponse`;
  - server → client: `SpellBook`, `GemBar`, `CastStart`/`CastEnd` (sent to everyone within the event radius), `BuffList`, `TrainerWindow`, `RezOffer`.
  - Existing messages gained fields: `CombatEvent.school` (the resist type, which colors log lines and particles), `PlayerStats.run_speed` (so prediction follows snares), and `ItemStack.kind` (the pack offers Scribe on scrolls).

**Rationale:** One resolver means a new spell is a JSON row, never code. Paying at completion and the interrupt rules follow the genre.
**Alternatives considered:**
- Paying mana up front. Rejected: an interruption would then cost as much as a completed cast.
- Separate code paths for abilities and spells. Rejected: Phase 4's hybrids need both.

## 2026-11-21 — Spell effects, buffs, and stacking

**Decision:**
- **Instant effects:** damage, heal, heal_pct, stun, threat, taunt, area_threat, dispel, resurrect, teleport, restore_mana_pct, restore_endurance_pct.
- **Lasting effects:** buff, debuff, slow, snare, root, mez, fear, dot, hot. All lasting lines of one spell from one caster become **one** effect on the target. It holds stat modifiers, control flags, and a periodic amount, and occupies one slot.
- **Slots:** 15 beneficial (buff) and 10 detrimental (debuff) per target, from `rules.spells`.
- **Stacking** (pure `decide_stacking`, tested):
  - Within a `stack_group` (default: the spell's id), an equal or higher `rank` replaces and refreshes the existing effect; a lower rank is blocked, and the caster is told so.
  - Detrimental effects that tick (DoTs) stack **per caster**, so two Wizards' burns both run. Everything else overwrites regardless of caster.
  - Beneficial and detrimental effects never collide.
- **Ticks:** DoTs and HoTs tick every `tick` seconds (default 3 s), the last tick landing on expiry. A DoT ends if its caster leaves the zone.
- **Crowd control:**
  - Mez stops a mob completely (no actions, no aggro scans) and breaks on any damage.
  - Root stops movement but not swings. Each direct hit has a 5% chance to break it. A rooted mob does not leash for being kited.
  - Snare and slow are percentage modifiers to movement speed and attack delay. Slow 50% doubles the delay.
  - Fear makes a mob run from its caster and stop attacking.
  - Mez, stun, root and fear honor a per-effect `max_target_level`; above it the target is immune.
- **Effects on players:** a snare changes the player's speed (sent in `PlayerStats`). Root, mez and fear hold the player in place; fear-running on players waits for pathing. Mez breaks on damage for players too.
- **Dispel:** on an enemy it strips beneficial effects; on a friend (a cure) it strips detrimental ones, newest first.
- **Death:** clears every effect.
- **Polarity rules:** content validation rejects beneficial spells with harmful effects and the reverse.
- **Test hook:** `Zone::force_effect` places a spell's lasting part on any player or mob as if cast by a nameless caster. Tests use it, and it is the path mob spells and GM commands will use.

**Rationale:** One effect per spell matches how players read the buff tray, and slot counts stay meaningful.
**Alternatives considered:** One effect per stat line (what Phase 2 did). Replaced: a two-line buff would cost two slots.

## 2026-11-21 — Resists and concentration

**Decision:**
- **Resist chance:** `clamp(R / (R + 100 + 5 × caster level) + 0.03 × (target level − caster level), 2%, 95%)`, where `R` is the target's resist for the spell's type plus its debuffs, plus the spell's `resist_adjust`. Type `none` is never resisted.
- **Partial resists:** a damage spell that lands is partially resisted with the same chance, keeping 25–100% of its damage.
- **Mob resists:** default to `5 + 0.5 × level`. Body types override some:
  - undead: high poison and disease, moderate cold;
  - constructs: high poison and disease;
  - fire elementals: fire 150, cold 0;
  - plants: fire 0;
  - dragons: fire 80.
- **Concentration** (keeping a cast through a hit): `clamp(0.5 + 0.45 × channeling / (channeling + 100) − 0.6 × damage / max HP, 5%, 97%)`. The `channeling` skill improves with each check.
- **Meditate:** a new skill that raises sitting mana regen by up to 50%.

**Rationale:** An even-level mob resists 5–8% of spells, rising about 3% per level above the caster. Low-level casters are interrupted often in melee, as in the genre, and channeling fixes that with practice.

## 2026-11-21 — Healer aggro

**Decision:** Healing a player gives the healer threat on every living mob that already has that player on its threat list **and** is within `heal_aggro_radius` (60 m) of the healer.
- The amount is half the healing actually done, split evenly between those mobs.
- Buffing someone who is in a fight adds `2 × spell level` threat per such mob.
- Overhealing (no HP restored) and mobs not yet in the fight generate nothing.
- HoT ticks count as heals.

**Rationale:** Healers draw aggro in a way the tank can manage, and healing never pulls a whole camp.

## 2026-11-21 — Learning spells: scrolls, spellbook, gems, trainers

**Decision:**
- **Scrolls:** each spell has a scroll item ("Spell: Name", kind `scroll`).
- **Scribing:** consumes the scroll. It needs the spell's class and level, not already knowing it, and room in the 120-spell book.
- **Gems:**
  - gems open at levels 1, 1, 4, 9, 16, 25, 35 and 45 (2 at start, 8 at 45);
  - a spell sits in one gem at a time;
  - memorizing is refused in combat or while casting, and the gem is not castable for 2 s;
  - losing a level can close gems; their spells stay in the book.
- **Trainers:** NPCs (`npcs.json`, entity kind 3) that sell every scroll of their class at `5 + 2 × level²` copper. You must be within 15 m, and you can buy a scroll above your level but not scribe it.
- **New casters:** start with two level-1 spells scribed and memorized (`classes.json starter_spells`) and 25 copper (`rules.starting_coin`). The third level-1 spell, Frost Needle for Wizards and Holy Rebuke for Clerics, is bought from the trainer, so the very first session goes through the whole path: buy, scribe, memorize.
- **Hotbar:** slots can hold `gem:N`. A class with no abilities of its own gets a default bar of auto-attack plus gems 1-8 on keys 1-9.

**Rationale:** The classic acquisition loop with the fewest moving parts. Trainers selling every rank keeps every spell reachable at its level until vendors and loot tables arrive in Phase 5. Content validation enforces reachability, as the Phase 3 scope requires.
**Alternatives considered:** Gating high ranks behind loot. Deferred to Phase 5/6 content.

## 2026-11-21 — Resurrection

**Decision:** A resurrection spell targets a player corpse whose owner is in the zone (not your own).
- **The offer:** when the spell lands, the owner gets a `RezOffer` that is valid for 60 s, which they accept or decline.
- **Accepting:**
  - restores `pct`% of the XP that death cost (50%, 75% or 96% by rank);
  - revives the owner at the corpse with 20% health, mana and endurance, or teleports them there if they had already respawned;
  - leaves the gear on the corpse to loot;
  - saves the corpse and the character in one transaction.
- **Once per corpse:** a corpse can be resurrected once (`rezzed`). After a restart its `xp_lost` is 0, so a second resurrection restores nothing.

**Rationale:** Classic behavior, with no way to farm XP from one corpse.

## 2026-11-21 — Wizard travel and gather mana

**Decision:**
- `teleport` effects go to `safe_spot` (an evacuate: everything chasing the caster forgets them), `bind`, or a city.
- A teleport is refused at the start of the cast unless its destination is in the caster's zone. Real zone-to-zone portals arrive with zone lines in Phase 6.
- Group-target spells (Wind Retreat) land on the caster only until groups exist (Phase 7).
- Gather Aether restores 60% of mana (20-minute recast); Deep Aether restores 100% (15 minutes).

## 2026-11-21 — Phase 3 spell content and balance

**Decision:** `tools/gen_spells.py` writes 30 Cleric and 30 Wizard spells, their scrolls, and the two trainers; the output is committed.
- **Sizing:** spell power is derived from the server's own curves. A rank learned at level L deals about 38% of an even-con mob's HP (Wizard fire) or heals about 45% of a Warrior's HP at L, then grows per level until it caps near the next rank. Mana cost comes from a damage-per-mana target of about 1.3 + 0.01 × level.
- **Wizard lines:** fire (7 ranks), cold + snare (4), arcane (3, slow but efficient), point-blank AE (3), targeted AE, root (2), a fire DoT, a resist/armor debuff, self wards (3), gate, evacuate, a city translocation, and gather mana (2).
- **Cleric lines:** heals (7), HoTs (3), armor/HP blessings (5), undead nukes (4, undead only), smites (3), stuns with level caps (3), resurrection (3), fear vs undead, and a cure.
- **Balance check** (`[spells][balance]`): a scripted, skill-trained, ungeared caster faces an even-con mob at levels 1, 5, 10, 15, 20, 30, 40, 50 and 60. Wizards win by nuking; Clerics by smiting, meleeing, and healing themselves. It must win at least 75% of fights; today both win every fight.

**Rationale:** Generated tiers keep 60 spells consistent and retunable from one place; hand edits stay possible in the JSON.

## 2026-11-21 — Content validation for spells

**Decision:** Both the server and `tools/validate_content.py` reject:
- a spell that no starter grant, trainer, or loot drop teaches;
- a scroll that teaches an unknown spell, or a trainer selling to a class it does not teach;
- polarity violations (see "Spell effects, buffs, and stacking");
- a corpse target without a resurrect effect;
- a teleport to an unknown destination;
- bad gem-unlock tables;
- **level bands:** any class with abilities that lacks one in each band (1-15, 15-30, 30-45, 45-60). The 11 classes with no abilities yet are skipped until Phase 4 (the Python validator lists them).

## 2026-11-21 — Character schema v3 and persisted timers

**Decision:** Characters gain `spellbook`, `gems`, `buffs` and `timers` (JSON columns, schema v3, migrated in place).
- **Buffs** are saved with their caster level and time left, and rebuilt from the definition on login.
- **Timers:** every ability and spell recast is saved with its time left. This closes the Phase 2 gap where long cooldowns reset on relog.

## 2026-11-21 — Tick rounding

**Decision:** Seconds-to-ticks conversion subtracts a tiny epsilon before rounding up, so 1.95 s at 20 Hz is 39 ticks, not 40, despite float error.

## 2026-11-21 — Client spell UI

**Decision:**
- **Layout:**
  - the gem bar (8 round gems) sits above the hotbar, with the cast bar above it;
  - your effects show as a tray at top right, and your target's under the target frame (blue rim = buff, red = debuff, blinking for the last 10 s, tooltips with caster and description);
  - the spellbook (B) lists spells with their numbers.
- **Memorizing:** drag a spell onto a gem, or press Mem for the first free gem. Right-click forgets a gem.
- **Keys:** T opens a targeted trainer; P shows the group stub; Esc stops a cast.
- **Spell effects:** simple particle placeholders: a ring of motes while casting, and a burst in the school's color when a spell lands.

**Rationale:** Placeholder visuals that read clearly, and every action works by mouse or key.

## 2026-11-20 — Snapshot entity selection

**Decision:** Each player keeps the tick at which each visible entity's state was last sent to it. An entity goes into the next snapshot when:
- it changed in the last tick (it is sent twice, because the channel is unreliable),
- it changed since its last send, or
- it has not been sent for a second (the heartbeat).

Corpses are static and only need their spawn message. If more than 256 entities are due, the longest-waiting go first.
**Rationale:** Idle mobs no longer cost bandwidth every tick, and no entity can be starved by busier neighbors (a CodeRabbit finding).

## 2026-11-20 — Phase 2 combat model (protocol v3)

**Decision:** Tab-target combat runs entirely on the server in the zone tick.
- Clients send intents: `SetTarget`, `AutoAttack`, `UseAbility`, `RespawnRequest`, `LootOpen/Take/Close`, `SetSitting`.
- Failures are answered, not punished. `AbilityResult` carries the reason and the time left on cooldowns.
- Only intents sent outside the world count as violations. A per-session action limiter (10/s, burst 20) silently drops spam.
- The server reports outcomes as `CombatEvent`s (source, target, kind, amount, ability name) to every player within `event_radius`.
- The client formats the combat log and floating text from those events. The server only sends combat-log text for things events don't cover: XP gains and range warnings.
- `PlayerStats` goes on the unreliable channel. It is sent on change at most every other tick, plus a 1 s heartbeat.
- `EntityState` gains `hp_pct` and `flags`. See "Snapshot entity selection" for which entities go out each tick.

**Rationale:** Keeps every rule server-side and the wire small, and the combat log becomes a pure presentation concern.
**Alternatives considered:** Server-formatted log lines. Rejected: more bandwidth, and no structured data for floating text.

## 2026-11-20 — Combat math, mob scaling, and the balance target

**Decision:** All formulas live in `server/src/game/rules.cpp` as pure functions, and every constant is in `content/rules.json`.
- **Hit and avoidance:**
  - Hit chance: base 72%, ±0.15% per skill point of offense versus defense, ±1.5% per level, clamped to 5–95%.
  - Riposte, parry and dodge are checked in that order after a hit roll. Each chance is `skill / (10 × (5 × attacker level + 50))`, capped at 25%; riposte is 0.6× that.
- **Damage:**
  - Armor mitigation is `AC / (AC + 50 + 25 × attacker level)`, capped at 75%.
  - Crits do double damage.
  - Double attack chance is `skill / 500`.
- **Mobs:** HP and max hit are `base + per_level × L + quad × L²`, scaled by per-mob multipliers.
- **Placeholder weapon:** players use a class weapon (base + per-level damage, fixed delay) until items arrive in Phase 5.
- **Balance target:** a Monte Carlo test (`[balance]`) checks a skill-capped, ungeared Warrior against a generic even-con mob. It must win at least 80% of fights, each lasting 5–90 s, at levels 1–60. The current tuning wins nearly every fight in 11–30 s with 60–75% HP left.

**Rationale:** Classic-feeling, tunable, deterministic under a seeded RNG (xoshiro256**). The first tuning failed the balance test from level 20 up: mob damage grew quadratically while player damage grew linearly. The fix was a flatter mob damage curve and about 2.3× weapon damage per level.
**Alternatives considered:** EverQuest's exact tables. Not reproducible from public knowledge, and not needed.

## 2026-11-20 — XP curve and consider colors

**Decision:**
- **XP curve:** `xp_to_next(L) = 10 × L² × 1.0718^(L-1)`. Kill XP is `1.5 × mob_level² × con multiplier`: grey 0, green 0.5, blue 0.85, white 1, yellow 1.15, red 1.3.
- **Pace:** because 1.0718¹⁰ ≈ 2, the kills (and so the time) needed for levels 50→60 roughly equal those for 1→50. A test checks the ratio is between 0.8 and 1.25, and that level 60 is reachable in a finite number of kills.
- **Con colors:**
  - Red at +3 levels or more, yellow at +1/+2, white at even.
  - Below the player's level: blue within `1 + L/7` levels, green within `1 + L/6` more, then grey.
- **Groups:** `split_group_xp` (shares by level, +10% per extra member) is the group hook. Every kill passes a single member until groups arrive in Phase 7.

**Rationale:** Matches the CLAUDE.md rule that levels 50→60 take about as long as 1→50, with a level 2 in about five even-con kills.

## 2026-11-20 — Mob AI and threat

**Decision:**
- **AI states:** a pure state machine (`mob_ai.hpp`): idle, wander, chase, aggro (melee), flee, leash, dead, respawn. The zone applies each state's movement and attacks.
- **Aggro and assist:**
  - Mobs aggro on living, non-grey players within `aggro_radius`, scanning every 4 ticks with a staggered start.
  - Same-faction mobs with `assist` join within their `assist_radius`.
- **Leashing:** mobs leash if they get more than `leash_radius` from home, lose every target, or go 30 s without reaching melee range.
  - A leashing mob is immune ("evades") and forgets its threat.
  - It heals to full when it gets home.
- **Fleeing:** happens once per fight, below `flee_hp_pct`, for 6 s.
- **Threat:**
  - Damage adds threat equal to the damage, and abilities can add bonus threat.
  - A taunt sets the taunter to the current top plus 1.
  - Healing adds half the healed amount, split across the mobs already fighting the healed player.
  - The mob attacks the top of the list.
- **Pathing:** none yet. Zones are flat and mobs move in straight lines.

**Rationale:** Deterministic and unit-tested. Evade-on-leash prevents kiting exploits. Real pathing arrives with zone geometry in Phase 6.
**Alternatives considered:** EverQuest's no-leash behavior. Rejected: it encourages training mobs onto other players.

## 2026-11-20 — Warrior abilities, skills, and passives

**Decision:**
- **Abilities:** 16 Warrior abilities in `content/abilities.json`: 12 active and 4 passive, levels 1–56, all with original names.
  - Taunts: Iron Challenge and Warlord's Challenge.
  - Strikes: Rising Kick, Cleaving Blow and Earthsplitter.
  - Pommel Crack stuns, and fails on mobs more than 2 levels above you or immune to stuns.
  - Rallying Cry and Stalwart Resolve heal.
  - Disciplines: Bulwark Stance and Unbroken Wall (defensive), Battle Fury (offensive).
  - Provoking Roar adds threat to every engaged mob nearby.
- **Effects:** abilities are built from a small effect vocabulary: damage, stun, threat, taunt, buff, heal_pct, restore_endurance_pct, area_threat. Phase 3's spell engine extends it.
- **Timers:** a 1.5 s global cooldown applies unless an ability sets `"gcd": false` (the disciplines). Facing is not checked yet.
- **Skills:**
  - Caps are `min(max, per_level × level + 5)` from the class's unlock level. `skills.json` holds defaults and `classes.json` holds per-class overrides.
  - Skills improve through use (offense per swing; defense and avoidance per incoming attack; ability skills per use) with a chance that falls as the skill nears its cap.
  - A newly unlocked skill starts at 1.
- **Passives:** Dodge, Parry, Double Attack and Riposte are skill-driven. Their ability entries exist for display and must match the skill's unlock level.

**Rationale:** Data-driven from the start, so adding classes in Phases 3 and 4 is mostly content.

## 2026-11-20 — Death, corpses, and loot

**Decision:**
- **On death:**
  - The player loses `10% × xp_to_next(level)` of XP (none below level 6). They can drop a level.
  - Their coin and whole pack move to a corpse that persists for 1 hour in the `corpses` table.
  - The corpse is written in the same transaction as the stripped character, so a crash cannot duplicate or lose items.
- **Respawn:**
  - `RespawnRequest` (the death dialog button) revives the player at full health at their bind point, with a hard movement correction.
  - Camping out while dead revives the character at the bind point before saving.
  - Characters are bound at their starting city.
  - Phase 1 characters have no bind yet and fall back to their city.
  - A bind in another zone uses this zone's safe spot until zone lines exist in Phase 6.
- **Corpse rules:**
  - Only the owner can loot a player corpse, and an emptied one stays until it decays (a resurrection target in Phase 3).
  - Mob corpses roll coin plus up to `max_drops` items from their loot table.
  - Mob corpse loot goes to the player who did the most damage and is still present.
  - An emptied mob corpse vanishes when the window closes. Unlooted mob corpses decay after 5 minutes, empty ones after 30 s.
- **Looting:**
  - Coin is taken automatically when a corpse is opened.
  - Each loot step is saved immediately; player-corpse loot uses one transaction for the corpse and the looter.
  - Only one player can have a corpse open at a time.
  - The window closes if you walk more than 1.5× the loot range away.
- **Corpse ids:** player corpse ids are random 63-bit values chosen on the game thread, so later updates need no database round trip.

**Rationale:** The CLAUDE.md death rules, plus crash-safe persistence.
**Alternatives considered:** Letting the database assign corpse ids. Rejected: loot could arrive before the insert completes.

## 2026-11-20 — Phase 2 inventory and items are placeholders

**Decision:** Characters get a flat 16-slot pack (stacking by item `stack`) and copper-denominated coin. 10 copper = 1 silver, 10 silver = 1 gold, 10 gold = 1 platinum.
`items.json` has 26 loot and consumable placeholders. There is no equipment, and consumables can't be used yet.
**Rationale:** Looting and corpse recovery need somewhere to put things. Phase 5 replaces this with equipment slots, 8 general slots holding bags, and the bank.

## 2026-11-20 — Test zone layout for Phase 2

**Decision:**
- The Proving Flats grows to 2 × 2 km.
- 25 placeholder mob types (levels 1–60) sit in rings by level band around the safe centre, spread on a golden-angle spiral. Level 1 is about 50 m out; Grimjaw the Ancient (60) is at about 940 m.
- The data comes from `tools/gen_phase2_content.py` and is committed as plain JSON.
- Rings of short posts every 100 m mark rising danger.

**Rationale:** One zone (zone lines are Phase 6) covering the whole level range, reachable on foot, and readable at a glance.

## 2026-11-20 — Client combat UI

**Decision:**
- **Code-built HUD** (`ui/hud.gd`, presentation only):
  - Player frame and target frame, with the border and name in the con color.
  - XP bar and a 10-slot hotbar showing cooldown and GCD sweeps.
  - Ability window (K), pack (I), loot window, and death dialog.
- **Hotbar:** drag-and-drop uses Godot's `_get_drag_data`/`_drop_data`. Slot 1 defaults to Attack; layouts persist per character in `user://hotbar_<name>.cfg`.
- **Targeting:** Tab and Shift+Tab cycle living mobs by distance. Clicking picks in screen space, with no physics shapes. F1 targets yourself.
- **Keys:** R attack, 1–0 hotbar, L loot, C consider, X sit.
- **Slash commands:** /sit, /stand, /loc, /con, /attack, /target.
- **Chat:** the window gains a Combat tab.
- **Visuals:** mobs and corpses are simple capsules by model family.
- **Autotest:** `--fight N` and `--die-at X,Z` drive the same UI paths for end-to-end checks.

**Rationale:** Keeps Phase 1's UI-in-code approach, and makes the full combat loop scriptable in CI.

## 2026-11-20 — Phase checklist follows the prompt pack

**Decision:** docs/PROGRESS.md lists Phases 0-8 exactly as defined in `emberfall_prompts.md`.
**Rationale:** The initial checklist was a guess written before the phase prompts were available.

## 2026-11-20 — Login over TLS with pinned certificates; OpenSSL added (protocol v2)

**Decision:** Passwords travel only over a TLS 1.2+ TCP login service (port 7779), never over ENet.
The server uses OpenSSL 3 from the system (`libssl-dev`), which adds to the dependency list in CLAUDE.md.
The flow:
1. `HelloResult` (ENet) carries the server certificate, the login port, and a random per-connection nonce.
2. The client checks the certificate against its pin for that server address (trust on first use
   with a fingerprint prompt, like SSH; a changed certificate is refused).
3. The client sends `TlsLogin` over TLS to exactly that certificate.
4. The server verifies the password with Argon2id on the DB worker and returns a single-use ticket
   (30 s TTL, bound to that connection's nonce).
5. The client presents the ticket with `TicketLogin` over ENet.

The server generates a self-signed P-256 certificate on first start, keeps it in `data/tls/` (key
mode 0600), and logs its SHA-256 fingerprint (also shown on `/health`). The login service caps
per-IP attempts (burst 10, then 12/min) and applies whole-request I/O deadlines. It runs on 4 worker
threads with a bounded queue. The TLS-only messages use channel id 2, so the ENet channel check rejects them there.
**Rationale:** The user chose a vetted TLS library over hand-rolled crypto. Godot ships an mbedTLS
client with certificate pinning (`TLSOptions.client(cert)`), and OpenSSL on the server is a standard
package. Pinning avoids needing a CA-issued certificate for LAN and self-hosted servers.
**Alternatives considered:** Hand-written X25519/ChaCha20 in GDScript (rolling our own crypto);
a libsodium GDExtension (a native Windows build per client); ENet DTLS (lsalzman ENet has none);
CA-only verification (unusable for self-hosted servers without a domain).
**Remaining exposure:** Game traffic after login (movement, chat) is unencrypted, as in most MMOs.
An on-path attacker could still hijack an already-logged-in ENet session but cannot learn
passwords. Full session encryption would be a separate decision.
**History:** CodeRabbit's Phase 1 scan flagged cleartext credentials. A LAN-only client guard and a
loopback-only Docker bind were added as stopgaps, then removed once TLS login landed.

## 2026-11-20 — Godot 4.7.2 and the Compatibility renderer

**Decision:** Use Godot 4.7.2 (latest stable at the time) and `gl_compatibility`.
**Rationale:** CLAUDE.md requires 4.3+. 4.7 writes the Windows icon and version info into the
exported .exe itself (no rcedit/wine), which makes exporting from Linux and CI simpler. The
Compatibility (OpenGL 3) renderer runs on the widest range of Windows hardware, and low-poly art doesn't need Forward+.
**Alternatives considered:** Pinning 4.3; Forward+ renderer.

## 2026-11-20 — Dependencies: FetchContent (pinned + hashed), libsodium and OpenSSL from the system

**Decision:** ENet 1.3.18, nlohmann/json 3.12.0, spdlog 1.15.3, SQLite 3.50.4 (amalgamation), and
Catch2 3.8.1 come in through FetchContent with SHA-256 hashes. libsodium (pkg-config) and OpenSSL 3
(`find_package(OpenSSL 3.0)`) come from the system packages (`libsodium-dev libssl-dev` /
`dev-libs/libsodium dev-libs/openssl`).
**Rationale:** No extra tooling to bootstrap in Docker or CI; reproducible versions. libsodium has
no CMake build, OpenSSL is security-critical and should get distro security updates, and both are
packaged everywhere. ENet and SQLite are compiled as our own targets because
ENet's CMakeLists predates CMake 3.5 (CMake 4 rejects it).
**Alternatives considered:** vcpkg manifest mode (heavier bootstrap, slower cold builds).

## 2026-11-20 — Protocol definition format and codec rules

**Decision:** `protocol/messages.json` (JSON, not YAML) is the single source. The wire format is a u16
message id followed by fields in order, little-endian. Strings are a u16 byte length plus UTF-8 with
a per-field max. Arrays are a u16 count with a per-field max. Decoders reject: overruns, oversize
strings/arrays, invalid UTF-8 (incl. overlongs/surrogates), NaN/Inf floats, bools other than 0/1,
and trailing bytes. Encoders never fail: they truncate strings on a UTF-8 boundary and clamp arrays,
so the server can never emit undecodable data. Each message declares its direction and channel, and
the server enforces both.
**Rationale:** The generator needs only the Python standard library. Strict decoding is the main
defence against malformed input.
**Alternatives considered:** YAML (needs PyYAML), FlatBuffers/protobuf (less control over GDScript output).

## 2026-11-20 — Movement: intents + prediction + reconciliation

**Decision:** Every 50 ms (one server tick) the client sends a sequenced intent: a move direction of
length ≤ 1, a facing, and its predicted position. The server applies intents with the same pure
`step_position` the client uses for prediction. A per-player input budget (one token per tick, up to 4
banked) caps simulation at one step per tick, so flooding cannot speed a player up. Two checks
trigger a reliable `MoveCorrection`: client predictions that jump implausibly between inputs
(teleport, also counted as a violation), and predictions that drift more than 2 m from the server
(desync). Checks are suppressed for 10 ticks after a correction. Snapshots carry the last applied
sequence number, so the client rewinds to the server position and replays its unacknowledged
intents. Remote players render 100 ms in the past by interpolation.
**Rationale:** CLAUDE.md says clients send intents and the server simulates. This makes speed hacks
impossible by construction, while teleport/desync checks still catch broken or tampered clients.
**Alternatives considered:** Client-authoritative positions with speed checks (EverQuest-style, easier
to cheat).

## 2026-11-20 — Client timing uses the wall clock; delta smoothing off

**Decision:** The client paces prediction steps and its server-time estimate with
`Time.get_ticks_usec()`, and `application/run/delta_smoothing` is disabled.
**Rationale:** On a high-refresh monitor (200 Hz) with the window presenting at 60 fps, Godot's
delta smoothing reported deltas about 3.3x too small, so the client simulated at ~1/3 speed (found in
Windows testing).

## 2026-11-20 — Interest management

**Decision:** A uniform spatial hash on XZ with cell size = the zone's interest radius (80 m in the
test zone). Spawn at ≤ r, despawn beyond 1.15 r (hysteresis). Spawns and despawns go on the reliable
channel; positions go in per-recipient unreliable snapshots that list only known entities.

## 2026-11-20 — Sessions and accounts (see also: Login over TLS)

**Decision:** Usernames are 3-16 chars `[a-z0-9_]`, case-insensitive. Passwords are 8-64 chars.
Character names are 4-15 ASCII letters, capitalized, unique case-insensitively. One session per
account: a new login kicks the older session (saving its character first). Five invalid tickets on
an ENet connection disconnect it, and the TLS login service rate-limits password attempts per IP.
Unknown usernames still pay for one Argon2id verify (timing equalization). Argon2id uses libsodium
INTERACTIVE limits and runs on the DB worker thread, so the game tick never blocks.
"Logout" from the world camps to character select; "Logout" at character select ends the session.

## 2026-11-20 — Abuse limits

**Decision:** Per client: 60 packets/s (burst 120) and 32 KiB/s (burst 64 KiB). Packets over the
limit are dropped, and 200 drops cause a kick. Ten protocol violations (malformed, unknown id, wrong
channel, wrong state, teleport) cause a kick. Max packet 4 KiB, 8 connections per IP, chat limited
to 1 message/s (burst 5). All values are in the config struct.

## 2026-11-20 — Character-creation options come from the server (Phase 1)

**Decision:** After login the server sends `CreateOptions` (races with allowed classes/cities,
classes, cities) built from `/content`.
**Rationale:** It keeps one source of truth until the content-sync pipeline exists (Phase 2). The server
still validates every choice.
**Alternatives considered:** Bundling a synced content copy in the client now.

## 2026-11-20 — Client UI built in code; `preload` instead of `class_name`

**Decision:** Screens are scripts that build their controls in `_ready()`; only `main.tscn` is a
scene file. Scripts reference each other through `preload()` constants rather than global
`class_name`s.
**Rationale:** Hand-written `.tscn` files are fragile, and the global class cache can be missing on a
fresh headless export. Real scenes will come with real art.

## 2026-11-20 — Ports and container layout

**Decision:** Game UDP 7777, TLS login TCP 7779, health TCP 7778. Compose publishes 7777/7779 on all
interfaces (`EMBERFALL_GAME_BIND` restricts them) and 7778 on 127.0.0.1 only. The container runs
as uid 10001, with the database, logs, and TLS identity in the `emberfall-data` volume.

## 2026-11-20 — Placeholder race names

**Decision:** Phase 1 races are Highlander, Vaelith, and Deepkin. An early draft used "Sylvari", which
is another game's race name, and it was replaced.
