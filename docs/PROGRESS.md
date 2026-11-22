# Progress

Work one phase at a time (see CLAUDE.md). At the end of each phase, record **Done / Stubbed / Next** under it.

## Phase checklist

Phases follow the prompt pack in `emberfall_prompts.md`.

- [x] Phase 0: Repository setup (monorepo layout, CLAUDE.md, docs, .gitignore/.gitattributes, first commit)
- [x] Phase 1: Foundations: connect, log in, walk around (server skeleton, protocol + generator, accounts/characters, test zone with interest management and validated movement, Godot client with prediction/interpolation and say chat, bot, tests, Docker, Windows export, CI)
- [x] Phase 2: Core combat and leveling, Warrior only (stats/rules, mobs + AI + threat, auto-attack/GCD, ~10 Warrior abilities, XP to 60, death/corpses/loot basics, combat UI)
- [x] Phase 3: Spell engine with Cleric and Wizard (cast state machine, effects, resists, buffs/debuffs/stacking, scrolls/spellbook/gems, trainers, spell UI)
- [x] Phase 3.5: Character model and animation pipeline (glTF/GLB models from content with placeholder fallback, data-driven animation maps and state machine, attachment points, appearance variants, facing/foot-sliding fixes, CC0 starter assets + ASSETS.md, model previews in character select/create, "how to add a model" doc)
- [x] Phase 4: All 14 classes, pets, skills, threat (pets, Bard songs, positionals, feign/sneak/charm/illusions, skills and proficiencies, final races, balance simulator v1)
- [x] Phase 5: Items, equipment, vendors, bank, trade (600+ items, equipment stats/procs, loot tables and group loot, vendors, bank, secure trade, auction board)
- [ ] Phase 6: World content, in progress: 6a-6f done (6a zone pipeline + procedural dungeons + travel, 6b hubs + 12 outdoor zones, 6c 8 dungeons, 6d 150+ mobs + factions + NPCs, 6e quests + class epics, 6f map/journal/dialogue UI, 6g content validation)
- [ ] Phase 7: Social systems (all chat channels, groups, guilds, friends, /who and slash commands, GM commands with audit log and bans)
- [ ] Phase 8: Polish, balance, load test, release (balance sim 1-60, 100+ bot load test, robustness, client settings/polish, installer + release CI, README/DEPLOY.md, smoke test, tag v0.1.0)
- [ ] Phase 9: Art pass (one art direction; character, equipment, mob and pet visuals; data-driven spell effect library; per-biome environments and zone theming; UI theme and icons; audio and mixer; graphics quality and performance; Credits screen and LICENSES/)

## Phase notes

### Phase 6: World content (in progress)

Worked through in sub-steps (see `emberfall_prompts.md`). Done so far: **6a-6f** (newest first below).

#### 6f: Map, journal, dialogue and faction UI (completed 2026-11-22)

**Verified:**
- **Headless client e2e** (`--quest`, added to `tools/scripts/client_e2e.sh`): a new Paladin walks to Captain Brack in Emberhold and presses T. The dialogue window offers "Vermin in the Glade"; the run accepts it, finds it in the journal and the tracker, opens the map (the minimap is up all along), and abandons it from the journal.
- **Client tests** (429 checks): dialogue options send back their keys, journal lines show progress, the tracker and the factions tab fill in, the map draws a dungeon's tiles, and Escape closes the new windows.

**Done** (`client/scripts/ui/world_ui.gd`)
- **Dialogue window:** what a quest giver says, with buttons for each option the server lists ([Complete], [In progress], offers, accept / not now).
- **Quest journal (U):** active quests with their step, progress (3/8), who to see next, an Abandon button, and the number completed; a **Factions** tab lists your standings by regard, coloured.
- **Quest tracker:** the first five quests under the minimap, gold when ready to turn in.
- **Minimap:** about 90 m around you, drawing the zone's walls and obstacles (and a dungeon's corridors), zone lines (blue), players, NPCs (quest givers gold), mobs, corpses and pets, with your facing arrow and the zone name.
- **Map (M):**
  - a **Zone** tab draws the whole zone with its exits labelled;
  - a **World** tab lists every zone by level band, with its kind and where its roads lead, and yours highlighted.
- **Zone banners** (6a) show each zone's name, kind and level band. **Faction messages** arrive in chat ("Your standing with The Emberhold Watch got better."), and considering shows the regard (6d).
- T on a quest giver opens the dialogue; NPC titles show Soulbinder, Waystone Keeper, Ferry Master and Trade Board.

**Stubbed / placeholder (6f)**
- The world tab is a list, not a drawn map of the continent (zones have no world coordinates yet).
- Map and UI art are flat placeholders (Phase 9).

**Next:** 6g (validation: reachability, mob levels against zone bands, every quest, loot and vendor reference).

#### 6e: Quests and class epics (completed 2026-11-22)

**Verified:**
- **Catch2:**
  - the quest book: refusals (level, class, prerequisites, done, full), kill credit, collecting, deliveries that need the item, ready and turn-in, repeatables, the JSON round trip and malformed input;
  - the shipped quests: 39 regular quests, every giver in the world, every mob a quest asks for spawns somewhere, most dungeon bosses have a quest, and every class has an epic chain of at least three quests ending in an epic weapon it can wield (level 46);
  - in a zone: a kill quest from offer to reward (XP, coin, faction); a delivery the giver hands you the letter for and the next NPC takes; quest items that drop only while you need them, stop at the count, and are handed over; abandoning; epics refused to other classes and to the too-young.

