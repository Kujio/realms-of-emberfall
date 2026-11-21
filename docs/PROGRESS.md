# Progress

Work one phase at a time (see CLAUDE.md). At the end of each phase, record **Done / Stubbed / Next** under it.

## Phase checklist

Phases follow the prompt pack in `emberfall_prompts.md`.

- [x] Phase 0: Repository setup (monorepo layout, CLAUDE.md, docs, .gitignore/.gitattributes, first commit)
- [x] Phase 1: Foundations: connect, log in, walk around (server skeleton, protocol + generator, accounts/characters, test zone with interest management and validated movement, Godot client with prediction/interpolation and say chat, bot, tests, Docker, Windows export, CI)
- [x] Phase 2: Core combat and leveling, Warrior only (stats/rules, mobs + AI + threat, auto-attack/GCD, ~10 Warrior abilities, XP to 60, death/corpses/loot basics, combat UI)
- [ ] Phase 3: Spell engine with Cleric and Wizard (cast state machine, effects, resists, buffs/debuffs/stacking, scrolls/spellbook/gems, trainers, spell UI)
- [ ] Phase 4: All 14 classes, pets, skills, threat (pets, Bard songs, positionals, feign/sneak/charm/illusions, skills and proficiencies, final races, balance simulator v1)
- [ ] Phase 5: Items, equipment, vendors, bank, trade (600+ items, equipment stats/procs, loot tables and group loot, vendors, bank, secure trade, auction board)
- [ ] Phase 6: World content (6a zone pipeline + procedural dungeons + travel, 6b hubs + 12 outdoor zones, 6c 8 dungeons, 6d 150+ mobs + factions + NPCs, 6e quests + class epics, 6f map/journal/dialogue UI, 6g content validation)
- [ ] Phase 7: Social systems (all chat channels, groups, guilds, friends, /who and slash commands, GM commands with audit log and bans)
- [ ] Phase 8: Polish, balance, load test, release (balance sim 1-60, 100+ bot load test, robustness, client settings/polish, installer + release CI, README/DEPLOY.md, smoke test, tag v0.1.0)

## Phase notes

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
