# Realms of Emberfall 0.1.0

The first release: a complete level 1-60 game loop on an authoritative server, with placeholder art.

## Downloads

- `EmberfallSetup-0.1.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.1.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is in it

- **14 classes** with 25-30+ original abilities each (spells from scrolls into up to 8 gems, songs,
  disciplines, pets, positionals, stealth, feign death, charm, illusions), **8 races**, 3 starting cities.
- **Combat:** tab-target auto-attack, hotbar abilities, cooldowns and a global cooldown, threat and
  taunts, crowd control (mez, root, snare, stun, slow, fear, charm) with resists, buffs and debuffs with
  stacking rules, DoTs and HoTs, cast interrupts, sit/rest regeneration.
- **Progression:** levels 1-60 with con colours, group experience for up to six (with a bonus per
  member and tougher mobs worth more), death with an experience penalty and a corpse run, resurrection.
- **World:** 3 hubs, 13 outdoor zones and 10 procedural dungeons (2 raid-tier) across four level bands,
  connected by zone lines, ferries, teleport stones and Druid and Wizard portals; 160+ mob types with
  factions and special abilities; dungeon bosses with mechanics.
- **Items:** 700+ items across every slot, weapon kind and armor weight, procs and click effects, loot
  tables with rare drops, need/greed and round-robin group loot, vendors, a bank, secure trade and an
  auction board.
- **Quests:** 81 quests (kill, collect, deliver chains) including an epic chain per class.
- **Social:** say, shout, tells, group, guild, OOC and emotes; groups, guilds with ranks, friends and
  ignore lists, `/who`; game master tools with an audit log, bans and mutes.
- **Client:** settings (window mode, resolution, vsync, volumes, rebindable keys), a loading screen,
  clear disconnect messages, the server's message of the day and population on the login screen,
  placeholder sound effects.
- **Server operations:** Docker and systemd deployments, online backups with integrity checks, refusal
  to start on a damaged database, a health endpoint with tick percentiles, CPU and bandwidth, graceful
  shutdown that saves everyone, a 30 s linger for characters dropped mid-fight.

## Measured

- **Balance:** every class reaches 60 within 0.76-1.15x of the median solo time and 0.94-1.08x grouped
  (simulated; details in docs/DECISIONS.md). Grouping is the faster way to level.
- **Load:** 300 simulated players in four zones: tick mean 8.1 ms, p99 11.5 ms (budget 50 ms),
  ~100 MB of memory, ~17 KB/s sent per player.

## Known limitations

- Placeholder art and sound everywhere (the art pass is next).
- Game traffic after login is not encrypted (passwords are: they only travel over TLS).
- Groups are not saved across a server restart; tradeskills, a guild bank and raids beyond the two
  raid-tier dungeons are not in yet.
- Warriors and Monks level fastest solo; priests slowest (see docs/DECISIONS.md).

## Next

1. **Art pass** (Phase 9): one art direction for characters, equipment, mobs and pets; a spell effect
   library; per-biome environments; a UI theme and icons; real audio and music; graphics settings; a
   Credits screen.
2. **Tradeskills:** gathering from trade loot, recipes, crafting stations, skill-ups (the components
   already drop).
3. **Guild bank** with rank permissions and an audit trail.
4. **Raids:** raid groups of several groups, raid loot rules, lockouts, and encounters with phases.
5. **Encrypting game traffic** after login (a session key from the TLS login).
6. **Persistent groups** and group finder ("looking for group" on `/who`).
7. **Class tuning from live data** (the simulator's autopilot does not crowd-control, pull or kite).