**Done**
- **Quest system** (pure `game/quests.*`, zone side `zone_quests.cpp`, content checks `content_quests.cpp`):
  - steps in order: kill (anyone in the kill's loot party gets the credit), collect (counted from the pack and handed over at the turn-in NPC), and talk or deliver (the item is taken);
  - quest items fall only while you are on the step and short of the count;
  - givers hand over quest items on accepting (all or nothing if the pack is full);
  - rewards: XP as a fraction of a level at the quest's level, coin, items (refused until there is room), and faction;
  - prerequisite chains, class-only quests, repeatables, at most 20 active.
  - Content checks: NPCs, mobs, items, factions and classes exist; quest items are kind `quest`; prerequisites never loop.
- **Dialogue:** `NpcTalk` / `NpcChoose` / `NpcDialog`. Talking to a quest giver advances talk, delivery and collect steps at that NPC, then lists turn-ins, quests in progress, offers, and offers a few levels away; choosing shows the offer, accepts, reports progress or completes.
- **Journal:** `QuestJournal` (active quests with the current step, have/need, who to see next, and the number completed) on entry and on every change; `QuestAbandon`. Progress is saved per character (schema v7's `quests` column).
- **Content** (`tools/gen_quests.py`, 81 quests, 27 quest givers):
  - **39 regular quests** across every band, from 13 givers in the hubs and outposts: culling camps (resolved to the families that really spawn in each zone), trade-loot fetches, quest-item gathers (idols, lens shards, frost essence, cinder cores), the three-city letter chain (Emberhold, Thornwood, Cragmouth), and a quest for nine of the ten dungeon bosses, with armour rewards.
  - **14 epic chains**, one per class, three quests each, from a guildmaster in a hub. The class's writ goes to an outpost contact; three ancient shards come from the depths of a 34-53 dungeon; a boss is slain; and the class's epic weapon is the reward (14 new epic, lore, no-trade weapons with procs, `tools/gen_items.py`).
- **Protocol v10:** dialogue, journal and abandon messages. CI checks `gen_quests.py`.
- **Tests:** 260 Catch2 tests.

**Stubbed / placeholder (6e)**
- Collect steps take trade loot from anywhere in the pack; there is no choice of rewards.
- Epic chains use shared outpost contacts and dungeons (several classes walk the same trials).

**Next:** 6f (world map, minimap, zone banners, journal and tracker, dialogue windows, faction messages).

#### 6d: Mobs, factions and NPCs (completed 2026-11-22)

**Verified:**
- **Catch2:**
  - the regard tiers and their thresholds;
  - every mob has a defined faction, and every hub has its watch, guards, and races that start out amiable with it;
  - an Emberhold watchman ignores a citizen but attacks a player the Watch hates, and considering shows the regard;
  - killing a bandit raises standing with the Emberhold Watch, says so, and updates the standings list;
  - a Thornwood merchant refuses a dubious Morrok until he wears a Vaelith illusion;
  - standing and quest progress survive a save (schema v7), and every old schema still migrates.
- **Headless client e2e:** the whole suite (Phase 1-5 runs in the test zone, the zone walk, the certificate pin check) passes against the release server.

**Done**
- **166 mob types** (6c's generator, plus three city guards), with the specials asked for: enrage, rampage, summon (bosses), root (webs, roots, stares) and poison procs, as well as disease, fire, frost and lightning.
- **Factions** (`content/factions.json`, generated by `tools/gen_mobs.py`; rules in `game/factions.hpp`):
  - 16 factions: the three city watches and 13 monster factions.
  - Standing = default + race adjustment + earned, clamped to -2000..2000, in nine regard tiers (ally ... scowls).
  - At threatening and below, a faction's mobs attack on sight; from dubious, its service NPCs refuse you ("I will not deal with the likes of you.").
  - Killing a member applies the faction's `hits`: hurting the city's guards costs a lot with that city and a little with the others; brigands, skulks, mirefolk, giants, cultists, dragonkin and the undead are worth a little standing with the cities they threaten.
  - Illusions of a race count as that race.
  - Standing is saved per character (schema v7, with a quest-progress column for 6e).
- **Guards:** six watchmen, wardens or guardsmen (level 55) at each hub's gates and squares. They attack only those their city hates, and assist each other.
- **Service NPCs:** each hub's trainers, merchants, banker, trade board, soulbinder and waystone keeper belong to its watch and refuse the disliked.
- **Protocol v9:** `TargetInfo` carries the regard tier and faction name; `FactionStandings` lists your standings (sent on entry and on every change).
- **Client:** considering shows the regard ("an Emberhold watchman regards you indifferently -- looks like it would wipe the floor with you!"); faction changes appear in chat; the standings list is kept for 6f's faction window.
- **Tests:** 254 Catch2 tests and 421 client checks.

**Stubbed / placeholder (6d)**
- Guards and mobs never fight each other: a monster in a city is not chased by its guards.
- Quest givers come with quests in 6e.
- There is no faction window yet (6f).

**Next:** 6e (the quest system, 30+ quests, and a class epic chain for every class).

#### 6c: Dungeons, bosses and mechanics (completed 2026-11-22)

**Verified:**
- **Catch2:** every dungeon has one boss in its boss room with at least one mechanic, named rares, and the dungeon's named items on the boss's loot table; every named item drops somewhere. Mechanics are exercised in zone tests:
  - the Rattle King calls two skeletons at 66% and two more at 33%, each once, and they leave when he resets;
  - the Cinder Foreman enrages after 150 s of fighting, the Storm Herald below 20% health;
  - Mirefang's venom spray hits the tank and the player beside them, never the one 25 m away;
  - the Pale Abbot's dread pulse frightens those close to him only;
  - Admiral Grell summons a target that runs out of reach;
  - a venomfang cobra's bite poisons (a DoT through the spell engine);
  - named rares take their trash type's place, one at a time.
- **Client tests:** all 27 zones (10 dungeons) build from the bundled data (421 checks).

**Done**
- **Ten dungeons** (`tools/gen_world.py`), with cave-mouth entrances in their zones and exits back to them:

  | band | dungeon | levels | zone | boss | mechanics |
  |---|---|---|---|---|---|
  | 1-15 | Rattlebone Warren | 2-11 | Emberglade | The Rattle King | add phases |
  | 1-15 | Mirefang Hollow | 12-17 | Saltmarsh Fens | Mirefang | spread-out venom spray, poison |
  | 15-30 | The Sunken Bastion | 18-24 | Duskmere Shore | Admiral Grell, the Drowned | summon, adds |
  | 15-30 | The Cinderdeep Mines | 21-27 | Redrock Badlands | The Cinder Foreman | enrage timer, rampage, slag burst around him |
  | 15-30 | The Hollowmere Crypts | 27-33 | Gloomfen Bog | The Pale Abbot | fear pulse, adds |
  | 30-45 | The Stormspire Ruins | 34-41 | Shattered Steppe | The Storm Herald | spread-out chain lightning, enrage |
  | 30-45 | The Gloomroot Caverns | 40-46 | Frostveil Tundra | The Root Mother | rooting quake, three add phases |
  | 45-60 | The Deathless Sanctum | 47-53 | Ashen Reach | High Priest Vaurn | fear pulse, life drain, adds |
  | 45-60 raid | The Obsidian Citadel | 52-58 | Ashen Reach | The Obsidian Tyrant | shatter, enrage timer, adds, summon |
  | 45-60 raid | The Emberwyrm's Lair | 55-60 | Stormcrown Peaks | The Emberwyrm | fire breath, summon, enrage timer, adds |

  - Trash fills every room (two per type in the raids).
  - Each named rare (16) haunts one room in place of its trash type (25% of respawns).
  - The boss holds the deepest room with two guards.
  - Raid bosses have 30x health and 2.2x damage; group bosses have 7x and 1.5x.
- **Mob specials engine** (`zone_specials.cpp`; `MobSpecial` in `content.hpp`, data in `mobs.json`):
  - **procs** on a landed hit;
  - timed **casts** at the target (with line of sight);
  - **area attacks** around the target ("spread out") or around the mob ("step back");
  - **enrage** below a health fraction, and **enrage timers**;
  - **rampage** (a second foe in reach);
  - **summon** (a target beyond range is pulled back);
  - **add phases** at health thresholds (the adds join the fight, and leave if the boss resets);
  - shouted messages.
  - Mob spells (`content/mob_spells.json`) resolve through the spell engine (player resists, partial resists, DoTs, roots, snares, fear) and are validated (enemy-only, harmful effects only).
- **163 mob types** (`tools/gen_mobs.py`): 21 archetype families with 2-7 level tiers each, plus 30 dungeon trash types, 16 named rares and 10 bosses; the 25 Phase 2 mobs are kept for the test zone. Families carry biomes (where `gen_world.py` puts their camps), factions, behaviour presets, resists, trade loot and specials: venom and disease procs, webs and roots, frost, fire and lightning casts, fear wails, enrage and rampage.
- **Loot** (`tools/gen_items.py`): a table for every mob; new tables get coin by level and the family's trade loot (14 new trade items); bosses (35% each) and named rares (8%) carry their dungeon's six named items; named item levels follow the boss levels, and two new dungeons got named sets.
- `tools/gen_phase2_content.py` is superseded (refuses to run without `--force`); CI checks `gen_mobs.py`.
- **Tests:** 248 Catch2 tests and 421 client checks.

**Stubbed / placeholder (6c)**
- Mobs fight players and pets only. Adds and bosses do not cast heals or buffs on each other.
- Fear on players still holds them in place rather than making them run (from Phase 3).
- Dungeon art is tiles and braziers; bosses reuse their family's model, scaled up.

**Next:** 6d (factions, guards and the rest of the NPCs).

#### 6b: Hubs and outdoor zones (completed 2026-11-22)

**Verified end to end:**
- **Headless client e2e** (`--zone-walk`) through the regenerated Emberhold, Emberglade and Rattlebone Warren: 0 corrections, layout hashes match.
- **Client tests:** all 18 zones (3 hubs, 13 outdoor, the warren, the test zone) load from the bundled copy, build their visuals, and have their exits on open ground (349 checks).
- **Catch2:** the world test (hubs, bands, services, travel reach), a Druid's ringwalk leaving for another zone, and the 6a wire tests against the new hubs (a waystone hop now lands in Thornwood's real hub).

**Done**
- **Three hubs, hand-authored as Godot scenes** (exported to server collision with the 6a tool):
  - **Emberhold:** the walled forge city (east gate to the Emberglade, south docks).
  - **Thornwood:** the Heartwood and its ring of giant daughters inside a wall of pines (north gate to Briarwatch Wood, east river landing).
  - **Cragmouth:** a bowl of boulders closed by a gatehouse (south gate to the Cragfoot Hills), stone halls around the mountain forge.
  - Each has a banker, a trade board (auctioneer), a soulbinder, a waystone keeper (to the other two hubs, 50 copper), the general, alchemy, starter-armour and jewellery vendors, and trainers for every spell-using class its races can be (11 in Emberhold, 8 each in Thornwood and Cragmouth), all with their own names and placed by the scene's NPC markers. All three starting cities now begin in their hub.
- **Thirteen outdoor zones** (`tools/gen_world.py`), four bands:
  - 1-15: the Emberglade, Briarwatch Wood, the Cragfoot Hills (1-8, one outside each hub), the Saltmarsh Fens (8-15, where the three roads meet);
  - 15-30: Duskmere Shore, the Redrock Badlands, Hollowpine Forest, Gloomfen Bog;
  - 30-45: the Shattered Steppe, Frostveil Tundra, the Cinderspire Wastes;
  - 45-60: the Ashen Reach, the Stormcrown Peaks.
  - 13 biomes set their hills, prop scatter and colours (about 170 to 600 props a zone). Camps' levels rise across each zone's band, away from its lowest entrance; mobs are picked from the content by level (and biome, once 6d tags them).
  - Zone lines sit on the sides that link to neighbours, with an arrival entry 40 m in and roads kept clear between the entries and the middle. Every zone is reachable from every hub and back (validated).
- **Travel:**
  - ferries between the Emberhold docks, the Thornwood landing and the Duskmere landing (30 copper, 10 s boarding);
  - five Druid rings and four Wizard spires as travel points, with new **Ringwalk** (levels 12-46) and **Spirewalk** (levels 12-50) portal spells;
  - outposts in the Saltmarsh, Duskmere, Steppe and Ashen Reach with band vendors (the 15-30, 30-45 and 45-60 armour and weapon vendors live there) and a soulbinder.
- `content/travel.json` and the world NPC rows are generated; the hub clones are marked `"origin": "world"`, which the class and item generators keep.
- Validator: travel points count as teleport destinations; room selectors outside dungeons and NPCs placed by two scenes are rejected (also in the server).
- **CodeRabbit (6a scan):** 4 major findings, all fixed:
  - travel NPCs skipped the zone and position checks;
  - room selectors in a zone without a dungeon silently became (0, 0);
  - two scenes could place the same NPC;
  - a wall against the zone's edge could trap a body (the bounds clamp undid the push-out). Steps that would end inside geometry are now refused, and spawning finds the nearest free spot.
- **Tests:** 240 Catch2 tests and 349 client checks.

**Stubbed / placeholder (6b)**
- Camps use the 25 placeholder mobs until 6d generates the 150+ mob types (the camp levels are already right). Hubs have no guards yet (6d, with factions).
- Only the Rattlebone Warren exists as a dungeon (its cave mouth is in the Emberglade); 6c adds the rest and their entrances.
- Thornwood's and Cragmouth's look is placeholder primitives (Phase 9).

**Next:** 6c (the dungeons, their bosses and mechanics, and the named items bound to them).

#### 6a: Zone pipeline, procedural dungeons, zone lines and travel (completed 2026-11-21)

**Verified end to end:**
- **Headless client e2e** (`--zone-walk`, in `tools/scripts/client_e2e.sh`): a new Emberhold citizen starts in the Emberhold hub, walks out of the east gate into the Emberglade (about 650 m over hills, around trees), into the Rattlebone Warren's cave mouth, and back out through the warren's exit: four worlds, **0 movement corrections**, and every zone's layout hash matching the server's.
- **Over the wire** (Catch2, real `GameServer`): a waystone hop to another zone for a fee (refused without the coin), the zone change saved across a relog; zone lines both ways between Emberhold and the Emberglade; the empty Emberglade unloading while the hub stays.
- The Phase 1-5 client runs and bots still pass, now started in the test zone with `EMBERFALL_START_ZONE=proving_flats`.

**Done**
- **Zone format:** one file per zone in `content/zones/` (`hub`, `outdoor`, `dungeon`, `test`), with level band, terrain (base + round hills), props, obstacles (circles), walls (rotated boxes), entries, zone lines, spawns and a theme. `docs/ZONES.md` documents it.
- **Scene-to-server pipeline:** hubs are Godot scenes (`client/zones/<id>.tscn`) built from 29 primitive prop scenes (`tools/world_props.py`). `client/tools/export_zones.gd` exports their collision shapes, zone-line areas and markers (entries, NPC placements, spawns) to `content/zones/scenes/<id>.json`; the server merges the export, and NPC markers place `npcs.json` NPCs. CI checks the export, the prop scenes and the generated zones are fresh.
- **Shared geometry** (`game/world_geom.*`, mirrored by `client/scripts/world/zone_geom.gd`): terrain height, a deterministic 2D push-out against circles, rotated boxes and wall tiles, line of sight. Player movement (server and prediction) and mob movement collide; every entity stands on the terrain; spells check line of sight against walls.
- **Navigation:** a walkability grid rasterized from the collision at load (`game/nav_grid.*`), bounded A* with string pulling, a per-tick search budget; mobs path around walls when the straight way is blocked.
- **Procedural dungeons** (`game/dungeon_gen.*`, `client/scripts/world/dungeon_gen.gd`): seeded, integer-only rooms-in-sectors with corridors; exact room count; deepest room is the boss room; rooms place spawns, entries and the exit. A golden hash pins C++ and GDScript together; the seed travels in `EnterWorldResult`.
- **Layout hash:** the server hashes each zone's geometry and sends it; the client hashes its bundled copy and warns on a mismatch.
- **Zone instances:** zones load on demand and unload after `zone_idle_seconds` (default 300) when empty with no player corpses; hubs and the test zone persist; corpses of unloaded zones load their zone. Zone RNG streams are keyed by zone id.
- **Zone changes:** zone lines (1 s arrival grace), travel, portal/gate/translocation spells into another zone, and respawning at a bind elsewhere queue a transfer; the game server moves the character after the zones' tick, sends a new `EnterWorldResult` (`zone_change`), and saves. A summoned pet follows with its health; trades cancel; casts interrupt.
- **Travel NPCs** (`role` binder, teleporter, ferry; protocol `TravelOpen` / `TravelOptions` / `TravelChoose`): soulbinders set the bind point, waystone keepers teleport for a fee, ferries sail after a boarding wait (fare on departure, missed if you walk off). Teleport effects reach cities and travel points (`content/travel.json`) in any zone, so the Druid Stonepaths and the Wizard translocation now really travel.
- **First real zones** (pipeline samples; 6b-6c fill out the world): **Emberhold** (hand-authored hub scene: walls and gate, the Great Forge plaza, bank, temple, guild halls, homes, docks; a soulbinder and a waystone keeper), **the Emberglade** (outdoor, 1-8: 22 hills, ~280 props, 12 camps, a road, a cave mouth) and **the Rattlebone Warren** (dungeon, 2-10: 9 rooms, room spawns). Emberhold citizens now start in Emberhold.
- **Protocol v8:** `EnterWorldResult` gains zone kind, seed, layout hash, zone change and level band; travel messages.
- **Config:** `zone_idle_seconds`, `start_zone` (`EMBERFALL_ZONE_IDLE_SECONDS`, `EMBERFALL_START_ZONE`).
- **Client:** zones built from data (`zone_builder.gd`): themed sky and fog, terrain mesh, props, hub scenes, dungeon tiles (MultiMesh) with braziers, shimmering zone-line gateways; prediction through the zone's collision; a zone banner with the level band; the travel window (T on a soulbinder, waystone keeper or ferry master); the world swaps cleanly on a zone change.
- **Tools:** `tools/gen_world.py` (zones and world NPCs), `tools/world_props.py`, `tools/zone_format.py`, `tools/npc_order.py` (one ordering for the three generators that write `npcs.json`), `tools/world_check.py` (zone files, lines, entries, rooms, travel and reachability in `validate_content.py`), `sync_client_content.py` writes `client/data/zones/*.json` and `client/data/world.json` (the world map index), autotest `--zone-walk`.
- **Tests:** 236 Catch2 tests (14 new) and 259 client checks (34 new): terrain, every collision shape, sliding along walls, line of sight, dungeon determinism / room count / connectivity / golden hash, nav paths and enclosed goals, world content checks (lines, entries, rooms, travel, reachability), zone lines (and the arrival grace), mobs pathing around a wall, soulbinder / waystone / ferry, pets following, and the wire tests above; client: generator and collision parity with the server, every bundled zone loading and building.

**Stubbed / placeholder (6a)**
- Thornwood and Cragmouth cities still start in the test zone (their hubs come in 6b); Emberhold's trainers, vendors and banker still stand in the test zone (6d moves NPCs to hubs).
- No ferries or travel points are placed yet (6b); Wizard spires and Druid rings come with the outdoor zones.
- Dungeon mobs are the old placeholder mobs; bosses, named rares and mechanics are 6c.
- Cross-zone resurrection is not supported (the owner must be in the corpse's zone).

**Next:** 6b (the three hubs and 12+ outdoor zones across the four bands).

### Phase 5: Items, equipment, vendors, bank, and trade (completed 2026-11-21)

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v7; the live database migrated to schema v6 in place), running the items scenario: a Highlander Warrior bought and equipped Dented Breastplate (AC 7 → 8) and a Vaelith Wizard a Homespun Robe (AC 8 → 9, holding the staff from her kit); both sold food, drank, banked and withdrew an item and coin, and listed and cancelled an auction. Screenshots: `client/build/verify5`.
- **Headless client e2e** (`tools/scripts/client_e2e.sh`): all earlier runs, plus two shoppers (a Warrior in plate, a Wizard in cloth) driving the real item UI code: the starting kit is worn; at the novice outfitter they buy a chest piece, equip it (AC rises: Warrior 7 → 8), sell two rations, drink water (a Refreshed buff); at the banker they bank rations and 5 copper and take both back; at the auctioneer they list the rations (fee 10 copper) and cancel the listing.
- **Over the wire** (Catch2 with the real `GameServer` and SQLite): a starting kit worn and packed; a trade saved for both sides at once (both log back in with the swapped sword and coin); a disconnect mid-trade moving nothing; the auction board (list with a fee, search, a stale price refused, buy, collect); a hostile client sending 1,500 random item intents.
- **Live test:** the bots (fighting, dying and recovering the corpse, training) pass against the release server.
- **Balance simulator:** all 14 classes, now wearing level-appropriate gear, win every fight at nine levels from 1 to 60, with no class beyond 3x the median time to kill.

**Done**
- **Item model** (`game/items.hpp`, `content/items.json`): 21 worn slots, an 8-slot pack, a 16-slot bank, bags (up to 16 slots) in pack and bank, one `u16` slot code for all of them. Items carry slots, armor weight, weapon type, damage/delay/range, AC, HP/mana/endurance, stats, resists, haste, procs and click effects, rarity, magic/lore/no-trade, class/race/level restrictions, stack sizes and bag sizes.
- **Item effects** (`content/item_effects.json`): weapon procs, potions, food and drink, bandages (4 s cast), elixirs, resolved by the spell engine.
- **Itemization** (`tools/gen_items.py`, `--check` in CI): 708 items besides spell scrolls: cloth/leather/chain/plate across 9 slots and 8 tiers each, 16 weapon kinds across 12 tiers, shields, mystic and martial jewelry and cloaks, arrows, 31 consumables, 7 bags, and 48 named dungeon placeholders (8 Phase 6 dungeons, raid ones epic and no-trade). Formulas are in DECISIONS.md.
- **Server:**
  - equipping with restrictions (slot, class, race, level, armor weight, weapon training, off-hand needs dual wield, two-handers need the off hand empty), stats recalculated from base + gear + buffs, weapon damage from the worn weapon (class `weapon_mult`), the off hand from its own weapon, item haste (non-stacking with spell haste), weapon procs, click effects, bows required for archery shots;
  - validated, transactional item moves (equip, swap, split, merge, bags with their contents, bank only at a banker), destroy, use (consume, scribe, auto-equip);
  - lore and no-trade enforcement;
  - **loot:** weighted pools in every mob's table (level-appropriate gear, consumables, scrolls) plus the old drops; the loot party (everyone who damaged the mob until groups in Phase 7) splits coin and shares items by **need/greed** rolls or **round robin**; player corpses hold worn gear, pack and bags, and looting your own corpse puts gear back on;
  - **vendors** by level band with markup by charisma, limited stock that restocks, and buyback; **consumables**;
  - **bank** (items, bags and coin); **secure trade** (invite, offer up to 8 stacks and coin, both accept, atomic swap, cancel on any change of circumstance, items locked while trading); **auction board** (global, persisted: list with a fee, search by name/kind/level with paging, buy at the seen price, cancel, collect proceeds and unsold items, 48 h expiry);
  - **anti-dupe:** single-character moves are batched once a second; trades, auctions and corpse loot are single transactions covering every character involved.
- **Protocol v7:** `ItemStack.slot` is a u16 slot code; new `MoveItem`, `DestroyItem`, `UseItem`, `CharacterSheet`, `EntityEquipment`, vendor, bank, trade, auction and loot-roll messages; `EntitySpawn` carries held items.
- **Database schema v6:** slot codes (old 16-slot packs become pack + bank), `bank_coin`, the `auctions` table, starting kits stored at character creation.
- **Client:**
  - **Inventory window (I):** paper doll of 21 slots, a stats panel (from `CharacterSheet`), pack and one row per open bag, coin, and a destroy square (with confirmation); drag-and-drop everywhere (Shift-drag splits one off a stack); right-click uses, equips, takes off, sells (vendor open) or moves across (bank open);
  - **tooltips** with type, stats, effects, restrictions, value and a comparison against what is worn (damage per second for weapons);
  - **vendor, bank, trade (with the invite prompt), auction board and loot windows**, and **need/greed/pass** prompts;
  - **T** talks to the targeted NPC by role (trainer, vendor, banker, auctioneer) or offers a trade to a targeted player; `/trade [name]`, `/lootrule ng|rr`, `/inv`;
  - **equipment visuals:** players hold what they wear (primary, off hand, a bow on the back), for remote players and our own.
  - `client/data/items.json` is the synced display copy (`tools/sync_client_content.py`).
- **Balance simulator** wears `recommended_kit` by default (`--gearless` for the Phase 4 comparison).
- **Tests:** 222 Catch2 tests (32 new) and 225 client checks (47 new).
  - pure rules: wear restrictions, gear totals and haste, weapon damage by class, price no-arbitrage at every charisma, every move rule, give_item placement, weighted loot distributions and caps, every shipped table dropping level-appropriate gear, need/greed and round robin, coin splits, atomic trades, lore, the auction board;
  - zone: a trade **fuzz test** (60 rounds of random requests, offers, coin, accepts, cancels, moves and disconnects, checking item and coin conservation after every step), equipping and the character sheet, consumables and bandages, procs, archery needing a bow, death and corpse recovery re-equipping, group loot (coin split, need/greed with a downgraded need, round robin), vendors (stock, restock, buyback, no selling worn gear), the bank;
  - database: v5 → v6 migration, auction transactions rolling back whole;
  - wire: starting kit, trade persistence and disconnect, a trade accept for an offer that changed in flight, auction flow (including a slot that no longer holds the item), hostile item intents, and a trade whose save fails three times (retried, both sides stored together, even after one logs out).
- **Build:** warning-free on GCC 16 (debug, release), GCC 13 (Docker, which also runs the tests), Clang 22 and ASan/UBSan; ASan clean; clang-tidy clean on the new files. The Windows export is 117.6 MB.
- **CodeRabbit:** three scans found 30 issues (16 major, 14 minor), all fixed. The ones that mattered most:
  - a database error after a trade, auction or batched save could store one side without the other: failed batches are now carried by the DB worker into the next transaction (newest copy wins), a character with an unsaved save cannot be loaded, and corpse writes wait behind them;
  - a trade accept could apply to an offer that changed in flight (accepts now name the offer revision);
  - a staged auction square could list a different item (the server checks the item id);
  - a need/greed winner's reservation lapsed like a round-robin one;
  - a partially read auction board was opened as complete;
  - items with no slot at login were dropped (entry is now refused instead);
  - "2p 5gg" was read as 2 platinum; buyback rows mixed stacks; the character sheet missed buff-only changes;
  - the offline validator now matches the server on item effects, stock counts, pool counts, kit sizes and integer fields.

**Stubbed / placeholder**
- **Groups:** the loot party is "everyone who damaged the mob" until Phase 7 groups replace it; the loot rule is the top damager's.
- **Named dungeon items** exist but drop nowhere until Phase 6 binds them to bosses.
- **Item art:** items are coloured initials in the UI (icons in Phase 9); only seven weapon/shield meshes exist, so most weapons show the nearest basic mesh and armor is not drawn on the body.
- **Arrows** add damage but are not used up; **tradeskill** components are trade loot only (tradeskills are a stretch goal).
- **Character select** previews still show the class's training gear, not the character's real equipment.

**Known gaps**
- Item STA does not raise HP through the stamina formula (items carry explicit HP instead).
- No hotbar slots for items yet (use them from the inventory).
- The balance spread widened with gear (melee gained more than casters); Phase 8 retunes 1-60.

**Next**
- Phase 6 (world content): zone pipeline, hubs and dungeons; bind the named items to dungeon bosses; vendors per hub instead of the test plain.

### Phase 4: All 14 classes, pets, skills, and threat (completed 2026-11-21)

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v6; the live database migrated to schema v5 in place).
  - **Tinkerling Magician (new race):** summoned its ember servant. The pet window showed it, the pet answered "Attacking an ember rat, master", and it killed 3 rats (credited to the Magician), with 0 movement corrections.
  - **Vaelith Bard:** sang Chant of Embers at its targets in melee. The song indicator showed the song with its pulse bar; it killed 2 rats, and singing improved.
  - Screenshots: `client/build/verify4`.
- **Headless client e2e:** 8 runs pass:
  - the Phase 1-3 runs;
  - a Tinkerling Magician (`--pet 2`): summon, send the pet in, kill and loot;
  - a Bard (`--bard 2`): kill in melee while singing.
- **Live test:** the bots pass under release and ASan/UBSan.
- **Balance simulator:** all 14 classes win every fight at nine levels from 1 to 60. No class is beyond 3x the median time to kill (table and notes in DECISIONS.md).

**Done**
- **All 14 classes, 25-39 abilities each:**
  - `tools/gen_classes.py` generates 128 abilities and 331 spells and songs, from tiered templates sized against the server's own curves, with hand-written signature abilities.
  - It also writes 8 pet templates, 331 scrolls, 3 summoned items, 11 trainers (one per spell-using class) and a practice lockbox. CI runs it with `--check`.
  - Every class has an ability at level 1 and something new at least every 3 levels to 60. `rules.json "content"` turns this on in both validators, together with the 25-per-class minimum.
  - Hybrids get spells from level 9.
  - The Warrior grew from 16 to 30 abilities: pull, snare, an armor sunder, a second stun rank, a hate discipline, Worldbreaker.
  - Generated names were audited so none echoes another game's spells.
- **Engine features** (`zone_pets.cpp`, `zone_classes.cpp`; pure rules in `class_rules.cpp`):
  - **Pets:** a pet is a mob with an owner:
    - summon at the spell's level;
    - follow, guard, attack, back off, dismiss;
    - defends its owner, tanks (mobs hate pets), and earns its owner loot and XP;
    - heal and buff spells on pets (`pet` target type; friendly and group spells too);
    - fire servants carry a damage shield, and earth servants extra threat;
    - variants: elemental (4), undead (Necromancer and Shadow Knight), spirit wolf (Shaman), animated armor (Enchanter).
  - **Charm:** a charmed mob becomes a pet. The charm rolls to break every 6 s (by level difference and CHA) and runs out at its duration; either way the mob turns on its charmer. Level caps and `charm_immune` mobs are respected.
  - **Bard song engine:**
    - songs pulse every 6 s and cost nothing;
    - twisting works because song effects last three pulses;
    - moving keeps a song going, while a stun, mez, fear, death or feign stops it;
    - pressing the same gem again stops the song.
  - **Positionals:** Backstab and Garrote need the user within a 120° arc behind the target.
  - **Stealth and aggro drops:**
    - Sneak (slower, halves the aggro radius), Hide (fails when watched; moving ends it unless sneaking) and invisibility buffs;
    - feign death clears every hate list, and mobs walk home without healing;
    - `threat_drop` effects: Evade, Feint, Fading Memory, and Vanish in Smoke (an area version).
  - **Damage and conversion effects:** damage shields (players and pets), lifetaps, drains (DoTs that heal the caster), and HP-to-mana.
  - **Summoned items:** they expire (persisted time left) and dissolve on death.
  - **Illusions:** of a race or a creature model, seen by everyone, including the caster's own client.
  - **Tracking, pick lock:** tracking lists creatures with level, con and compass direction; pick lock works on lockboxes.
- **Skills and passives:** dual wield (off-hand swings, Monk fists from level 1), enhanced damage, backstab, sneak, hide, pick lock, tracking, feign death, singing, archery, throwing and mend, all with per-class unlock levels and caps, improving with use. Every class has armor and weapon proficiency tables, enforced on equip in Phase 5.
- **Threat tuning:**
  - class `threat_mult`: Warrior 1.5, Paladin and Shadow Knight 1.35, casters and Rogue 0.9;
  - `threat_pct` hate disciplines for the three tanks;
  - a tested Warrior keeps a mob off an unlimited-mana Wizard for over 80% of the fight.
- **Races:** eight original races, with stats, class restrictions and starting cities: Highlander, Vaelith, Deepkin, Duskborn, Hollowfolk, Frostmane, Morrok and Tinkerling. Looks for both genders are in `appearance.json`, and character creation lists them all.
- **Protocol v6:**
  - `PetCommand`, `PetStatus` and `SongUpdate`;
  - `flags2` on entity state, spawns and player stats (feigned, hidden, sneaking, singing, pet, charmed);
  - `EntitySpawn.model` (illusions) and `owner_id` (pets);
  - `ItemStack.expires_ms`, `SpellInfo.song`.
- **Database:** schema v5 adds `character_items.seconds_left`, with an in-place migration.
- **Client:**
  - **Pet window:** health, mode, activity and charm time, with Attack, Follow, Guard, Back and Dismiss buttons.
  - **Song indicator** with a pulse bar.
  - **Skills window (J):** values and caps, plus commands you can drag to the hotbar.
  - **Hotbar:** `cmd:` slots (sit, consider, loot, pet orders, stop). Non-Warriors get a sensible default bar: attack, active abilities, then gems.
  - **Slash commands:** `/pet ORDER`, `/stopsong`, `/skills`.
  - **Entities:**
    - feigning players lie down, and hidden ones are drawn faintly;
    - illusions rebuild the body, for remote players and for our own;
    - pets have a friendly name colour and are skipped by Tab and the autotest's mob picking;
    - combat log lines for every new event.
- **Balance simulator v1** (`tools/balance_sim`, `emberfall-balance-sim`): every class fights even-level mobs back to back in a real headless zone, playing a kit built from content alone. It reports time to kill, win rate, health left and mana efficiency, flags outliers, and runs in CI.
- **Tests:** 190 Catch2 tests, 26 of them new, plus 179 client checks (41 new).
  - **Catch2:**
    - pure rules: behind arcs, dual wield, enhanced damage, stealth and charm chances, threat scaling and drops, compass directions, proficiencies, pet templates;
    - pets: summon, follow, attack, tank and kill credit, pet death and owner exit, heals and buffs on pets, dropping leashing mobs, guard recall;
    - songs: pulses, twisting, silencing, a detrimental song, toggling off;
    - backstab position;
    - charm: forced break and turning hostile, level caps and immunity;
    - aggro: feign death (no reset, no proximity aggro), evade, and tank-vs-Wizard threat;
    - hide and sneak; damage shields, lifetaps and drains;
    - summoned items: expiry, dissolving on death, the save round trip;
    - illusions; tracking and pick lock; monk dual wield;
    - every race and class entering the world with a level-1 ability;
    - the schema v4 → v5 migration; a balance smoke test.
  - **Client:** the class UI (pet window, song indicator, skills and commands), entity `flags2` and illusions, and every race having a real model.
- **Build:** warning-free on GCC 16, GCC 13 (Docker) and Clang 22; ASan/UBSan clean; clang-tidy clean on the changed files. The Windows export is 117.2 MB.

- **CodeRabbit:** two scans found 12 issues (2 major, 10 minor), all fixed. The ones that mattered most:
  - a pet kept re-targeting a mob that was leashing home;
  - an item whose definition had left the content would have been destroyed on death instead of staying on the corpse;
  - a guarding pet never came back when its owner left leash range;
  - a charmed mob ignored a fear already on it;
  - toggling a song off during the global cooldown failed twice;
  - lockboxes skipped the zone-position check;
  - an empty class passed the Python validator;
  - the balance sim's median was wrong for even counts, and an empty `--levels` ran nothing;
  - three tests could pass vacuously or crash.

**Stubbed / placeholder**
- **Pets:**
  - Pets are not saved: a summoned pet vanishes on logout, and a charm breaks.
  - Pets do not follow their owner across zones (no zone lines until Phase 6).
  - Pets have no spells or special abilities of their own.
- **Item effects:** summoned food, water and the conjured blade cannot be used or equipped yet (consumables and equipment in Phase 5).
- **Lockboxes:** the practice lockbox holds nothing (chests and doors in Phase 6).
- **Illusions:** cosmetic only; faction effects come with factions in Phase 6.
- **Group targets:** group spells reach the caster and its pet only (groups in Phase 7).
- **Art:** pets reuse existing models (imp, wraith, golem, bird placeholder, skeleton, wolf, hollow knight).

**Known gaps**
- No facing check for melee or non-positional abilities (only `position: behind` abilities check facing).
- The balance sim is solo-only, even-level, gearless, and has no burst metric. Its notes (strong Warrior and Monk strikes, slow Paladin, Magician ahead of Wizard) are in DECISIONS.md for tuning in Phase 5-8.
- Feign death on failure leaves the player standing; there is no "fallen but not fooled" state.
- Name tags still render at a fixed world size, so a pet next to the camera shows a large name (from Phase 3).

**Next**
- Phase 5 (items, equipment, vendors, bank, trade):
  - enforces the armor and weapon proficiency tables on equip;
  - makes summoned food, water and weapons usable;
  - fills held-item slots from real equipment;
  - retunes melee against gear using the balance simulator.

### Phase 3.5: Character model and animation pipeline (completed 2026-11-21)

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v5; the live database migrated to schema v4 in place).
  - **Vaelith female Wizard:** the create screen's preview showed her hat, robe, staff and slender build. In the world she bought, scribed and memorized Frost Needle, then killed two rats with spells, with the cast animation playing.
  - **Deepkin male Warrior:** a short, broad body with sword and shield. He ran, swung (attack1/attack2), flinched, killed and looted three rats, leveled, died to a thornback boar (a quadruped with tusks and spines), respawned and recovered his corpse.
  - **Third-party models:** a Highlander male Wizard and a Deepkin female Cleric, run after the Quaternius packs went in, trained at Sister Maelis and Archmage Torvan, who are now the textured Quaternius Cleric and Wizard.
  - Every run enforced `--expect-models` and `--expect-anims`.
  - Screenshots: `client/build/verify35`.
