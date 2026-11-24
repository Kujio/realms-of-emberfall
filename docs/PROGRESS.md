# Progress

Work one phase at a time (see CLAUDE.md). At the end of each phase, record **Done / Stubbed / Next** under it.

## Phase checklist

Phases 0-9 follow the prompt pack in `emberfall_prompts.md`; from Phase 10 on, each phase is chosen from
the gaps recorded below.

- [x] Phase 0: Repository setup (monorepo layout, CLAUDE.md, docs, .gitignore/.gitattributes, first commit)
- [x] Phase 1: Foundations: connect, log in, walk around (server skeleton, protocol + generator, accounts/characters, test zone with interest management and validated movement, Godot client with prediction/interpolation and say chat, bot, tests, Docker, Windows export, CI)
- [x] Phase 2: Core combat and leveling, Warrior only (stats/rules, mobs + AI + threat, auto-attack/GCD, ~10 Warrior abilities, XP to 60, death/corpses/loot basics, combat UI)
- [x] Phase 3: Spell engine with Cleric and Wizard (cast state machine, effects, resists, buffs/debuffs/stacking, scrolls/spellbook/gems, trainers, spell UI)
- [x] Phase 3.5: Character model and animation pipeline (glTF/GLB models from content with placeholder fallback, data-driven animation maps and state machine, attachment points, appearance variants, facing/foot-sliding fixes, CC0 starter assets + ASSETS.md, model previews in character select/create, "how to add a model" doc)
- [x] Phase 4: All 14 classes, pets, skills, threat (pets, Bard songs, positionals, feign/sneak/charm/illusions, skills and proficiencies, final races, balance simulator v1)
- [x] Phase 5: Items, equipment, vendors, bank, trade (600+ items, equipment stats/procs, loot tables and group loot, vendors, bank, secure trade, auction board)
- [x] Phase 6: World content (6a zone pipeline + procedural dungeons + travel, 6b hubs + 12 outdoor zones, 6c 8 dungeons, 6d 150+ mobs + factions + NPCs, 6e quests + class epics, 6f map/journal/dialogue UI, 6g content validation)
- [x] Phase 7: Social systems (all chat channels, groups, guilds, friends, /who and slash commands, GM commands with audit log and bans)
- [x] Phase 8: Polish, balance, load test, release (balance sim 1-60, 100+ bot load test, robustness, client settings/polish, installer + release CI, README/DEPLOY.md, smoke test, tag v0.1.0)
- [x] Phase 9: Art pass (one art direction; character, equipment, mob and pet visuals; data-driven spell effect library; per-biome environments and zone theming; UI theme and icons; audio and mixer; graphics quality and performance; Credits screen and LICENSES/)
- [x] Phase 10: Encrypted game traffic (a session key over the TLS login; every ENet packet after login sealed with AES-256-CTR and HMAC-SHA256 on server, client and bot; protocol v15)
- [x] Phase 11: The world's surfaces (generated detail textures; textured ground per biome with high, slope and shore layers; ground drawn past each zone's edge; water pools and a sea; a cloud sky; textured dungeon walls and floors)
- [x] Phase 12: Towns, groups and the map (groups outlast logouts and restarts, schema v9; guards defend their towns from monsters and claim their kills; a drawn world map with routes; protocol v16)

## Phase notes

### Phase 12: Towns, groups and the map (completed 2026-11-24, version 0.5.0)

**Done**
- **Groups outlast logouts and restarts:**
  - A member who leaves the world keeps their place, offline, for 15 minutes
    (`social.group.offline_grace_seconds`), and is back in the group on their return ("You are
    back with your group.").
  - Leadership passes to a member who is online. Offline members are greyed in the group window,
    with their last known level and class.
  - Groups are saved (schema v9) and come back after a restart, every member offline until they log
    in. A deleted character leaves its group.
- **Guards defend their towns:**
  - A city guard attacks a monster fighting within its sight (a train led into town) and calls the
    other guards. The monster fights back.
  - If the guards land the killing blow or out-damage the players, they claim the kill: no XP,
    loot, faction or quest credit, and an empty corpse.
  - Monsters minding their own business, pets and other guards are left alone.
  - Monsters never cross a zone line, so two of each city's guards keep its gate on the outdoor
    side (the Emberglade, Briarwatch Wood, Cragfoot Hills), where a train toward town ends up.
- **A drawn world map:**
  - The map window's World tab draws every city and outdoor zone in its cell (authored in
    `gen_world.py`, checked by the validator), joined by roads and coloured by how its level band
    suits you.
  - Dungeons are diamonds where their entrances are. Clicking a place describes it, lists its
    dungeons and travel stops, and lights the way there. The list moved to a Zones tab.
- **Smoke test:** `EMBERFALL_RNG_SEED` pins the dice. The CI smoke run's "died to
  level-appropriate mobs" flake is gone.
- **Protocol v16** (`GroupMember.online`). **Version** 0.5.0.

**Verified**
- Catch2: 323 tests, warning-free, also under ASan/UBSan. New tests cover offline members and the
  grace period, restore checks and id allocation, groups across a restart over the wire, a failed
  group save retried with the current roster, schema v9 save, load and upgrade, guards answering a
  train and claiming the kill, guards ignoring peaceful or leashing monsters, and the seed override.
