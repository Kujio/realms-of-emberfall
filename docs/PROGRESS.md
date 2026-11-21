# Progress

Work one phase at a time (see CLAUDE.md). At the end of each phase, record **Done / Stubbed / Next** under it.

## Phase checklist

- [ ] Phase 0: Repository setup (monorepo layout, CLAUDE.md, docs, .gitignore/.gitattributes, first commit)
- [ ] Phase 1: Build skeleton and CI (CMakePresets debug/release/asan, dependency manager, warning-free empty `emberfall-server`, Catch2 harness, clang-tidy, Godot project + export_presets.cfg, GitHub Actions for server and Windows export)
- [ ] Phase 2: Protocol (message-definition file, tools/gen_protocol.py → C++ and GDScript, bounds-checked binary codec, version handshake, CI staleness check)
- [ ] Phase 3: Server core (20 Hz tick loop, ENet channels, config + env overrides, spdlog, SIGTERM shutdown, health/admin endpoint, packet rate limiting)
- [ ] Phase 4: Accounts and persistence (SQLite WAL repository, DB worker thread, Argon2id login, 4 characters per account, character create/select, autosave)
- [ ] Phase 5: Client core (ENetConnection, login/char select UI, zone load, third-person camera, movement intents, snapshot interpolation)
- [ ] Phase 6: Content pipeline (JSON schemas, tools/validate_content.py, server startup validation, client content sync)
- [ ] Phase 7: World and zones (zone instances, zone lines, entity replication, movement validation, hubs, seeded procedural dungeons, collision/pathing pipeline)
- [ ] Phase 8: Characters and progression (races, 14 classes, stats, XP curve to MAX_LEVEL 60, con colors, passive skills and skill-ups)
- [ ] Phase 9: Combat (tab-target, auto-attack, hotbar, GCD/cooldowns, mana/endurance, threat, death/corpse/bind, sit/rest regen)
- [ ] Phase 10: Spell engine (gems, scrolls, buff/debuff slots, stacking, resists, interrupts, DoT/HoT, CC, pets, bard songs)
- [ ] Phase 11: NPCs and AI (spawns, aggro/assist/leash, pathing, factions, vendors, trainers, bankers, guards)
- [ ] Phase 12: Items and economy (equipment slots, bags, loot tables, group loot, secure trade, bank, auction board)
- [ ] Phase 13: Social (chat channels, emotes, groups with shared XP, guilds, friends, slash commands, GM commands)
- [ ] Phase 14: Content build-out (races, 25-30+ abilities per class, 150+ mobs, 600+ items, 12+ zones, 8+ dungeons, 30+ quests, class epics)
- [ ] Phase 15: Deploy (Dockerfile, docker-compose, systemd unit, SQLite backups, docs/DEPLOY.md, optional Inno Setup installer)
- [ ] Phase 16: Polish and balance (playtesting, tuning, performance, release build)

## Phase notes

<!-- Add Done / Stubbed / Next per phase as each one completes. -->