- **Headless client e2e:** two walkers (one female, one male), the fighter, the Wizard and the Cleric. All load real models, and between them play idle, walk, run, combat_idle, attack1, attack2, cast, hit and death with 0 missing clips. Beetles stay placeholders by design.
- **Live test (ASan):** the existing bots, plus one created with gender 1.
- **Client tests:** 138 headless checks pass.

**Done**
- **Model pipeline (content-driven):**
  - **`content/models.json`:**
    - **Models:** scene path, scale, nameplate height, collision radius, yaw offset, placeholder shape/colour, swappable part groups, default parts, tints and attachment points;
    - **Animation maps:** standard state → clip, with ground speeds;
    - **Held-item visuals.**
  - **`content/appearance.json`:** race + gender → model, scale (per-axis allowed), tints and parts; class → tints, parts and default held items.
  - **Wiring:** items gain `visual`, and mobs and NPCs name a model id.
  - **Client copy:** the client reads `client/data/visuals.json` (`tools/sync_client_content.py`).
  - **Fallbacks:** a missing or broken model falls back to its placeholder; a missing item mesh or attachment point is skipped with a warning.
- **Client:**
  - **CharacterVisual** (`client/scripts/world/character_visual.gd`) loads the GLB and applies tints (shared tinted materials), part visibility, `BoneAttachment3D` held items, the animation map and loop modes.
  - **Placeholders** get procedural motion: bob, lunge, flinch, lie down, squat.
  - **Animation state machine** (`anim_state.gd`, pure, tested), driven only by server state:
    - snapshot or predicted speed;
    - the dead, sitting, stunned/mezzed and in-combat flags;
    - `CastStart`/`CastEnd`;
    - combat events (swings, abilities, hit reactions, instant-spell releases);
    - deaths: a fresh corpse plays its death, one that was already dead shows the final pose.
  - **Fallback chains** for missing clips (run → walk → idle, attack2 ↔ attack1, cast → idle, missing hit skipped, missing death/sit procedural).
  - **Foot sliding:** walk and run play at actual speed / (ground speed × model size), clamped to 0.4-2.5x. Backwards walking reverses walk when there is no walk_back clip.
  - **Facing:** smooth turning on the visual only (root yaw, camera and prediction untouched). While standing, an entity faces its cast target, or for 4 s its swing target; while moving it faces its movement.
  - **Picking:** click picking scales with collision radius, and nameplates sit at the model's height.
  - **Character screens:** turning previews on character create (follows race, gender and class, with a swing or cast flourish) and select. Character creation has a Gender choice.