- Live test, smoke test (seeded) and the Windows client's zone walk against a server built from this
  branch: all pass. The world map drawn by the exported client (`--lineup DIR --worldmap`).
- Local CodeRabbit scans: two rounds, nine real points fixed (failed group writes retried, saved
  leaders keep the lead, ids after a restored maximum, guards spare leashing monsters, and smaller
  ones); the rest checked and answered.
- Client: 765 checks. New checks cover map cells, routes, details, clicks on zones and dungeons,
  band colours, and offline group rows.
- `validate_content.py` and its self-test (31 broken copies, 2 new for the map), every generator
  `--check`, the protocol check.

**Stubbed / placeholder**
- Ferry and teleport routes are named in a zone's details, not drawn as lines.
- Guards stay in their own zone.

**Next**
- Candidates: the art still listed in ART_TODO.md (snakes, dungeon wall meshes, landmark buildings),
  tradeskills (the stretch goal), raid tools.

### Phase 11: The world's surfaces (completed 2026-11-24, version 0.4.0)

The ground had no texture: every zone was one flat colour shaded by height, dungeon walls were
plain blocks, there was no water, and the fog washed the sky out.

**Verified end to end:**
- **Every zone photographed:** all 26 zones, from the exported Windows client's lineup tool
  (`--zones`), plus close views of the bog's pools, the fens' pools and the Duskmere's sea
  (`--view`).
- **FPS against `main`:** the same views with `--fps` (vsync off, Intel Iris Xe, 1920x1080).
  Outdoors it is 124 to 143 FPS against 146 to 161 on `main`; hubs and dungeons are unchanged.
  The table is in DECISIONS.md.
- **Tests:** client 694 checks (44 new); `validate_content.py` and its self-test (4 new broken
  copies); `gen_textures.py --check`; every other generator check; the server builds and tests
  unchanged.

**Done**
- **Textures:** `tools/gen_textures.py` (numpy and Pillow) makes 17 tileable 512 px maps: grass,
  dirt, rock, sand, snow, mud, ash, cobble, flagstone, brick, blocks, tiles, cave rock, scorched
  rock, ripples (a normal map), clouds and a macro variation map. They are detail maps around grey,
  tinted by the content, original and CC0. Committed `.import` files give them mipmaps and GPU
  compression. CI installs numpy and Pillow and checks them, with a small pixel tolerance.
- **Ground** (`shaders/terrain.gdshader`, `scripts/world/surfaces.gd`): the biome's `ground` block
  in `content/environments.json` names a base, high, slope and shore layer with tints, a tile
  scale, the skirt height and the shore band. Hub grounds use the same shader.
- **Past the edge:** outdoor terrain continues 300 m beyond the bounds, rising into hills or
  sinking under a sea.
- **Water** (`shaders/water.gdshader`): `theme.water` per zone (level, colour, opacity, sea sides).
  Pools fill the Gloomfen Bog's and the Saltmarsh Fens' hollows, and the Duskmere lies south of
  Duskmere Shore. The terrain draws a wet, darker band at the waterline.
- **Sky** (`shaders/sky.gdshader`): the theme's sky and horizon colours, a sun disc and glow, and
  clouds by biome (cover, colours, how much fog veils the sky).
- **Dungeons** (`shaders/dungeon.gdshader`): floors, walls and wall tops textured per biome (crypt
  brick and flagstone, mine and cave rock, ruin blocks, temple and citadel tiles, lair scorched
  rock).
- **Validation:** textures named in `environments.json` must exist; ground, sky and dungeon numbers
  and colours are checked; `theme.water` is checked, and no sea side may hold a zone line.
- **Tools:** the lineup tool's `--view X,Z,TX,TZ` and `--fps`.
- **Version** 0.4.0 (client, server, installer).

**Stubbed / placeholder**
- Clouds hold still (a moving sky re-renders its radiance every frame).
- Water reflects only the sky, has no shoreline foam, and nobody swims in it.
- No rivers or waterfalls; hub cities have no drawn roads; zone-line gateways and waystones are
  still primitive shapes (ART_TODO.md).

**Next**
- Phase 12 (done): persistent groups, guards that fight monsters in town, a drawn world map.

### Phase 10: Encrypted game traffic (completed 2026-11-23, version 0.3.0)

**Verified end to end:**
- **The real Windows client** (`EmberfallClient.exe`, exported on thor) against a server built from
  this branch, on separate test ports: it logged in, entered Emberhold, walked, chatted and camped. The
  autotest result shows `encrypted: true`, 208 snapshots, no corrections and no refused packets.
