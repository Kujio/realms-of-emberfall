# Realms of Emberfall: Project Context

Original fantasy MMORPG inspired by the original EverQuest (1999-2001 era, levels 1-60). Use ORIGINAL names, lore, and art for everything. Never copy EverQuest's proprietary zone/NPC/item/spell names. Mechanics and class archetypes are fine; names must be original.

## Working rules
- Work in phases (see docs/PROGRESS.md). Only do the phase I ask for. Do not start the next phase.
- At the start of every session: read this file and docs/PROGRESS.md.
- At the end of every phase: server builds warning-free, tests pass, the Windows client exports, then update docs/PROGRESS.md (done / stubbed / next) and commit.
- If something is ambiguous, make a reasonable choice, log it in docs/DECISIONS.md, and keep going instead of stopping to ask.
- Prefer clear, modular code. Keep game rules (combat math, spell resolution, XP/loot formulas) in pure, unit-tested C++ functions, separate from networking and I/O.
- The server must never crash on malformed client input.

## Architecture
- Authoritative C++ server. The client sends intents and the server validates and simulates everything.
- Client: Godot 4.3+, GDScript with static typing (NOT C#). Third-person 3D low-poly preferred; 2D top-down fallback acceptable if scope demands. Tab-target combat.
- Server: C++20, CMake >= 3.22, GCC 12+/Clang 15+ on Linux (Ubuntu/Debian). No Windows/macOS-only deps. Single process: 20 Hz fixed-tick game loop on the main thread, a DB worker thread for persistence, ENet for networking.
- Dependencies (vcpkg manifest mode or FetchContent; pick one and document it): enet, nlohmann/json, SQLite3, spdlog, libsodium (Argon2id), Catch2. entt is optional.
- Transport: ENet. Channel 0 = reliable ordered (commands, chat, inventory). Channel 1 = unreliable (position/snapshots). The client uses Godot's LOW-LEVEL ENet classes (ENetConnection), NOT the high-level multiplayer/RPC API, so the wire format is fully ours.
- Protocol: /protocol holds one human-readable message-definition file (YAML/JSON). tools/gen_protocol.py generates C++ headers and GDScript from it. Binary little-endian, bounds-checked everywhere. CI fails if generated files are stale. The handshake carries a protocol version, and mismatches get a clear "please update your client" message.
- Content: all game data is JSON under /content (classes, spells, items, mobs, zones, loot, quests, factions), data-driven. Adding content never requires code changes. The server validates content at startup and fails fast. tools/validate_content.py checks schemas and ID references. The client bundles a synced copy of what it needs for display.
- Persistence: SQLite (WAL) behind a repository interface (Postgres-swappable later). Autosave every 60 s and on logout/zone change, on the DB worker thread so the tick never blocks.
- Config: file + env overrides (port, db path, content path, max players, admin password, tick rate). Graceful SIGTERM shutdown (save everyone). spdlog with rotation. Small health/admin endpoint (players online, tick time, memory).
- Quality: -Wall -Wextra -Wpedantic, ASan/UBSan CI build, clang-tidy config, RAII/smart pointers, an allocation-light tick loop.

## Repo layout
/server  /client  /protocol  /content  /tools  /docs  /deploy  /.github/workflows

## Game design rules
- MAX_LEVEL = 60 (single config constant). XP curve is exponential-ish; levels 50->60 take about as long as 1->50. Formula lives in config.
- Tab-target combat: auto-attack, hotbar abilities, cooldowns, mana/endurance, global cooldown.
- Groups up to 6 with shared XP. Mob "con" colors (grey/green/blue/white/yellow/red) from level difference; grey gives no XP.
- Death: respawn at bind point with an XP penalty; corpse holds gear for 1 hour real-time and only the owner can loot it. Cleric resurrection restores some lost XP.
- Threat/aggro: damage, taunts, healing, and buffs generate threat. Mobs assist allies and leash to spawn.
- Crowd control: mez, root, snare, stun, slow, fear, charm, with resist checks and break-on-damage where appropriate.
- Out-of-combat sit/rest regen.

## Classes (14, classic archetypes, ORIGINAL ability names)
Melee/Tank: Warrior, Paladin, Shadow Knight
Hybrid: Ranger, Bard
Melee DPS: Rogue, Monk
Healers: Cleric, Druid, Shaman
Casters: Wizard, Magician, Necromancer, Enchanter

Identity:
- Warrior: highest HP/defense, taunts, defensive disciplines, no spells.
- Paladin: tank/healer hybrid, stuns, self-heals, undead-slaying, faith buffs.
- Shadow Knight: tank/necro hybrid, life tap, fear, harm-touch, lesser undead pet.
- Ranger: bow DPS, tracking, snares, nature buffs, light heals, DoTs.
- Bard: songs that tick every few seconds (regen, haste, run speed, mez, DoT), no mana, song-twisting possible.
- Rogue: positional backstab, sneak/hide, evade, pick lock, poisons.
- Monk: unarmed, feign death, fastest melee, high avoidance, mend.
- Cleric: best direct heals, HP/armor buffs, resurrection, undead nukes, stuns.
- Druid: heal-over-time, root/snare, nature DoTs/nukes, hub teleports, damage shields.
- Shaman: slows, haste and stat buffs, DoTs, spirit companion.
- Wizard: highest burst nukes, teleport/evacuate, gather-mana.
- Magician: elemental pets (fire/water/earth/air), summoned items, damage shields.
- Necromancer: DoTs, life drain, undead pets, feign death, HP-to-mana.
- Enchanter: best CC (mez, charm), haste/mana-regen buffs, slows, illusions.

Abilities: 25-30+ per class across levels 1-60 (first at level 1, or level 8 for pure casters; a new one at least every 2-3 levels; tiered ranks). Each has: id, name, class/level, type (nuke/heal/DoT/HoT/buff/debuff/CC/pet/utility/passive/discipline), cost, cast time, cooldown, range, target type, effects (scaling with level and stat), duration, resist type (magic/fire/cold/poison/disease), description. Spells are learned from scrolls (drops/vendors/trainers) and memorized into gem slots (2 at start, up to 8 for casters). Passive skills (dual wield, double attack, dodge, parry, riposte, enhanced damage) unlock by level and skill up through use. Spell engine supports stacking rules, 15 buff slots, debuff slots, dispels, resist rolls, cast interrupts (concentration check), and DoT/HoT ticks.

## Races
6-8 original races with small stat modifiers and class restrictions. Starting cities tie to race groups.

## World
12+ outdoor zones and 8+ dungeons covering levels 1-60, connected by zone lines, each a separate server instance. 3 starting hubs (safe: bank, vendors, trainers, bind point, trade board).
Level bands: 1-15 (3-4 zones, 2 dungeons), 15-30 (3-4 zones, 2-3 dungeons), 30-45 (3 zones, 2-3 dungeons), 45-60 (2-3 zones, 2-3 dungeons incl. 2 raid-tier). Dungeons have trash, named rares, and a boss with unique drops and at least one mechanic. Travel via boats/teleport stones/druid+wizard portals. Hubs are hand-authored; dungeon layouts are seeded procedural (server and client build from the same seed). Server collision/pathing data comes from the same source as the client scenes (document the pipeline).

## Mobs, NPCs, factions, quests
150+ mob types from archetypes with level tiers (level, HP, damage, attack speed, resists, faction, aggro radius, assist, loot table, special abilities). Factions affect whether guards/mobs attack on sight. NPC vendors, trainers, bankers, quest givers, guards. 30+ quests (kill/fetch/deliver/chains) including one class epic chain per class ending in a powerful weapon at ~level 46-50.

## Items
Slots: head, face, neck, shoulders, back, chest, arms, wrists x2, hands, fingers x2, waist, legs, feet, primary, off-hand, ranged, ammo, ears x2; 8 general inventory slots holding bags. Stats: AC, HP, mana, STR/STA/AGI/DEX/WIS/INT/CHA, resists, haste, weapon damage/delay, procs/click effects. Rarity: common/uncommon/rare/epic. Flags: no-trade, lore, magic, class/race/level restrictions. 600+ items (1H/2H slash/pierce/blunt, bows, hand-to-hand, cloth/leather/chain/plate, jewelry, shields, consumables, scrolls, quest items, tradeskill components, bags). Per-mob loot tables with rare drops and coin; group loot (round-robin or need/greed). Secure trade window, bank, player auction board. Tradeskills are a stretch goal.

## Social/multiplayer
Accounts with Argon2id; 4 characters per account. Chat: say, shout, tell, group, guild, OOC, emotes. Groups, guilds (ranks, chat), friends. Commands: /who /loc /sit /consider /assist /target. Anti-cheat: server-side movement/teleport validation, cooldown and range checks, packet rate limiting, bounds-checked parsing. Admin commands (/gm setlevel, spawn, item, tp) gated by an admin flag.

## Build and deploy
- Server: CMakePresets.json (debug, release, asan) -> `emberfall-server`. /deploy has a multi-stage Dockerfile, docker-compose.yml, a systemd unit, and a SQLite backup script. docs/DEPLOY.md covers a fresh Ubuntu VPS.
- Client: committed export_presets.cfg ("Windows Desktop" x86_64, embedded PCK if practical, icon, product name, version). Scriptable export: `godot --headless --path client --export-release "Windows Desktop" build/EmberfallClient.exe`. Document export templates and exporting from Linux and from Windows. Optional Inno Setup installer in /deploy/windows. The portable .exe is always produced. Note that unsigned builds trigger SmartScreen.
- CI (GitHub Actions): Linux job builds/tests the server (incl. ASan/UBSan), validates content, checks protocol sync, and builds the Docker image; a second job exports the Windows client and uploads artifacts.