- **Gender:**
  - `CharCreate`/`CharSummary`/`EntitySpawn.gender` (protocol v5), validated server-side;
  - `characters.gender` (schema v4, in-place migration);
  - fresh player corpses carry the owner's race, class and gender.
- **Server:**
  - loads and validates `models.json` and `appearance.json` (fail fast: unknown model ids, missing race/gender looks, held items in the wrong slot or without a visual);
  - mob melee reach now uses the model's collision radius (`melee_range + collision_radius × scale`).
- **Third-party art (Quaternius, CC0, from OpenGameArt):** scripted downloads from the official hosts were blocked, so the packs came from OpenGameArt mirrors, downloaded by the user into the git-ignored `downloads/`. Two are in the build:
  - **LowPoly RPG Characters**, converted from FBX with `tools/convert_to_glb.sh` (Godot's importer plus `GLTFDocument`, textures bound and embedded). They are the Cleric and Wizard trainers, brigands (Rogue), brigand lookouts (Ranger), ashen cultists (Wizard, darkened) and hollow knights (Warrior, pale), with four animation maps.
  - **The Universal Animation Library mannequin** (46 clips, every standard state including sit, swim and fall) for golems: a model swap done purely in data.

  UAL 2 is downloaded but unused (different rig, needs retargeting). Measured ground speeds and heights come from `tools/godot_convert/measure.gd`. Godot's import renaming (`_Loop` suffixes dropped from clips, dots in FBX bone names) is handled by the client, the validator and `inspect_model.py`.
- **Generated starter art:** `tools/gen_models.py` generates original CC0 GLB models (deterministic, committed, `--check` in CI):
  - female and male humanoids on one 18-bone rig, with 10 clips, 13 swappable parts and 8 tintable materials;
  - a quadruped with 6 clips;
  - six held items.

  From these, model ids cover players, generic NPCs, skeletons, giants, imps, treants, wraiths, rats, wolves, boars and drakes. Beetle, snake, spider, bird and horror keep placeholder shapes. Every class shows class colours, headgear, robes, capes and pauldrons, and holds placeholder "training" gear (sword and shield, mace, staff, daggers, a bow on the back).
- **Tools and checks:**
  - `tools/validate_content.py` opens every GLB and checks clip names, bones, part nodes and tinted materials;
  - `tools/inspect_model.py` lists a model's clips, bones, meshes and materials (and Godot's renamed clip names);
  - `tools/convert_to_glb.sh` converts FBX/OBJ packs to GLB with Godot, and `tools/godot_convert/measure.gd` measures heights and ground speeds;
  - CI checks that generated models and the client copy are fresh, and runs the client tests;
  - the autotest gained `--gender`, `--expect-models`, `--expect-anims`, `--preview-screenshot` and `--camera-distance`, and `client_e2e.sh` enforces models and animation states;
  - the bot gained `--gender`.
