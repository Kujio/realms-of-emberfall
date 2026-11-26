# Realms of Emberfall 0.9.0

Supplies and know-how. The protocol changed (v20), so update both the client and the server. The
database gains a column (schema v12); the server upgrades an older database in place on its first start.

## Downloads

- `EmberfallSetup-0.9.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.9.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Arrows run out.** Every bow shot uses an arrow from your ammo slot. You are warned at ten left and
  when the last one flies. Fletchers make arrows, and vendors sell them.
- **Recipe books.** The first recipes of every craft are known from the start. Later ones are taught by
  recipe books, one per craft for each band of 50 skill points: read one from your pack to learn its
  recipes. The crafting window shows recipes you have not learned yet dimmed, with the book that
  teaches them. The second volume of each craft is sold by your city's supplier; the rest are quest
  rewards and monster drops.
- **Crafting quests.** A master artisan stands in the middle of each city's crafting yard with quests
  for two crafts: make something, then something harder, and earn the craft's next recipe books.
- **Combines take time.** Each combine takes a couple of seconds, with a progress bar and your
  character at work. Moving, fighting or casting stops it, and nothing is used.
- **Target reticle.** A coloured ring now marks whatever you have targeted: a monster's difficulty
  colour, green for players, gold for NPCs.
- **Name tags** stay a readable size: smaller right in front of the camera, a little larger far away.

## Known limitations

- Other players do not see you working at a station (only your own character moves).
- Everything listed for 0.8.0 and earlier below still applies.

# Realms of Emberfall 0.8.0

Raid rules and smarter fights. The protocol changed (v19), so update both the client and the server.
The database gains two columns (schema v11); the server upgrades an older database in place on its
first start.

## Downloads

- `EmberfallSetup-0.8.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.8.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Raid lockouts.** Sharing the kill of a raid dungeon's boss locks you out of its treasure for 20
  hours (real time, so it runs while you are offline). Locked out, you can still help: you earn the
  experience and quest credit, but the coin and items go to the raiders who were not locked out.
  `/lockouts` lists yours.
- **Raid dungeons are for raids.** The Obsidian Citadel and the Emberwyrm's Lair let in only raids.
- **Main assist.** The raid leader names one (`/raidassist NAME`, or Main assist in the raid window);
  `/assist` with no name, or Assist in the raid window, takes their target.
- **Target marks.** The raid leader or main assist marks up to three monsters (`/mark 1`-`3` on your
  target, `/mark clear`, or Mark target in the raid window). Every raider sees the mark above the
  monster's name.
- **Monsters help each other.** Cultists mend their wounded, mirefolk tidecallers renew theirs, skulk
  warlords drive their war band to strike harder, and High Priest Vaurn mends his servants.
- **Fear makes you run.** A feared character now runs away from whatever frightened them, turning
  aside at walls, instead of standing frozen.
- **Gentler first steps.** Every starting city now has level 1-3 creatures just outside its gate:
  field rats, burrow beetles, spiderlings, hill rooks and wild piglets.

## Known limitations

- Raid-only entry is checked at the dungeon's door: someone who logs in inside, or whose raid
  disbands inside, stays.
- Everything listed for 0.7.0 and earlier below still applies, except that raid dungeons now have
  lockouts and raid-only entry.

## Next

1. More art (dungeon wall pieces, landmark buildings, snakes), and the remaining polish items (arrows
   used up, name tags scaled for distance, recipe books).

# Realms of Emberfall 0.7.0

Raids. The protocol changed (v18), so update both the client and the server. The database gains two
tables (schema v10); the server upgrades an older database in place on its first start.

## Downloads

- `EmberfallSetup-0.7.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.7.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Raids.** Up to twelve groups (72 players) join together for the Obsidian Citadel and the
  Emberwyrm's Lair. A group leader targets another group's leader and invites their group (the
  invitation pops up; /raidinvite NAME works too). Joining a group in a raid joins the raid.
- **The raid window (Y).** Every group side by side, with each raider's health, level and class; click
  a raider to target them. The raid leader removes groups, hands over the lead, picks the loot rule
  and disbands; a group leader takes their group out.
- **One party.** Everyone in the raid at a kill shares the experience, the coin, quest and faction
  credit, and the monster's attention. The experience bonus counts no more members than a full group,
  so a raid pays out what a group would, split more ways.
- **Master loot.** By default the raid leader holds a raid kill's items and hands each one to a raider
  who was there (Give, in the loot window); everyone hears where it went, including what the leader
  keeps. If the master looter leaves or dies, the job passes to someone still there. The raid can roll need
  before greed instead (/raidloot ng).
- **Raid chat.** /rsay (/rs) and a Raid tab in the chat window.
- **Raids keep.** Like groups, a raid waits for its members across logouts and server restarts. When
  the raid leader goes offline the lead passes to someone online.

## Known limitations

- There are no raid lockouts or raid-only rules for the raid dungeons: a strong group can still try
  them alone.
- Everything listed for 0.6.0 and earlier below still applies.

## Next

1. More art (dungeon wall pieces, landmark buildings, snakes), and the remaining polish items (arrows
   used up, name tags scaled for distance, recipe books).

# Realms of Emberfall 0.6.0

Tradeskills. The protocol changed (v17), so update both the client and the server. The database is
unchanged (schema v9).

## Downloads

- `EmberfallSetup-0.6.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.6.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Six tradeskills.** Smithing, tailoring, cooking, brewing, fletching and jewelcraft. Each city has
  a crafting yard with a forge, a loom, an oven, a brewing vat, a fletching bench and a jeweler's
  bench, and a supplier who sells the basics and the tools.