- **On the wire:** UDP traffic captured on the server during a bot session. The handshake text is
  readable; none of the text sent after login appears (the chat line, "You have entered", the welcome
  with the account name, the character's name).
- **Load:** `load_test.sh` with 150 sealed bots over four zones: tick mean 3.43 ms, p99 5.12 ms
  (Phase 8 without encryption: 3.5 and 5.5), 11% of one core.
- **An old client** (protocol v14) gets "Your client is out of date ... Please update your client."
- **Catch2:** 315 tests (8 new: known-answer vectors made with the openssl command line, round trips,
  tampering, replays, reflection, wrong keys, random input, the secure transport, a reply sent while
  the request is still being handled, and a whole login
  through the game server where nothing after the ticket is readable), warning-free, also under
  ASan/UBSan. **Client:** 650 checks (31 new: the same vectors, round trips of every length, tampering,
  replays, and what `Net` accepts unsealed).

**Done**
- **Session key over TLS:** the login service issues a random 32-byte key with each ticket;
  `TlsLoginResult.session_key` (protocol v15) carries it inside the pinned TLS connection.
- **Sealed packets:**
  - every ENet packet after `TicketLogin`, both ways, on both channels: a counter, then AES-256-CTR
    ciphertext, then a 16-byte HMAC-SHA256 tag, with keys per direction and channel;
  - tags are checked in constant time before anything is decrypted;
  - counters must be exactly the next one on the reliable channel and newer on the unreliable one;
  - a bad reliable packet closes the connection.
- **Server:** `net/packet_crypto` (OpenSSL AES-CTR, libsodium HMAC) and `net/secure_transport`,
  which wraps ENet. The game server only hands over the key; the raw ENet transport refuses to be
  asked for encryption.
- **Client:** `net/packet_crypto.gd` (Godot's AESContext and HMACContext); `Net` seals and opens.
  Before the first sealed packet it accepts only three messages unsealed: a ServerStatus resent
  late, a refused AuthResult and a Kick.
- **Bot:** the load-test bot seals too, so load tests now measure the encrypted protocol.
- **Review fixes (local CodeRabbit scan):**
  - a ServerStatus that ENet resends late no longer breaks a login;
  - the bot accepts only what the client accepts unsealed;
  - an OpenSSL failure closes one connection, not the server;
  - a reliable packet ENet refuses closes that connection instead of silently going missing.
- **PR review fixes:** the client and the bot follow the server's rules. A bad reliable packet, a
  reliable send that fails, or 32 bad unreliable packets close the connection.
- **Live test:** `live_test.sh` waits up to 30 s for the server, not 5. An ASan build on a slower
  machine took 5.6 s to load the content, and the first two bots started before it was listening.
- **Export:** `build/*` is excluded from the Windows export. An export run in a checkout with
  earlier builds in `client/build` used to pack them (206 MB instead of 180).
- **Docs:** DECISIONS.md ("Game traffic encrypted after login"), ARCHITECTURE.md, ADMIN.md, the
  protocol file's channel notes.

**Stubbed / placeholder**
- The handshake stays readable (it carries nothing secret), and so do packet sizes and timing.
- Groups still end at a logout or restart; guards still ignore monsters in town; the world tab is
  still a list (Phase 12 candidates).

**Next**
- Phase 11 (my pick): the world's surfaces: ground textures per biome, water in bogs, marshes and
  shores, skies, and dungeon wall and floor materials.

### Phase 9: Art pass (completed 2026-11-23, version 0.2.0)

**Verified end to end:**
- **Windows client, Docker server:** the exported `EmberfallClient.exe` on Windows 11: every race, gender and armour type in the lineup tool (`--lineup`), every zone's lighting, props and clutter (`--zones`), every effect kind (`--effects`), the HUD and windows at 1280x720, 1920x1080, 2560x1440 and 3840x2160 (`--window`), zone music, ambience and footsteps in the audio report, and the Credits screen from the login screen and the in-game menu (`--polish`).
- **FPS** in Emberhold with 100 load-test bots (about 230 characters and mobs in view, 1920x1080, vsync off, Intel Iris Xe integrated graphics): Low 47.0 average (32.3 1% low), Medium 32.8 (18.6), High 30.3 (19.7). Details and the export size (about 180 MB, from 128 MB) in DECISIONS.md.
- **Catch2:** 307 tests, warning-free; **client:** 614 checks (60 new); `validate_content.py` and its self-test, every generator `--check` (now with `gen_icons.py`), `gen_protocol.py --check`.

**Done**
- **Art direction:** one low-poly, flat-shaded, vertex-coloured style from Polyfork's CC BY 4.0 catalogue, tied together per biome by sun, ambient light, fog and sky (`content/environments.json`).
- **Characters and equipment (protocol v14):** 19 Polyfork race models with 25 CC0 Quaternius clips each, retargeted by `tools/retarget_to_glb.sh`; each race and gender has an outfit model per armour type; the worn chest piece picks it and its level tints it (`armor_tiers`), and a helmet shows while a head item is worn. `EntityEquipment` and `EntitySpawn` carry the chest and head items, so others see the change.
- **Mobs, pets and NPCs:** 43 Polyfork monsters and townsfolk (plus tinted variants) with 11 clips each, and 19 Polyfork animals with procedural attacks, flinches and deaths; picked per mob, pet and NPC in `tools/model_picks.py`, which the generators apply. Every mob keeps its collision radius, so no fight changed.
- **Spell effects:** `content/effects.json`: about thirty procedural effects for casts, projectiles, impacts, heals, buffs, ground rings and lasting states (stun, mez, root, fear, charm), coloured by school, with per-spell overrides; full-screen cues for low health, crowd control and level ups. The validator checks every reference.
- **Environments:** 112 Polyfork kit parts over the unchanged collision props, with variants per biome and per hub city; seeded ground clutter and dungeon decoration as chunked MultiMeshes; per-biome lighting and fog.
- **UI:** one theme (`game_theme.gd`) over every window; 44 generated SVG icons (`tools/gen_icons.py`) by spell type and school and by item kind, framed by rarity; the window scales from a 1280x720 base to 4K.
- **Audio:** `content/audio.json`: music and ambience per zone and biome with crossfades, boss and battle themes, footsteps per surface, casts and impacts pitched per school; an Ambient bus (Master, Music, Ambient, SFX, UI) with sliders in Settings. CC0 from OpenGameArt.
- **Performance:** a Graphics quality setting (Low, Medium, High) for shadows, anti-aliasing, view, prop, clutter and character draw distances, clutter density and effect particles; character level of detail (animation every 1, 2 or 4 frames by distance, shadows only near the camera).
- **Credits and licences:** a Credits screen (login screen and in-game menu) generated by `tools/sync_client_content.py` from ASSETS.md, the packs' LICENSE.txt credit lines and `LICENSES/` (CC0 1.0, CC BY 4.0, OFL 1.1 for Open Sans, Godot's licence and copyright); the validator fails if an asset folder is missing from ASSETS.md.
- **Tools:** the lineup tool's `--zones` and `--effects` modes; autotest `--window`, `--quality`, `--no-vsync`, FPS and audio reports.
- **Reviews:** PRs #1 and #2 (characters, mobs, environments) went through CodeRabbit, Greptile and Aikido; every finding was fixed or answered on the pull request.

**Stubbed / placeholder** (all in docs/ART_TODO.md)
- Snakes (no free model), several female and race-specific outfits borrowed from another race or gender, armour tiers shown as tints only (no meshes per tier; gloves and boots do not change the look).
- No attack, hit or death clips for animals (procedural); stand-ins for ghouls, giants, ogres, golems and imps.
- Dungeon walls and floors are shaded blocks; no water, ground textures, skyboxes or landmark buildings.
- Effects are procedural shapes; icons are per category, not per spell or item; Godot's default font.
- One cast and impact sound pitched per school; no mob voices, no town or shore ambience, no login theme.
- FPS was measured on integrated graphics only. (The installer was first compiled by the v0.2.0 release workflow, and installing from it was confirmed on Windows.)

**Next**
- The prompt pack ends with Phase 9. Candidates: the art still missing (ART_TODO.md), a discrete-GPU FPS run, and running the release workflow on a `v0.2.0` tag.

### Phase 8: Polish, balance, load test, and release (completed 2026-11-22, tagged v0.1.0)

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v12, the live database untouched) ran `--polish`: the login screen showed "Online: 0 players of 200 (server 0.1.0)" and the message of the day from the probe, the settings window opened with 37 key actions, a rebind through the real autoload moved the key and reset restored it, the loading screen showed on the way in, the window title read "Realms of Emberfall - Ashwyn (Emberhold)", seven sounds played, and the character walked and chatted with 0 movement corrections. Screenshots: `client/build/verify8g` (`login.png`, `settings.png`, `world.png`). The run also found and fixed an old layout bug: the menu screens had no size (panels sat in the top-left corner since Phase 1); they now live on a canvas layer and are centered with their backdrop.
- **Smoke test** (`tools/scripts/smoke_test.sh`): a fresh server, then one character through login → create → level 1 to 3 (looting every kill) → a two-way trade (coin for an item) with a second character → logout → login again; both bots pass, the server shuts down cleanly with nothing in its error log. The run took 18 kills to reach level 3; the fighter gave 25 copper and received one of the partner's starting items.
- **Load test** (`tools/scripts/load_test.sh`, release build, four zones of the test band): 150 bots: tick mean 3.5 ms, p99 5.5 ms, 12% of one core, 101 MB, 10 KB/s sent per player. 300 bots (~75 per zone): mean 8.1 ms, p99 11.5 ms, worst 16 ms, 22% of a core, 102 MB, 17.5 KB/s per player. Every bot entered and stayed; budget 50 ms.
- **Headless client e2e** (`tools/scripts/client_e2e.sh`): every Phase 1-7 run passes against the release server.
- **Fuzzing:** the libFuzzer decoder target ran 20 M inputs in 5 minutes and the live-session target 0.74 M streams in 7 minutes (5,100+ coverage edges), both under ASan/UBSan, with no crash.
- **Catch2:** 306 tests (11 new), GCC 16 release and ASan/UBSan, warning-free; the live bot test also passes under ASan/UBSan with no reports; clang-tidy (LLVM 23) clean.
- **Backups, for real:** `deploy/backup.sh docker` against the Docker server took an online copy of the live database (mode 0600, integrity-checked), and `--check-db` passes on it. **Client:** 554 checks (73 new).
- **Content and generators:** `validate_content.py` and its self-test, every generator `--check` (now with `gen_sounds.py`), `gen_protocol.py --check`. The Docker image builds and runs its tests.