- **Docs:** `docs/MODELS.md` ("How to add a character or mob model": format, folders, model entries, animation maps and fallbacks, ground speed, appearance layering, attachments, worked example for a third-party pack); `ASSETS.md`; BUILD.md updates.
- **Tests:** 164 Catch2 tests and 138 client checks.
  - New Catch2 tests (6): model and appearance validation, models and appearance files required on disk (and a non-empty catalog), shipped model coverage and reach, gender storage, schema v3 → v4 migration, gender over the wire. The death test also checks corpse appearance.
  - Build: warning-free on GCC 16, GCC 13 (Docker) and Clang 22; ASan/UBSan clean; clang-tidy clean on the changed files.
  - The Windows export grew from 109.5 MB to 117.2 MB with the third-party models.
- **CodeRabbit:** three scans found 40 issues (all minor), all fixed. The ones that mattered most:
  - the generated walk and run lifted each knee during the wrong half of the stride (planted feet slid), and the quadruped's paws had the same problem;
  - a repeated swing of the same clip did not rewind;
  - a stun did not cut an active swing short;
  - placeholder lunges did not restart for a new swing;
  - the quadruped's attack lunge (a body translation) was dropped from the exported clips;
  - an expanded pick radius could steal a click from a smaller body;
  - the cast-history table grew without bound;
  - an empty model catalog on disk skipped every model check;
  - `--expect-models` passed with every mob a placeholder;
  - the validator could crash on malformed JSON types, accepted truncated GLBs and missing external buffers, and let `res://` paths escape the client folder;
  - the FBX converter could let two sources' textures or outputs collide, and dropped import errors;
  - the gait measurement ignored root scale and mixed sideways foot motion into ground speed;
  - the content generator would have deleted the hand-authored class gear.

