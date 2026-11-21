# Progress

Work one phase at a time (see CLAUDE.md). At the end of each phase, record **Done / Stubbed / Next** under it.

## Phase checklist

Phases follow the prompt pack in `emberfall_prompts.md`.

- [x] Phase 0: Repository setup (monorepo layout, CLAUDE.md, docs, .gitignore/.gitattributes, first commit)
- [x] Phase 1: Foundations: connect, log in, walk around (server skeleton, protocol + generator, accounts/characters, test zone with interest management and validated movement, Godot client with prediction/interpolation and say chat, bot, tests, Docker, Windows export, CI)
- [x] Phase 2: Core combat and leveling, Warrior only (stats/rules, mobs + AI + threat, auto-attack/GCD, ~10 Warrior abilities, XP to 60, death/corpses/loot basics, combat UI)
- [x] Phase 3: Spell engine with Cleric and Wizard (cast state machine, effects, resists, buffs/debuffs/stacking, scrolls/spellbook/gems, trainers, spell UI)
- [x] Phase 3.5: Character model and animation pipeline (glTF/GLB models from content with placeholder fallback, data-driven animation maps and state machine, attachment points, appearance variants, facing/foot-sliding fixes, CC0 starter assets + ASSETS.md, model previews in character select/create, "how to add a model" doc)
- [ ] Phase 4: All 14 classes, pets, skills, threat (pets, Bard songs, positionals, feign/sneak/charm/illusions, skills and proficiencies, final races, balance simulator v1)
- [ ] Phase 5: Items, equipment, vendors, bank, trade (600+ items, equipment stats/procs, loot tables and group loot, vendors, bank, secure trade, auction board)
- [ ] Phase 6: World content (6a zone pipeline + procedural dungeons + travel, 6b hubs + 12 outdoor zones, 6c 8 dungeons, 6d 150+ mobs + factions + NPCs, 6e quests + class epics, 6f map/journal/dialogue UI, 6g content validation)
- [ ] Phase 7: Social systems (all chat channels, groups, guilds, friends, /who and slash commands, GM commands with audit log and bans)
- [ ] Phase 8: Polish, balance, load test, release (balance sim 1-60, 100+ bot load test, robustness, client settings/polish, installer + release CI, README/DEPLOY.md, smoke test, tag v0.1.0)
- [ ] Phase 9: Art pass (one art direction; character, equipment, mob and pet visuals; data-driven spell effect library; per-biome environments and zone theming; UI theme and icons; audio and mixer; graphics quality and performance; Credits screen and LICENSES/)

## Phase notes

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