**Done**
- **Balance** (`emberfall-balance-sim --progression`): time to level 1-60 for every class, solo and in a standard group of six, by band (1-15, 15-30, 30-45, 45-60), with real resting on regeneration, pull time and death costs, from the zone's own experience awards. Tuning: kill experience scales with mob toughness, the group bonus is 30% per extra member, a stronger meditate bonus, priest and Paladin melee multipliers. Result: solo 63-96 h to 60 (0.76-1.15x of the median), grouped 67-77 h (0.94-1.08x); grouping is faster than soloing (it was 4x slower). Table and known imbalances in DECISIONS.md; CI fails beyond 1.4x of the median.
- **Load and performance:**
  - the bot's `--swarm N` (N bots in one process, logging in over a ramp, wandering, fighting, chatting) and `load_test.sh`;
  - `/health` gained the one-minute p99 tick, CPU, bandwidth in and out (ENet's counters), per-player bandwidth, zones loaded, lingering characters, and the time per zone tick phase;
  - hotspots: interest management (the spatial hash keeps positions in its cells: half the time) and snapshots (each entity's entry built once per tick, not once per watcher);
  - `start_zone` takes a list; per-IP connection and TLS login limits are configurable; measured capacity is in DEPLOY.md.
- **Robustness:**
  - **reconnects:** a connection dropped mid-fight leaves the character in the world for 30 s (`linger_seconds`); the account's next login reclaims the same entity where it stands; anything else lets it leave saved;
  - **login timeout:** connections that never log in are dropped after 120 s;
  - **database:** a quick integrity check before any migration at every start (a damaged file stops the server with exit code 3 and the restore steps); `--check-db`; `--backup FILE` (an online, checked copy); `deploy/backup.sh` for cron; the restore procedure in DEPLOY.md;
  - **fuzzing:** `cmake --preset fuzz` builds two libFuzzer targets (the decoder with a round trip; a live in-world session), run in CI for 2 minutes each;
  - **clang-tidy** findings fixed; two noisy checks disabled with reasons;
  - **anti-cheat review** of movement, cooldowns, ranges, duplicate logins and flooding (DECISIONS.md).
- **Protocol v12:** `ServerStatus` (message of the day, players online, max players, server version) after every `HelloResult`.
- **Client polish:**
  - **settings** (Esc menu and login screen): window mode, resolution, vsync, master/music/effects/interface volumes and mute, and 37 rebindable key actions (Godot `InputMap`, physical keys, conflicts move the key), saved in `user://options.cfg`;
  - **login screen:** the server's status (online, players, version) and message of the day from a probe connection, refreshed every 30 s and when the address changes;
  - **loading screen** with the zone name and a tip, into the world and across zone lines;
  - **disconnect dialog** with plain reasons; the window title shows the version, then the character and zone;
  - **audio:** an `Audio` autoload, Music/SFX/UI buses, 21 events in `data/sounds.json` hooked to clicks, hits, misses, spells, heals, level-ups, death, loot, tells, invites and the login ambience; the sounds are synthesized by `tools/gen_sounds.py` (CC0, in ASSETS.md);
  - autotest `--polish DIR`.
- **Packaging:** the export preset carries version, icon and copyright; `deploy/windows/emberfall.iss` (Inno Setup, per-user); `.github/workflows/release.yml` on a `v*` tag: client export, installer on a Windows runner, the Docker image (smoke-tested, pushed to GHCR, saved as a tarball), and a GitHub release with SHA-256 sums.
- **CI:** the progression simulator, the smoke test (ASan), a 100-bot load test, both fuzzers, `gen_sounds.py --check`.
- **Docs:** README (players and admins), DEPLOY.md (fresh Ubuntu VPS: Docker and systemd, firewall, configuration, logs, cron backups, restoring, updating, monitoring, capacity), CONTENT.md (spells, items, mobs, zones, quests), ARCHITECTURE.md (threads, the tick, protocol, persistence, client, testing), RELEASE_NOTES.md; `deploy/systemd/emberfall.service`.

- **CodeRabbit:** three scans found 33 issues (15 major, 18 minor); 32 fixed. The ones that mattered most:
  - backups: a stalled copy could run forever; a destination naming the live database could replace it; two backups could share one staging file or replace each other's result; the staging copy (with password hashes) was world-readable; an existing destination could be paired with a stale journal; `--check-db`/`--backup` on a mistyped path created an empty database (and migrated an old one); a locked database was reported as corrupt;
  - the release workflow pasted the tag into shell source, and published an image whose health check never passed;
  - the progression report hid a class that stopped earning experience; NaN thresholds disabled `--strict`;
  - the smoke test did not check the saved level or the item received; the bots could hang the swarm launch, skip prey forever, or pass after a failed logout; health requests had no deadline;
  - the login screen could show one server's status under another's address; Docker logs grew without limit;
  - skipped: advancing the simulator's clock through pull and death time (the time is counted, and cooldowns lasting past a pull matter only for once-an-hour abilities; DECISIONS.md).

- **Fix after the phase (protocol v13):** players froze for minutes after crossing a zone line (inputs the old zone sent in flight set the new zone's input count above the client's restarted one); the same would have hit a character reclaimed after a dropped connection. The client now continues its count 200 higher across a zone change and the server drops anything below that; a reclaimed character starts a fresh count. Two regression tests (307 in all); on Windows the zone walk (Emberhold → Emberglade → Rattlebone Warren → Emberglade) kept moving after every crossing.

- **Optimization after the phase:** profiled with `perf` (now installed on the build host): the snapshot pass's per-entity hash lookups were 14.5% of server time at 300 players. A list kept beside each player's visible set (last-sent tick and entity pointer) replaced them: 1.6% now; 300 players run at 6.5 ms mean, 9.5 ms p99 (from 8.1 and 11.5). ASan/UBSan tests and live bots are clean.

**Stubbed / placeholder**
- **Art and audio:** all placeholder (Phase 9); the sounds are simple synthesized tones and there is no music beyond the login ambience.
- **Release pipeline not run:** the repository has no GitHub remote yet, so `release.yml`, the Windows installer job and the new CI steps have not run; `v0.1.0` is a local tag. The installer script was not compiled here (no Inno Setup on the build machines).
- **Code signing:** none; SmartScreen warns (documented).

**Known gaps**
- **Balance:** Warriors and Monks level fastest solo, priests slowest; the simulator's autopilot does no crowd control, pulling or kiting (DECISIONS.md).
- **Unencrypted game traffic** after login (Phase 1).
- **Lingering characters** still look online (`/who`, tells go nowhere) until the linger ends.

**Next**
- Phase 9 (art pass): one art direction; character, equipment, mob and pet visuals; spell effects; per-biome environments; UI theme and icons; audio and a mixer pass; graphics quality settings; Credits screen and LICENSES/.

### Phase 7: Social systems (completed 2026-11-22)

**Verified end to end:**
- **Windows client, Docker server:** Two `EmberfallClient.exe` instances on Windows 11 against the Docker server on the Linux host (protocol v11; the live database migrated to schema v8 in place) ran the social scenario: a Highlander Cleric in Emberhold found a Deepkin Warrior in Cragmouth with `/who all`, invited him (accepted from the popup), founded <The Ashen Vow> and recruited him, and both heard each other on the group and guild channels across zones; the group, guild, friends and `/who` windows lay on screen, then the guild was disbanded. 0 movement corrections. Screenshots: `client/build/verify7` (`lead_social.png`, `join_social.png`).
- **Headless client e2e** (`tools/scripts/client_e2e.sh`): every Phase 1-6 run, plus the new social pair. Two players find each other with `/who all`, group through the invitation popup, found and join a guild, and hear each other on the group and guild channels; then the guild is disbanded.
- **Acceptance, in Catch2:**
  - **A group of six clears a dungeon.** Six level-9 members of six classes clear the Rattlebone Warren, the Rattle King included. Every kill worth experience fed every member standing, grey kills fed nobody, and each kill's loot party was everyone standing near it. Coin was split and need/greed rolls were offered to all six.
  - **Guild creation and chat persist across restarts.** A guild with a promoted officer and a message of the day is restored after a restart on the same database file; its members see the message of the day, and guild chat works.
  - **The admin tools are audit-logged.** Setlevel, item, spawn, announce, mute/unmute, ban/unban, kick and tp each leave an audit row naming the account; `/gm audit` reads them back.
  - **A banned account is refused at login,** with the time left and the reason, until it is unbanned.
- **Live test:** the bots pass against the release server.
- **Catch2:** 295 tests (32 new), under GCC 16 (debug, release), Clang 22 and ASan/UBSan, all warning-free and ASan clean. **Client:** 481 checks (51 new).
- **Content:** `validate_content.py` and its `--self-test` (18 broken copies caught, 6 of them in `social.json`); `gen_protocol.py --check`. The Docker image builds and runs its tests.

**Done**
- **Chat** (`game/chat.hpp`, routed in `game_server_social.cpp`):
  - **Channels:** say, shout (the zone), OOC (the server), tells by name (echoed, with `/reply`), group, guild, emotes, and GM announcements.
  - **Limits:** a token bucket per channel, a repeat guard (the same line more than 3 times in 30 s), and a profanity filter hook (`ChatFilter`; the shipped word list masks words that start with a listed word).
  - **Ignore and mutes:** an ignore list (an ignored player's lines are hidden on every channel, their tells bounce as if you were offline, and their invitations never arrive), plus server-side switches for say, shout, OOC, emotes and guild (`/channel ooc off`).
- **Groups** (`game/groups.hpp`, `zone_social.cpp`):
  - **Membership:** invite (by name or target) and accept or decline, with a 60 s expiry; leave, kick, leader handover; at most 6; groups cross zone lines, and logging out leaves the group.
  - **Credit:** kill credit counts per group, and members alive and within 120 m share experience, loot, quest credit and faction.
  - **Group XP:** the mob cons against the highest member, members too far below the highest share nothing, and the rest split the kill by level with a 10% bonus per extra sharer.
  - **Loot rule:** chosen by the leader.
  - **Spells and threat:** group spells and songs reach nearby members. Group-aware threat: a mob that turns on a member notices the members beside it.
  - **Group window (P):** each member's HP, mana and endurance bars; click a member or press F2-F6 to target them.
- **Guilds** (`game/guilds.hpp`, the `guilds` and `guild_members` tables):
  - **Founding:** `/guildcreate` checks the name (letters, spaces and apostrophes; unique in any case; filtered).
  - **Ranks:** invitations, and ranks with permissions from `content/social.json` (leader, officer, member, recruit). Officers manage the ranks below them; leadership passes explicitly; a leader leaves only as the last member.
  - **Members:** a message of the day; a roster with online status, zone, level and class; guild chat; the guild name under each member's name.
  - **Storage:** every change is stored before it is applied or announced, one change per guild at a time; a rank handover is one transaction. An empty or leaderless guild left behind by a failed cleanup is repaired at startup.
- **Friends and `/who`:**
  - **Friends:** a friends list with online and offline notices, and the ignore list and channel switches, saved with the character (schema v8 `social` column).
  - **`/who`:** your zone, or `all`, filtered by name fragment, class, race, level or range, zone and guild.
  - **Slash commands:** one server-side command parser. The client forwards everything it does not handle itself (sit, loc, target, consider, windows), including `/assist`.
- **Admin and moderation** (`game_server_admin.cpp`, `docs/ADMIN.md`):
  - **Rights:** the `is_admin` flag, set by the operator with `emberfall-server --grant-admin USER` (or `--revoke-admin`).
  - **Commands:** `/gm setlevel, spawn, item, tp, goto, summon, kick, ban, unban, mute, unmute, announce, audit`.
  - **Audit log:** every action goes to the `audit_log` table and `logs/audit.log`.
  - **Bans and mutes:** account bans and mutes with durations (`30m`, `7d`, `perm`). A ban is refused at login, checked again at ticket redemption, and kicks the account's online sessions at once.
- **Protocol v11:** `ChatSend`, `SlashCommand`, `GroupState`, `SocialInvite`, `GuildInfo`, `FriendsList`, `WhoResults`; `EntitySpawn.guild`.
- **Database schema v8:** account bans and mutes, the character `social` column, `guilds`, `guild_members`, `audit_log`; Phase 6 databases migrate in place.
- **Client:**
  - **Chat window:** tabs (All with per-channel filters, Chat, Group, Guild, Tells, Combat) and channel prefixes (`/s /sh /ooc /t /r /g /gu /me`); plain text goes to the current tab's channel.
  - **Windows:** group (P), guild (G), friends, ignore and channels (O), `/who` results, and invitation popups.
  - **Autotest:** `--social lead|join --partner NAME`.
- **Tests:**
  - pure rules: the filter, chat limits and repeats, list names, the group registry (formation, cap, leadership, kicks, expiry, an inviter who joined another group), group XP (solo equals the old value, the level floor, grey kills, range), guild permissions and every rank rule, guild names, command parsing, durations, `/who`, bans and mutes, and `social.json` checks;
  - zone: kill credit (ungrouped helpers share nothing; a group outdamaging a stronger soloer; a faraway member), group songs, group-aware threat, the six-member dungeon clear;
  - database: guilds and rosters, transactional rank changes, social lists, bans, mutes, admin flag, the audit log, the v7 → v8 migration;
  - wire: every chat channel with ignore, mutes, the filter and the limits; six-member groups with kick, handover, logout and expiry; guilds across a restart; friends with online notices and persistence; `/who`; every GM tool with audit and ban enforcement at login; 1,500 hostile social packets.
- **CodeRabbit:** four scans found 25 issues (5 major, 20 minor), all fixed. The ones that mattered most:
  - guild changes were announced before they were stored (a failed write could be undone by the next restart): every guild change is now stored first, one change per guild at a time; a leadership handover is one transaction; the last member leaving deletes the guild in one step; a failed join keeps the invitation; and startup repairs empty or leaderless guilds;
  - a tell arriving while you typed a reply redirected it to the new sender;
  - `/addfriend` and `/unignore` toggled instead of adding or removing; a muted player could still broadcast through the guild MOTD; a ban that had already run out still kicked;
  - private tell text was written to the server log (now only who and how long);
  - `/gm item ITEM NAME` without a count gave the item to the GM; help text overflowed one chat message;
  - a grouped winner far from the kill still took experience; an empty admin name started the server instead of failing; the content checker accepted empty sections; a long friends list ran off screen.

**Stubbed / placeholder**
- **Guild bank:** not built (a stretch goal).
- **Online-only data:** an offline friend's level and class are not shown, and `/who` has no anonymous or "looking for group" flag (it shows whether someone is grouped).
- **Filter:** the word list is short; servers extend it in `content/social.json` or plug in their own `ChatFilter`.

**Known gaps**
- **Groups are not persisted:** a restart or logging out ends membership.
- **Group invitations reach anyone online by name,** whatever the distance.
- **Unencrypted traffic:** game traffic after login is still unencrypted (see Phase 1), which is why game master rights come from the operator and not from an in-game password.

**Next**
- Phase 8 (polish, balance, load test, release): balance 1-60 with groups in mind, the load test, DEPLOY.md, and tag v0.1.0.

### Phase 6: World content (completed 2026-11-22)

Done in sub-steps 6a-6g (each below, newest first).

**Verified end to end:**
- **Windows client, Docker server:** `EmberfallClient.exe` on Windows 11 against the Docker server on the Linux host (protocol v10; the live database migrated to schema v7 in place). Screenshots: `client/build/verify6`.
  - A new Emberhold Warrior walked out of the east gate into the Emberglade, into the Rattlebone Warren, and back out: four worlds, 2 movement corrections, layout hashes matching.
  - A Paladin took Captain Brack's quest through the dialogue window, saw it in the journal and the tracker, opened the map, and abandoned it; every window lay on screen.
  - A Vaelith started in Thornwood and a Deepkin in Cragmouth.
- **Headless client e2e** (`tools/scripts/client_e2e.sh`): all Phase 1-5 runs (in the test zone), the zone walk, the quest run, and the certificate pin check.
- **Live test:** the bots pass against the release server.
- **Catch2:** 263 tests (41 new in Phase 6), under GCC 16 (debug, release), Clang 22 and ASan/UBSan, all warning-free and ASan clean. **Client:** 430 checks (205 new).
- **Content:** `validate_content.py` and its `--self-test` (12 broken copies caught); every generator's `--check`; the zone export `--check`. The Windows export is 118.5 MB.

**CodeRabbit:** three scans of Phase 6 found 15 issues (8 major, 7 minor), all fixed:
- **6a scan:**
  - travel NPCs skipped zone checks;
  - room selectors silently placed things at (0, 0) outside dungeons;
  - two scenes could place one NPC;
  - a wall at a zone's edge could trap a body.
- **6c-6g scan:**
  - the new windows used parent-relative positions, so the journal, dialogue and map could open off screen;
  - epic weapons leaked into ordinary loot pools;
  - a boss's adds outlived it;
  - a boss's point-blank blast skipped its turn when a pet held aggro;
  - a repeatable quest could be a prerequisite that never counts as done;
  - the standings list ignored illusions;
  - a stale target regard showed for a new target;
  - the quest e2e checked only that results existed, and a chain test accepted loops;
  - the validator could crash on a malformed `max_level`.
- **Final scan:** the target frame's Talk button opened the trainer window for every NPC (it now does what T does).

Also found in the Windows check and fixed: outdoor ground was washed out (vertex colours, lighting and ground tones); the target frame said "Train" on every NPC; the quest screenshot was overwritten by the end-of-run one.

**Stubbed / placeholder (Phase 6)**
- **Art:**
  - zones, hubs and dungeons are placeholder primitives;
  - bosses are scaled family models;
  - the world map is a list.
- **Mobs and fights:**
  - guards do not fight monsters (no mob-vs-mob combat);
  - fear on players holds them in place;
  - mobs do not heal or buff each other.
- **Travel:** cross-zone resurrection is not supported; ferries are a boarding wait, not a moving boat.
- **Groups:** quests and faction credit go to the loot party (everyone on the damage list) until Phase 7 groups.

**Known gaps**
- The level curve and mob strength across 1-60 are untuned against the new content; Phase 8 balances the whole progression.
- The 25 Phase 2 mobs remain for the test zone alongside the generated families.

**Next**
- Phase 7 (social systems): groups replace the loot party for XP, loot, quests and faction; chat channels, guilds, friends, /who, GM commands.

#### 6g: Content validation (completed 2026-11-22)

**Done**
- **`tools/validate_content.py`** (CI) now also checks, through `tools/world_check.py` and `tools/progression_check.py`:
  - **zones:** every zone is reachable from a starting city and back by zone lines; lines land on real entries; entries are not inside lines; dungeon rooms exist; travel destinations are real;
  - **levels:** every camp's mob fits its zone's band (within 2); hubs hold only guards; some outdoor zone or dungeon covers every level from 1 to 60 (no dead end);
  - **quests:** givers, turn-ins and talk targets are quest givers; every mob a kill step asks for exists and spawns somewhere; items, factions and classes exist; quest items have a `drops_from`, and only quest items do; prerequisites never loop; every class has an epic chain ending in an epic weapon for it alone, for level 46-50;
  - **mobs and factions:** factions, race adjustments and hits are real; mob specials name real mob spells and adds; their numbers are in range; mob spells are harmful only;
  - **loot and vendors:** every loot and stock reference resolves (as before).
- **`--self-test`** (CI): breaks a copy of the content in 12 ways (a line to nowhere, a level-60 boss in a starter zone, a gap at the top of the level range, an unreachable zone, a quest naming a missing mob, a prerequisite loop, a non-questgiver giver, a class without an epic, a loot or stock item that does not exist, an unknown mob spell, an unknown faction) and fails unless the validator catches every one.
- **The server** checks the same at startup (fail fast): mob levels against zone bands, hubs holding only guards, and 1-60 coverage, besides everything from 6a-6e. Two new Catch2 tests break the content the same way.

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