**Stubbed / placeholder**
- **Art:** player characters use the simple box-built generated rig (the RPG Characters are fixed class looks with no race or gender variants). Five mob families are placeholder shapes. Retargeting the UAL 2 clips is for Phase 9.
- **Equipment display:** classes hold fixed placeholder "training" items until equipment exists (Phase 5). Held meshes inherit non-uniform race scale (a Deepkin's sword is a little squat).
- **Animation states:** `swim` and `fall` exist in the state list but nothing drives them yet (no water or falling). There is no strafe clip; strafing plays run.
- **Corpses:** player corpses restored from the database after a restart use the generic humanoid model.

**Known gaps**
- No AnimationTree or upper/lower-body layering: a swing while moving plays the swing on the whole body.
- Facing toward a target is visual only; there is still no server-side facing check for melee (from Phase 2).
- Name tags still render at a fixed world size (from Phase 3).
- Small, fast mobs (rats) hit the 2.5x playback clamp, so their feet slide a little.

**Next**
- Phase 4 (all 14 classes, pets, songs, positionals) can give pets and illusions model ids, and Phase 5 fills held-item slots from real equipment through the same item visuals.

### Phase 3: Spell engine with Cleric and Wizard (completed 2026-11-21)

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v4; the live database migrated to schema v3 in place).
  - A new Wizard went through the whole loop:
    - found its trainer (after the Cleric trainer turned it away), bought Frost Needle for 7 copper, and scribed it from the pack;
    - cast Ward of Glass on itself (shown in the buff tray) and memorized Frost Needle into gem 2;
    - killed and looted 3 rats with Ember Dart and Frost Needle, with the snare showing under the target frame.
  - A new Cleric did the same with Holy Rebuke and Blessed Ward, healing in melee and keeping concentration through hits.
  - Network: at most 1 movement correction, about 19.8-19.9 inputs/s.
  - Screenshots: `client/build/verify13`.
- **Live test:** over UDP, a Wizard bot and a Cleric bot each find their trainer, buy and scribe a scroll, buff themselves, memorize the new spell, and kill and loot 3 mobs with spells. Also under ASan. The Phase 2 fighter and death bots still pass.
- **Headless client e2e:** the real Godot client runs the same trainer, scribe, buff, memorize, and spell-fight flow for a Wizard and a Cleric, alongside the Phase 2 runs.

**Done**
- **Spell engine** (`server/src/game/spells.*` pure and unit-tested, applied in `zone_spells.cpp`; every tunable in `content/rules.json` "spells"):
  - **Casting:** a cast state machine with cast times. Mana is checked at the start and paid on completion, then recast timers and the GCD start.
  - **Interrupts:** moving, being stunned, mesmerized, or feared, or failing a concentration check when hit (the channeling skill improves it).
  - **Targeting:** range checked at the start and again, with slack, on landing; line of sight against zone obstacles. Target types: self, friendly, group (stub), enemy, area (around the caster or a target), and corpse.
  - **Resists:** magic, fire, cold, poison, and disease, with partial resists on damage; mob resists come from level and body type.