- **Make what you need.** Armour of every weight and tier, weapons, shields, jewellery, cloaks, bags,
  bandages, food and drink, potions and arrows: over 650 recipes. Rare pieces need rare drops, and
  each craft's masterwork teaches that skill up to 300.
- **Materials from the world.** Hides and meat from beasts, ore from golems and giants, herbs, gems,
  silk and heartwood, each tier from monsters of its level band.
- **Skill through practice.** Each combine may teach you something until the recipe is trivial for
  you. A failure can cost materials, but never your tools. Talk to a station (T) to open it; your
  tradeskills are also in the skills window (J).

## Known limitations

- A combine is instant, and every recipe is known from the start.
- Everything listed for 0.5.0 and earlier below still applies.

## Next

1. Raid tools (0.7.0), and more art (dungeon wall pieces, landmark buildings, snakes).

# Realms of Emberfall 0.5.0

Towns, groups and the map. The protocol changed (v16) and the database gains two tables (schema v9,
upgraded in place at the first start), so update both the client and the server.

## Downloads

- `EmberfallSetup-0.5.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.5.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Your group waits for you.** Log out, lose your connection, or sit through a server restart, and
  you keep your place in the group for 15 minutes. You're back in it the moment you return. Offline
  members show greyed in the group window, and the lead passes to someone who is online.
- **Guards defend their towns.** Lead a monster to a city's gate and the gate wardens (two of the
  city's guards, posted just outside) attack it and call for help. If they do most of the work, they claim the kill: no experience or loot for anyone.
- **A world map.** The map's World tab now draws the realm: cities and zones joined by their roads,
  coloured by how their levels suit you, with dungeons marked. Click a place to see it and the way
  there. The old list is on the Zones tab.

## Known limitations

- Ferry and teleport routes are listed, not drawn on the map.
- Everything listed for 0.4.0 and earlier below still applies.

## Next

1. More art (dungeon wall pieces, landmark buildings, snakes), tradeskills, raid tools.

# Realms of Emberfall 0.4.0

The world's surfaces: the ground, water, sky and dungeons are textured. Gameplay and the protocol
(v15) are unchanged from 0.3.0, so a 0.3.0 server works with this client.

## Downloads

- `EmberfallSetup-0.4.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.4.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Textured ground** in every zone: grass, earth, sand, snow, mud and ash by region, with rock on
  steep slopes and on the heights, and no visible repeating pattern.
- **Water:** pools in the Gloomfen Bog and the Saltmarsh Fens, and the Duskmere's sea off Duskmere
  Shore, with a wet band along the water's edge.
- **A real horizon:** the land continues past each zone's edge into hills, or down under the sea.
- **Skies with clouds** and a visible sun, overcast in the bog and the ash lands, clear over the
  badlands.
- **Dungeons:** brick, flagstone, rough cave rock, carved blocks and tiles, by dungeon.

## Measured

- About 1 ms more per frame outdoors on integrated graphics (Intel Iris Xe: 124 to 143 FPS in an
  empty view, from 146 to 161). Cities and dungeons are unchanged.
- The client download is 3 MB larger (183 MB).

## Known limitations

- The water is for looking at: nobody swims, and it does not slow anyone.
- Clouds do not move.
- Everything listed for 0.3.0 and 0.2.0 below still applies.

# Realms of Emberfall 0.3.0

Security: everything the game sends after you log in is now encrypted. Gameplay and art are unchanged
from 0.2.0, but the protocol changed (v15), so update both the client and the server.

## Downloads

- `EmberfallSetup-0.3.0.exe`: Windows installer (per-user, no administrator prompt).
- `EmberfallClient.exe`: the same client as one portable file.
- `emberfall-server-0.3.0-docker.tar.gz`: the server image (`docker load < file`), also on GHCR.
- `SHA256SUMS.txt`: checksums.

The Windows builds are not code-signed: SmartScreen asks once (More info → Run anyway).

## What is new

- **Encrypted game traffic:** chat, tells, movement, trades and everything else after login is
  encrypted and authenticated. The key comes over the same TLS login that protects your password,
  so it only ever reaches the server you trusted. A forged or replayed packet is refused, and a
  tampered connection is closed.
- **Old clients** get a clear "please update your client" message.

## Measured

- The server's tick time with 150 players is unchanged (3.4 ms average, 5.1 ms p99 of a 50 ms budget).
- Each packet is 24 bytes larger.

## Known limitations

- The connection's opening exchange is not encrypted: the version check, the server certificate,
  the message of the day, and the one-use login ticket the client hands over just before
  encryption starts. The ticket is no password: it works once, for 30 seconds, and only on the
  connection it was issued for, and whoever presents it gets a connection sealed with a key only
  the real client has.
- Everything listed for 0.2.0 below still applies, except encryption.

3. **Tradeskills**, a **guild bank**, **raids** with lockouts.

# Realms of Emberfall 0.2.0

The art pass: the whole game now has one low-poly, flat-shaded look, real music and sound, and a
graphics quality setting. Gameplay is unchanged from 0.1.0 (protocol v14: update both client and server).

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

