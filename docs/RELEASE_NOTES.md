# Realms of Emberfall 0.2.0

The art pass: the whole game now has one low-poly, flat-shaded look, real music and sound, and a
graphics quality setting. Gameplay is unchanged from 0.1.0 (protocol v14: update both client and server).

## Downloads

- `EmberfallSetup-0.2.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.2.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Characters:** Polyfork models for every race and gender (CC BY 4.0) with retargeted CC0
  animations; your chest armour picks the outfit and its level tints it, and helmets show. Other
  players see your gear change.
- **Mobs, pets and NPCs:** 43 monster and townsfolk models and 19 animals in the same style.
- **Spell effects:** casts, projectiles, impacts, heals, buffs, ground rings and crowd-control markers
  coloured by school; screen cues for low health, crowd control and level ups.
- **Environments:** trees, rocks, buildings and ruins per biome and per city, ground clutter, dungeon
  decoration, and each biome's own light, fog and sky.
- **Interface:** one theme over every window, icons for spells and items, scaling from 720p to 4K.
- **Audio:** music and ambience per zone, boss and battle themes, footsteps per surface; Master, Music,
  Ambient, Effects and Interface volumes.
- **Graphics quality:** Low, Medium and High (shadows, anti-aliasing, draw distances, clutter, effects).
- **Credits** on the login screen and in the game menu, with every asset's author and licence.

## Measured

- **FPS** in a hub with 100 simulated players (about 230 characters in view, 1920x1080, Intel Iris Xe
  integrated graphics): Low 47, Medium 33, High 30 on average.
- **Download size:** about 180 MB.

## Known limitations

- Some race and gender combinations borrow another's model; snakes, a few monsters and dungeon walls
  are still placeholders (docs/ART_TODO.md).
- Game traffic after login is not encrypted (passwords are: they only travel over TLS).
- Tradeskills, a guild bank and raids beyond the two raid-tier dungeons are not in yet.

## Next

1. **The art still missing** (docs/ART_TODO.md): snakes, female and race-specific outfits, dungeon
   walls, water, unique signature-spell effects.
2. **Tradeskills**, a **guild bank**, **raids** with lockouts.
3. **Encrypting game traffic** after login; **persistent groups** and a group finder.

# Realms of Emberfall 0.1.0

The first release: a complete level 1-60 game loop on an authoritative server, with placeholder art.

### What is in it

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

### Measured

- **Balance:** every class reaches 60 within 0.76-1.15x of the median solo time and 0.94-1.08x grouped
  (simulated; details in docs/DECISIONS.md). Grouping is the faster way to level.
- **Load:** 300 simulated players in four zones: tick mean 6.5 ms, p99 9.5 ms (budget 50 ms),
  ~100 MB of memory, ~15 KB/s sent per player.

### Known limitations

- Placeholder art and sound everywhere (the art pass is next).
- Game traffic after login is not encrypted (passwords are: they only travel over TLS).
- Groups are not saved across a server restart; tradeskills, a guild bank and raids beyond the two
  raid-tier dungeons are not in yet.
- Warriors and Monks level fastest solo; priests slowest (see docs/DECISIONS.md).