- **Effects:** direct damage, direct heal, heal %, DoT, HoT, stat buff, stat debuff, slow, snare, root, stun, mez, fear, dispel, resurrect, teleport, gather mana, plus the Phase 2 threat, taunt, and area threat. Abilities and spells share one resolver.
- **Buffs and debuffs:**
  - Each spell becomes one effect on its target. There are 15 buff slots and 10 debuff slots.
  - Stacking: rank overwrites within a stack group, lower ranks are blocked with a message, and DoTs stack per caster. Durations tick on the server tick.
  - Mez breaks on damage; root sometimes breaks on direct hits. Rooted mobs don't leash; feared mobs run and don't swing.
  - Haste and slows change attack delay. Snares change movement speed (players' too, through `PlayerStats.run_speed`), and AC, HP, mana, stat, and resist modifiers apply.
  - `BuffList` updates go to the player and to anyone targeting the entity.
- **Spells:** `tools/gen_spells.py` generates 30 Cleric and 30 Wizard spells across 1-60 in tiered ranks (output committed in `content/spells.json`). Highlights:
  - Wizard: fire, cold-and-snare, and arcane nukes, point-blank and targeted area spells, roots, a DoT, a resist/armor debuff, self wards, gate, evacuate, a city translocation, and gather mana.
  - Cleric: seven heals, three HoTs, five armor/HP blessings, undead-only nukes, smites, level-capped stuns, three resurrections (50/75/96% XP), fear vs undead, and a cure.
- **Learning spells:**
  - Every spell has a scroll item, scribed from the pack into a 120-spell spellbook (class and level checked).
  - Spells are memorized into gems: 2 at level 1, 8 at level 45, never in combat, with a 2 s lock.
  - Trainers (new NPC entities in `content/npcs.json`) sell every scroll of their class.
  - New casters start with two spells memorized and 25 copper; the third level-1 spell is bought from the trainer.
- **Threat and healer aggro:** heals (including HoT ticks) give the healer threat on every mob already fighting the healed player within 60 m, half the healing split between them. Buffing someone in a fight adds a flat amount per mob. Overhealing and uninvolved mobs generate nothing.
- **Cleric extras:**
  - Resurrection targets a player corpse. The owner gets a 60 s offer to accept or decline, then returns to the corpse alive with part of the lost XP; it works once per corpse, and the corpse and character save together.
  - Undead-only nukes (body types on mobs), and stuns with level caps.
- **Wizard extras:** evacuate (whatever was chasing you loses your trail), gate to bind, and a city translocation. All three work only within the current zone until zone lines arrive. Gather mana restores 60% or 100%.
- **Persistence:** schema v3 saves the spellbook, gems, beneficial buffs (with time left), and all recast timers. This closes the Phase 2 gap where long cooldowns reset on relog.
- **Client:**
  - **Bars:** a gem bar above the hotbar with memorize/recast/GCD sweeps, and a cast bar that turns red with the reason when interrupted.
  - **Effect trays:** your buffs at top right and your target's effects under the target frame, with timers, blinking at the end, and tooltips.
  - **Windows:** a spellbook (B) with drag-to-gem and Mem buttons, a trainer window (T), Scribe buttons in the pack, a resurrection prompt, and a group window stub (P).
  - **Effects and log:** placeholder particles while casting and when spells land, colored by school, plus combat-log lines for every new event.
  - **Hotbar:** it can bind gems (`gem:N`; casters get gems on keys 2-9 by default).
  - **Commands:** /cast N, /stopcast, /book. Esc stops a cast.
  - **Autotest:** `--caster N`.
- **Content validation** (server and `tools/validate_content.py`):
  - every spell is reachable at its level (a starter grant, a trainer, or a loot drop);
  - every class with abilities covers each level band;
  - polarity rules, scroll and trainer references, gem tables, and teleport destinations are checked.
- **Tools:** `emberfall-bot --caster`, plus Wizard and Cleric runs in `live_test.sh` and `client_e2e.sh`.
- **Tests:** 158 Catch2 tests, 42 of them new:
  - pure rules: resist math and rolls, partial resists, concentration, the cast state machine, stacking and slot limits, dispel order, per-line expiry and timers, line of sight, gems, magnitudes;
  - zone: casting and GCD, movement and hit interrupts, DoT and HoT ticks, buff stat changes and fades, mez break, root, snare, slow, fear, debuffs and cures, healer aggro, resurrection (accept, decline, expiry, stale replies), trainers, scribing and memorizing, targeting rules, level caps and immunities, evacuate/gate/gather mana, area caps, saved buffs and timers, and a spell defined only in JSON;
  - a Monte Carlo check that a Cleric and a Wizard solo even-level mobs at nine levels from 1 to 60;
  - content validation for the new rules, database schema v3 and its migration, and a full Wizard session over the wire.
- **Build:** warning-free on GCC 16, GCC 13 (Docker) and Clang 22; ASan/UBSan clean; clang-tidy 0 findings.
- **CodeRabbit:** five scans found 46 issues (6 major, 40 minor), all fixed. The ones that mattered most:
  - a timed heal whose target left landed on the caster and still cost mana;
  - a resurrection reply could answer a newer offer (offers now carry an id);
  - one capped line (a root above its level cap) threw away a whole spell's lasting part;
  - a spell's lasting lines shared one expiry and one tick timer (each line now has its own);
  - breaking a root also removed the DoT in the same spell;
  - default gem unlock levels broke content with a lower level cap;
  - a scroll in an unused loot table counted as a way to learn a spell;
  - the test bots and autotest could pass without the bought spell ever hitting, or scribe the wrong scroll.

**Stubbed / placeholder**
- **Group targets:** group spells land on the caster only, and the group window is a stub (groups arrive in Phase 7).
- **Travel:** teleports and evacuate work within the current zone only (zone lines in Phase 6). Fear on players holds them in place rather than making them run (pathing).
- **Mob spells:** mobs don't cast spells yet. `Zone::force_effect` is the hook for mob spells and GM commands, and the tests use it.
- **Line of sight:** checks against zone obstacles, but the test zone has none (zone geometry in Phase 6).
- **Scrolls:** trainers sell every rank. Scroll drops from loot tables and vendors are Phase 5/6 content.
- **Art:** placeholder spell effects and casting particles.

**Known gaps**
- The other 11 classes have no abilities or spells yet (Phase 4). Content validation lists them and skips their level-band check.
- The balance check is against an even-con generic mob with trained skills and no gear. Real tuning waits for gear (Phase 5) and dungeons (Phase 6).
- Spell school skills (evocation and the like) and fizzles are not modeled; only channeling and meditate improve with use.
- Game traffic after login is still unencrypted (see Phase 1).
- No facing check for melee (from Phase 2).
- Name tags render at a fixed world size, so an NPC standing right next to the camera (the trainers near the start) shows a very large name.

**Next**
- Phase 4 (all 14 classes, pets, songs, positionals) builds on the shared ability/spell resolver, `force_effect`, the stack groups, and the level-band validation.

### Phase 2: Core combat and leveling, Warrior only (completed 2026-11-20)

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v3). The server migrated the live Phase 1 database to schema v2 in place; Phase 1 characters kept their positions.
- **Fight and death run:** a new Warrior:
  - targeted, chased, auto-attacked and Rising-Kicked level-1 mobs, then looted each corpse through the loot window;
  - leveled to 2, then walked to a level-9 camp and died;
  - pressed *Respawn at bind point* in the death dialog, walked about 180 m back, and recovered its corpse with its pack and coin.
- **Network:** 0 movement corrections apart from the respawn teleport; about 19.7 inputs/s.
- **Live test:** over UDP, a fighter bot kills and loots 6 mobs and levels up. A second bot fills its pack, dies, recovers its whole pack and coin from its corpse, and checks the XP penalty against the rules. Also under ASan.
- **Headless client e2e:** the real Godot client runs the same fight, death and recovery flow.

**Done**
- **Stats and rules** (`server/src/game/rules.*`, pure and unit-tested; every constant in `content/rules.json`):
  - Combat math: race and class stats, derived HP/mana/endurance/AC, hit/miss/crit, armor mitigation, dodge/parry/riposte, double attack, weapon damage and delay (a class placeholder weapon until Phase 5).
  - Progression: stun and taunt checks, skill caps and skill-ups, regeneration (better out of combat, better again sitting), the exponential-ish XP curve, con colors, con-scaled kill XP, the group XP split hook, and the death XP penalty.
  - Determinism: a seeded RNG.
- **Mobs:**
  - **Content:** `content/mobs.json` has 25 placeholder mobs from level 1 to 60, including the named Grimjaw the Ancient. Each has HP and damage multipliers, attack delay, speed, aggro and assist radius, faction, flee threshold, wander and leash radius, respawn timer, stun immunity, and a loot table.
  - **Spawns:** The Proving Flats grew to 2 × 2 km with 25 spawn groups in rings by level band.
  - **AI:** a pure state machine (idle/wander/aggro/chase/leash/flee/dead/respawn) with proximity aggro (never on grey-con players) and same-faction assist. Leashing evades and resets; mobs flee once at low HP; respawns are new entities.
  - **Threat:** a threat table: damage plus bonus threat, taunt to the top, a healing-threat hook, and the top of the list gets attacked.
- **Combat:**
  - **Intents:** target, auto-attack (swing timer from weapon delay, range warnings), and abilities with a 1.5 s global cooldown and per-ability cooldowns.
  - **Costs and checks:** endurance costs, plus level, range and target checks with clear refusal messages.
  - **Wire messages:** `CombatEvent`s carry the combat log, and `PlayerStats`, `TargetInfo`, `AbilityList`, `SkillValues` and `Inventory` keep the client in sync.
  - Dead and stunned players can't move or act. Sitting stands up on movement or combat.
- **Warrior:** 16 abilities with original names (12 active, 4 passive skills):
  - Taunts: Iron Challenge and Warlord's Challenge.
  - Strikes: Rising Kick, Cleaving Blow and Earthsplitter.
  - Stun: Pommel Crack.
  - Heals: Rallying Cry and Stalwart Resolve.
  - Disciplines: Bulwark Stance and Unbroken Wall (defensive), Battle Fury (offensive).
  - Area threat: Provoking Roar.
  - Passives: Dodge, Parry, Double Attack and Riposte, which improve through use.
- **XP and leveling:**
  - Con-scaled XP goes to the player who did the most damage.
  - Level-ups recompute stats, unlock skills, announce newly learned abilities, refresh the level other players see, and save.
  - Levels 50→60 take about as long as 1→50 (tested), and level 60 is reachable.
- **Death and corpses:**
  - The XP penalty applies from level 6 and can cost a level.
  - The corpse holds coin and pack for 1 hour, is saved in the same transaction as the stripped character, and is restored on server restart.
  - Respawn is at the bind point (the starting city).
  - Only the owner can loot a player corpse; mob corpse loot goes to the top damage dealer. One looter at a time, coin is auto-looted, and every loot step is saved.
  - Mob loot tables give coin plus items; 26 placeholder items, 16-slot pack.
- **Persistence:** schema v2 adds XP, vitals, coin, bind point and skills to characters, plus `character_items` and `corpses` tables. It migrates Phase 1 databases in place.
- **Client:**
  - **Frames and bars:** player frame (HP/endurance/mana, status), target frame with con color and a Loot button, XP bar.
  - **Hotbar:** 10 slots, keys 1-0, drag-and-drop from the ability window (K), cooldown and global-cooldown sweeps, saved per character.
  - **Feedback:** floating combat text, and a Combat tab in chat with an EverQuest-style combat log.
  - **Windows and dialogs:** loot window, pack (I), death dialog with Respawn.
  - **Targeting and commands:** Tab/Shift+Tab and click targeting, F1 self-target, consider (C), sit (X), and slash commands (/sit /stand /loc /con /attack /target).
  - **Placeholder visuals:** mobs by model family, corpses lying flat, ring markers every 100 m.
  - **Autotest:** `--fight N --camp X,Z --die-at X,Z --death-screenshot --show-windows`.
- **Tools:**
  - `emberfall-bot --fight N`, `--die` (combinable), with the XP penalty checked against the rules.
  - `tools/gen_phase2_content.py`.
  - `tools/validate_content.py` checks all the new files and cross-references.
  - `live_test.sh` and `client_e2e.sh` include the combat runs.
- **Tests:** 116 Catch2 tests, 46 of them new:
  - combat math and probabilities, con table, XP curve shape and 1-60 reachability, death penalty, regen;
  - Monte Carlo Warrior-vs-even-con balance at 8 levels;
  - threat table and every AI transition;
  - zone combat: kill/XP/loot, level-ups, aggro and assist, leash reset, taunt, ability gating, stuns, death/corpse/respawn/recovery/decay, loot rights, overkill credit, locked skills, sitting regen, respawn timers;
  - content validation;
  - database: character state, corpses, v1 → v2 migration;
  - full sessions over the wire, including corpse restore after a server restart.
- **Build:** warning-free on GCC 16, GCC 13 (Docker) and Clang 22; ASan/UBSan clean; clang-tidy 0 findings.
- **CodeRabbit:** the first scan found 5 major issues, all fixed with regression tests:
  - an overkill finishing blow could steal loot and XP rights;
  - skills locked after losing a level still worked in combat;
  - hand-placed mobs kept a pointer to the caller's definition;
  - the content generator would drop other zones;
  - the bot didn't check the death XP penalty.
  A second scan found 3 more (major), fixed:
  - a database error mid-read could return a partial pack, which the next save would make permanent;
  - a `null` content file loaded as an empty list;
  - losing a level to the death penalty saved the character before the corpse write.
  A third scan found 7 (major), fixed:
  - snapshot selection could starve entities past 256;
  - XP curves whose total overflows `uint64` were accepted;
  - starting a hotbar drag fired the ability;
  - four bot checks: corpse-to-kill matching, a full-pack recovery check, a loot-take loop guard, and the XP-penalty assertion.
  A fourth scan found 14 (1 major, 13 minor), all fixed:
  - snapshots now track per watcher what was last sent, so nothing starves;
  - buffs expire before combat;
  - a counter-swing cannot be riposted;
  - a character save that matches no row fails its transaction;
  - corpse restore filters expired rows;
  - malformed rule sections are rejected;
  - hotbar polish: swaps save once, save failures are reported, a corrupt layout is ignored, the ability window refreshes on level-up, and the death dialog no longer promises one hour;
  - test guards.

**Stubbed / placeholder**
- **Items and stats:** no equipment. A class placeholder weapon, stats from race and class only, and consumables that can't be used yet (Phase 5).
- **Groups:** group XP is a hook with one member, and loot rights go to the top damage dealer (groups arrive in Phase 7).
- **Mobs:** factions are a string used only for assist; there are no special abilities (rampage, summon, procs), and mobs never stun players.
- **Zones:** one test zone. Cross-zone binds fall back to the zone's safe spot until zone lines exist (Phase 6).
- **Art:** placeholder capsules for mobs and corpses.

**Known gaps**
- No facing requirement for melee or abilities.
- No pathing; mobs move in straight lines on the flat test zone.
- Ability cooldowns don't persist across logout. A long cooldown such as Stalwart Resolve (15 min) resets on relog.
- Game traffic after login is still unencrypted (see Phase 1).
- The balance target is untuned against gear (Phase 5) and real dungeon content (Phase 6). A fully skilled Warrior wins even-con fights with 60-75% HP left.

**Next**
- Phase 3 (the spell engine with Cleric and Wizard) builds on the effect list in `abilities.json`, the buff list on players, the mana resource, and the empty player corpses kept for resurrection.
- Persist cooldowns, and add a facing check for melee.

### Phase 1: Foundations: connect, log in, walk around (completed 2026-11-20)

**Verified end to end:** `emberfall-server` running in Docker on a Linux host (Gentoo, Docker 29) with two
`EmberfallClient.exe` instances on Windows 11. Both registered, created characters, entered
The Proving Flats, saw each other move, and exchanged say chat. A second run logged into the existing
accounts. Results: 0 movement corrections, ~19.5 inputs/s, 235/236 inputs acknowledged at report time.
Version mismatch (v0 and v99) is rejected with "Please update your client". 3,000 network-fuzzed
packets plus 400k in-process mutated packets did not crash the server.

**Done**
- Server: CMake presets (debug, release, asan), pinned FetchContent dependencies, JSON config with
  `EMBERFALL_*` env overrides and fail-fast validation, spdlog console + rotating file, ENet host
  (2 channels, packet size cap, per-IP connection cap), 20 Hz fixed-tick loop, DB worker thread,
  SIGTERM/SIGINT graceful shutdown that saves everyone, `/health` JSON endpoint.
- Protocol: `protocol/messages.json` -> `tools/gen_protocol.py` -> C++ header + GDScript codec;
  little-endian, bounds-checked, strict (UTF-8, finite floats, array caps, no trailing bytes);
  version handshake (now protocol v2); `--check` mode for CI. Per-client token-bucket rate limiting,
  violation counting and kicks, direction/channel/state checks on every message.
- Login security: passwords travel only over a TLS 1.2+ login service (OpenSSL, TCP 7779) to a
  self-signed certificate that clients pin on first use (fingerprint prompt; a changed certificate is
  refused). The service returns a single-use ticket bound to the ENet connection's nonce, and
  the client redeems it over ENet. The login service has per-IP rate limits, I/O deadlines, and a bounded worker pool.
- Accounts and characters: SQLite (WAL) behind a `Repository` interface; Argon2id (libsodium)
  register/login on the DB thread; one session per account; bad-ticket lockout; character
  list/create/delete with name rules, race/class/city validation, 4-per-account limit; position save
  on logout/disconnect/shutdown and 60 s autosave.
- Zone: one test zone (`proving_flats`), spatial-hash interest manager with hysteresis,
  spawn/despawn, intent-based movement with input budget (no speed hacks), teleport and desync
  detection with authoritative corrections, unreliable snapshots on channel 1, say chat by radius.
- Godot client (4.7.2, GDScript, static typing): login/register with editable server address,
  TLS login with trust-on-first-use certificate pinning, character select/create/delete, world with third-person camera, client-side prediction +
  reconciliation, interpolated remote players, chat window, HUD, Esc menu, `--autotest` mode.
- CodeRabbit scan (end of phase): 5 major findings, all addressed. Cleartext credentials were fixed
  by the TLS login service. Also fixed: the character-delete confirmation could delete a different
  character after a list refresh; the native run command in BUILD.md used container paths; the
  health endpoint had no whole-request deadline (slow-client stall); compose published the cleartext
  login port everywhere (moot once TLS landed; `EMBERFALL_GAME_BIND` can still restrict it).
  A second scan after these changes found 10 more (2 major, 8 minor), all fixed:
  - TLS and health listen sockets made non-blocking (accept race could hang shutdown).
  - Autotest `--expect-error` could pass on a successful login; it could also skip character creation after a slow frame, and it ignored a failed result write.
  - `max_players` is now enforced at world entry.
  - The GDScript string encoder bounds characters before trimming bytes.
  - The silent-client TLS test now requires a server-side close.
  - The bot checks max displacement rather than the endpoint, and fails if `--delete-after` doesn't delete.
  - The test scripts clean up their servers on interrupt.
  A third, final scan found 1 major issue, now fixed: the TLS login server's stop flag wasn't set
  under the queue mutex, so a worker could miss the wakeup and hang shutdown (and the save after it).
- Tooling: `emberfall-bot` (scenario with TLS login, `--expect-version-reject`, `--fuzz`), 70 Catch2
  tests (protocol, randomized/mutation fuzzing, movement, spatial hash, rate limiter, auth, DB,
  config, content, zone, full server sessions incl. malformed-packet storm, tickets, TLS identity,
  TLS login service incl. pin mismatch, garbage input, and per-IP limits, max_players), `tools/validate_content.py`,
  `tools/scripts/live_test.sh`, `tools/scripts/client_e2e.sh`, `.clang-tidy`.
- Build/deploy: multi-stage Dockerfile (runs tests in build stage, non-root runtime), compose file,
  Windows export preset with embedded PCK and icon, scripted export, CI workflow (server build +
  tests + ASan/UBSan + live bot test + headless client e2e + Docker image; Windows export artifact).
- Warning-free and all tests passing on GCC 16, GCC 13 (Ubuntu 24.04 Docker), and Clang 22.

**Stubbed / placeholder**
- Content: 3 placeholder races (Highlander, Vaelith, Deepkin), 14 classes with descriptions only,
  3 starting cities that all spawn into the single test zone. No stats yet.
- Character creation options are sent by the server (`CreateOptions`) instead of a client-bundled
  content copy.
- `admin_password` config and the `is_admin` account flag exist but no GM commands use them.
- Placeholder art: capsules with facing markers and name tags, grid ground, boundary posts.
- Health endpoint is stats only (no admin actions).
- `.clang-tidy` is clean (0 findings with clang-tidy 22) but not yet enforced in CI.

**Known gaps**
- Game traffic after login (movement, chat) is not encrypted. Passwords are protected, but an on-path
  attacker could hijack a live session. See DECISIONS.md.
- Lost movement packets cause small server-side snap-backs (no input redundancy yet).
- No client/server clock-drift compensation beyond the input budget.

**Next**
- Phase 2 (core combat and leveling, Warrior) builds on the zone, session, and protocol layers here.
- Movement input redundancy (resend the last few intents in each `MoveInput`) to remove snap-backs under packet loss.
- Enforce clang-tidy in CI. The systemd unit, backup script, and DEPLOY.md belong to Phase 8.
