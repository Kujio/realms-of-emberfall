#!/usr/bin/env python3
"""World generator: writes the outdoor and dungeon zone files (content/zones/<id>.json), the world's
NPCs (hub services, soulbinders, waystone keepers, ferries, outposts) into content/npcs.json, and the
travel points (content/travel.json) from the specs below.

Hubs are hand-authored Godot scenes (client/zones/<id>.tscn, exported by client/tools/export_zones.gd);
their zone files here carry only metadata, and their NPCs stand where the scene's npc markers put them.

Outdoor zones get round hills, scattered props from tools/world_props.py (collision from the same table
the prop scenes use), camps of mobs whose level rises across the zone's band, entries and zone lines on
the sides that link to neighbours, roads between the entries, and features: dungeon mouths, Druid rings
and Wizard spires (travel points), outposts with vendors and a soulbinder, and ferry docks.
Dungeons get their seeded layout parameters (the server and client build the rooms) and room spawns.
Everything is deterministic: the same specs give the same files.

  python3 tools/gen_world.py          # write
  python3 tools/gen_world.py --check  # exit 1 if a committed file is stale
"""
from __future__ import annotations

import json
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from npc_order import render as render_npcs  # noqa: E402
from world_props import PROPS  # noqa: E402
from zone_format import format_zone  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
ZONES = CONTENT / "zones"

E, W, N, S = -math.pi / 2, math.pi / 2, 0.0, math.pi  # yaw facing east (+x), west (-x), north (-z), south (+z)
FACING = {"west": E, "east": W, "north": S, "south": N}  # arriving at a side, you face into the zone

# ------------------------------------------------------------------------------------------- hubs
# Each hub's gates (zone lines and arrival entries) are in its scene.
HUBS = [
    {"id": "emberhold", "name": "Emberhold", "half": 200.0, "spawn": (0.0, 15.0, S),
     "description": "A walled river city built around the Great Forge, whose fire has not gone out in four hundred years.",
     "theme": {"biome": "town", "ground": "#6b6350", "sky": "#4a6491", "fog": "#b39b85"}},
    {"id": "thornwood", "name": "Thornwood", "half": 200.0, "spawn": (0.0, 20.0, N),
     "description": "Halls woven from living briar beneath the Heartwood, the oldest tree in the realm, ringed by its ancient daughters.",
     "theme": {"biome": "grove", "ground": "#4d6a35", "sky": "#6a8fb0", "fog": "#a9c0a0"}},
    {"id": "cragmouth", "name": "Cragmouth", "half": 200.0, "spawn": (0.0, 0.0, S),
     "description": "A fortress-city carved into a mountain's jaw, its stone halls warmed by forges that never cool.",
     "theme": {"biome": "mountain", "ground": "#6a645c", "sky": "#58708f", "fog": "#9c958c"}},
]

# ------------------------------------------------------------------------------------------- outdoor zones
# scatter: (prop, count per 100x100 m); hills: count and height range; camps: how many.
BIOMES = {
    "meadow": {"scatter": [("tree_oak", 2.4), ("rock", 1.1), ("boulder", 0.2), ("reeds", 0.6)], "hills": (22, 2.0, 9.0)},
    "forest": {"scatter": [("tree_oak", 3.5), ("tree_pine", 3.0), ("rock", 0.6), ("mushroom", 0.4)], "hills": (18, 2.0, 8.0)},
    "hills": {"scatter": [("tree_pine", 1.6), ("rock", 1.6), ("boulder", 0.6)], "hills": (30, 6.0, 18.0)},
    "marsh": {"scatter": [("reeds", 3.0), ("tree_dead", 1.0), ("mushroom", 0.5), ("rock", 0.3)], "hills": (14, -2.5, 3.0)},
    "shore": {"scatter": [("tree_pine", 1.2), ("rock", 1.0), ("reeds", 1.2), ("boulder", 0.2)], "hills": (16, 1.0, 7.0)},
    "badlands": {"scatter": [("boulder", 0.9), ("rock", 1.6), ("tree_dead", 0.5), ("ruin_pillar", 0.1)], "hills": (26, 4.0, 16.0)},
    "bog": {"scatter": [("tree_dead", 1.8), ("mushroom", 1.2), ("reeds", 1.8)], "hills": (12, -3.0, 3.0)},
    "pinewood": {"scatter": [("tree_pine", 5.0), ("rock", 0.7), ("boulder", 0.2)], "hills": (20, 3.0, 12.0)},
    "steppe": {"scatter": [("rock", 0.9), ("boulder", 0.3), ("ruin_wall", 0.08), ("ruin_pillar", 0.12)], "hills": (16, 2.0, 10.0)},
    "tundra": {"scatter": [("crystal", 0.5), ("rock", 1.0), ("boulder", 0.5), ("tree_pine", 0.6)], "hills": (22, 3.0, 14.0)},
    "wastes": {"scatter": [("boulder", 0.7), ("rock", 1.4), ("tree_dead", 0.4), ("brazier", 0.03)], "hills": (24, 3.0, 15.0)},
    "ashland": {"scatter": [("tree_dead", 1.0), ("boulder", 0.8), ("rock", 1.2), ("ruin_pillar", 0.15)], "hills": (24, 4.0, 18.0)},
    "peaks": {"scatter": [("boulder", 1.2), ("rock", 1.4), ("tree_pine", 0.8), ("crystal", 0.2)], "hills": (36, 10.0, 30.0)},
}

OUTDOOR = [
    {"id": "emberglade", "name": "The Emberglade", "levels": (1, 8), "half": 400.0, "seed": 101, "biome": "meadow", "camps": 14,
     "description": "Rolling meadows east of Emberhold, where ember rats nest in the warm earth and brigands watch the road.",
     "theme": {"ground": "#5d7a3e", "sky": "#5a7fb5", "fog": "#c4c9b0"}},
    {"id": "briarwatch_wood", "name": "Briarwatch Wood", "levels": (1, 8), "half": 400.0, "seed": 102, "biome": "forest", "camps": 14,
     "description": "The young forest north of Thornwood, watched over by the briar-wardens and thick with wolves and spiders.",
     "theme": {"ground": "#466b33", "sky": "#6a8fb0", "fog": "#a8bda0"}},
    {"id": "cragfoot_hills", "name": "Cragfoot Hills", "levels": (1, 8), "half": 400.0, "seed": 103, "biome": "hills", "camps": 14,
     "description": "Wind-scoured hills below Cragmouth's gate, where goats graze and cave beetles burrow.",
     "theme": {"ground": "#6d7350", "high": "#8a8478", "sky": "#58708f", "fog": "#a8a294"}},
    {"id": "saltmarsh_fens", "name": "The Saltmarsh Fens", "levels": (8, 15), "half": 450.0, "seed": 104, "biome": "marsh", "camps": 16,
     "description": "Tidal fens where the three roads meet: reed-adders in the shallows and a drowned watchtower on every rise.",
     "theme": {"ground": "#5a6b45", "sky": "#7a90a8", "fog": "#9aa89a"}},
    {"id": "duskmere_shore", "name": "Duskmere Shore", "levels": (15, 22), "half": 450.0, "seed": 105, "biome": "shore", "camps": 16,
     "description": "The long grey shore of the Duskmere, where ferries land and the old lighthouse keeps a lonely watch.",
     "theme": {"ground": "#6e7556", "sky": "#6c86a6", "fog": "#9fa8ad"}},
    {"id": "redrock_badlands", "name": "The Redrock Badlands", "levels": (18, 26), "half": 500.0, "seed": 106, "biome": "badlands", "camps": 18,
     "description": "Sun-baked mesas of red stone, home to ogre clans and the mines they have taken for their own.",
     "theme": {"ground": "#8a5a3a", "high": "#a0705a", "sky": "#c08860", "fog": "#c4a080"}},
    {"id": "hollowpine_forest", "name": "Hollowpine Forest", "levels": (20, 30), "half": 500.0, "seed": 107, "biome": "pinewood", "camps": 18,
     "description": "A dark pine forest whose trees grow hollow with age, where wolves run in packs and treants stir.",
     "theme": {"ground": "#34513a", "sky": "#4f6880", "fog": "#6d8070"}},
    {"id": "gloomfen_bog", "name": "Gloomfen Bog", "levels": (22, 30), "half": 450.0, "seed": 108, "biome": "bog", "camps": 16,
     "description": "A sour black bog where the drowned do not rest and things with too many eyes wait beneath the water.",
     "theme": {"ground": "#3d4a33", "sky": "#4d5a5a", "fog": "#5a6655"}},
    {"id": "shattered_steppe", "name": "The Shattered Steppe", "levels": (30, 38), "half": 550.0, "seed": 109, "biome": "steppe", "camps": 20,
     "description": "Endless grass broken by the ruins of a fallen empire, roamed by drakes and the cultists who serve them.",
     "theme": {"ground": "#7d8050", "sky": "#7896b4", "fog": "#b0b090"}},
    {"id": "frostveil_tundra", "name": "Frostveil Tundra", "levels": (35, 45), "half": 550.0, "seed": 110, "biome": "tundra", "camps": 20,
     "description": "A white waste of ice and wind where the frost wraiths sing and crystals grow from the snow.",
     "theme": {"ground": "#d6dde3", "high": "#f0f4f8", "sky": "#9ab0c8", "fog": "#dfe6ee"}},
    {"id": "cinderspire_wastes", "name": "The Cinderspire Wastes", "levels": (38, 45), "half": 550.0, "seed": 111, "biome": "wastes", "camps": 20,
     "description": "Black glass and smoking vents beneath the Cinderspire, where fire imps dance and obsidian walks.",
     "theme": {"ground": "#3e3430", "high": "#5a4038", "sky": "#6a3a2a", "fog": "#5a3a30"}},
    {"id": "ashen_reach", "name": "The Ashen Reach", "levels": (45, 52), "half": 600.0, "seed": 112, "biome": "ashland", "camps": 22,
     "description": "A grey land where the Emberwyrm's fire fell long ago; trolls hunt the ash and the deathless keep their vigil.",
     "theme": {"ground": "#5a5650", "high": "#76706a", "sky": "#6a6460", "fog": "#7a746c"}},
    {"id": "stormcrown_peaks", "name": "The Stormcrown Peaks", "levels": (50, 60), "half": 600.0, "seed": 113, "biome": "peaks", "camps": 22,
     "description": "The roof of the world, where lightning never stops and the wyrms' children nest above the clouds.",
     "theme": {"ground": "#6c6e72", "high": "#b8bcc4", "sky": "#40506a", "fog": "#8a90a0"}},
]

# Links: (zone A, side A or hub gate entry, zone B, side B or hub gate entry, offset along the side).
# A hub end names its scene's entry (the gate) and the scene's line names the outdoor entry "from_<hub>".
LINKS = [
    ("emberhold", "hub:east_gate", "emberglade", "west", 0.0),
    ("thornwood", "hub:north_gate", "briarwatch_wood", "south", 0.0),
    ("cragmouth", "hub:south_gate", "cragfoot_hills", "north", 0.0),
    ("emberglade", "north", "saltmarsh_fens", "south", 0.0),
    ("briarwatch_wood", "east", "saltmarsh_fens", "west", 0.0),
    ("cragfoot_hills", "west", "saltmarsh_fens", "east", 0.0),
    ("saltmarsh_fens", "north", "duskmere_shore", "south", 0.0),
    ("duskmere_shore", "east", "redrock_badlands", "west", 0.0),
    ("duskmere_shore", "west", "hollowpine_forest", "east", 0.0),
    ("hollowpine_forest", "north", "gloomfen_bog", "south", 0.0),
    ("redrock_badlands", "north", "shattered_steppe", "south", 0.0),
    ("gloomfen_bog", "east", "shattered_steppe", "west", 0.0),
    ("shattered_steppe", "north", "frostveil_tundra", "south", 0.0),
    ("shattered_steppe", "east", "cinderspire_wastes", "west", 0.0),
    ("cinderspire_wastes", "north", "ashen_reach", "south", 0.0),
    ("frostveil_tundra", "east", "stormcrown_peaks", "west", 0.0),
    ("ashen_reach", "north", "stormcrown_peaks", "south", 200.0),
]

# Features placed in outdoor zones: (zone, kind, x, z, arg). kinds:
#   dungeon  - a cave mouth whose line enters dungeon `arg` (its exit returns to entry "from_<arg>")
#   ring     - a Druid ring of standing stones: travel point `arg`
#   spire    - a Wizard spire of crystal: travel point `arg`
#   outpost  - tents, a fire, and the NPCs in OUTPOSTS[arg]
#   dock     - a pier and a boat, with ferry `arg` (WORLD_NPCS)
FEATURES = [
    ("emberglade", "dungeon", 250.0, -200.0, "rattlebone_warren"),
    ("saltmarsh_fens", "dungeon", -260.0, -250.0, "mirefang_hollow"),
    ("duskmere_shore", "dungeon", 300.0, 200.0, "sunken_bastion"),
    ("redrock_badlands", "dungeon", -250.0, -300.0, "cinderdeep_mines"),
    ("gloomfen_bog", "dungeon", 220.0, -260.0, "hollowmere_crypts"),
    ("shattered_steppe", "dungeon", 300.0, -300.0, "stormspire_ruins"),
    ("frostveil_tundra", "dungeon", -300.0, -250.0, "gloomroot_caverns"),
    ("ashen_reach", "dungeon", 300.0, 300.0, "deathless_sanctum"),
    ("ashen_reach", "dungeon", -350.0, -350.0, "obsidian_citadel"),
    ("stormcrown_peaks", "dungeon", 300.0, -350.0, "emberwyrm_lair"),
    ("emberglade", "ring", -150.0, 220.0, "ring_emberglade"),
    ("briarwatch_wood", "ring", 180.0, -150.0, "ring_briarwatch"),
    ("cragfoot_hills", "spire", -160.0, 120.0, "spire_cragfoot"),
    ("saltmarsh_fens", "outpost", 0.0, 60.0, "saltmarsh_camp"),
    ("duskmere_shore", "dock", 0.0, -330.0, "duskmere_ferry"),
    ("duskmere_shore", "outpost", 60.0, -240.0, "duskmere_camp"),
    ("redrock_badlands", "spire", 220.0, 200.0, "spire_redrock"),
    ("hollowpine_forest", "ring", -200.0, -150.0, "ring_hollowpine"),
    ("shattered_steppe", "outpost", 0.0, 0.0, "steppe_camp"),
    ("shattered_steppe", "spire", -260.0, -260.0, "spire_steppe"),
    ("frostveil_tundra", "ring", 150.0, 250.0, "ring_frostveil"),
    ("ashen_reach", "outpost", -200.0, 150.0, "ashen_camp"),
    ("ashen_reach", "ring", 260.0, -100.0, "ring_ashen"),
    ("stormcrown_peaks", "spire", -250.0, -250.0, "spire_stormcrown"),
]
TRAVEL_NAMES = {
    "ring_emberglade": "The Emberglade Ring", "ring_briarwatch": "The Briarwatch Ring", "ring_hollowpine": "The Hollowpine Ring",
    "ring_frostveil": "The Frostveil Ring", "ring_ashen": "The Ashen Ring",
    "spire_cragfoot": "The Cragfoot Spire", "spire_redrock": "The Redrock Spire", "spire_steppe": "The Steppe Spire",
    "spire_stormcrown": "The Stormcrown Spire",
}

# ------------------------------------------------------------------------------------------- dungeons (6c fills these out)

# Mobs come from content/mobs.json rows tagged with the dungeon (tools/gen_mobs.py): trash fills every room, each
# named rare sometimes takes the place of its trash type in one room, the boss holds the deepest room with two guards.
CRYPT = {"ground": "#4a4038", "wall": "#5a5048", "fog": "#2a2420"}
CAVE = {"ground": "#3e3a34", "wall": "#4c463e", "fog": "#201c18"}
DUNGEONS = [
    {"id": "rattlebone_warren", "name": "Rattlebone Warren", "levels": (2, 11), "seed": 4242, "rooms": 9, "from": "emberglade",
     "description": "Burrows and bone-pits under the Emberglade, where the restless dead of an old battlefield gather.",
     "theme": {"biome": "crypt", **CRYPT}},
    {"id": "mirefang_hollow", "name": "Mirefang Hollow", "levels": (12, 17), "seed": 5151, "rooms": 10, "from": "saltmarsh_fens",
     "description": "A drowned cave under the fens where the mirefolk feed their god, a serpent grown huge on offerings.",
     "theme": {"biome": "cave", "ground": "#3a4434", "wall": "#4a5440", "fog": "#1c241a"}},
    {"id": "sunken_bastion", "name": "The Sunken Bastion", "levels": (18, 24), "seed": 6262, "rooms": 11, "from": "duskmere_shore",
     "description": "A sea fort the Duskmere swallowed whole. Its drowned garrison still keeps the watch.",
     "theme": {"biome": "ruin", "ground": "#3c4a50", "wall": "#4e5c62", "fog": "#16242a"}},
    {"id": "cinderdeep_mines", "name": "The Cinderdeep Mines", "levels": (21, 27), "seed": 7373, "rooms": 12, "from": "redrock_badlands",
     "description": "Ogre-dug tunnels into a seam of living fire, worked by slaves and watched by the Cinder Foreman.",
     "theme": {"biome": "mine", "ground": "#4a3428", "wall": "#5e4030", "fog": "#2a140a"}},
    {"id": "hollowmere_crypts", "name": "The Hollowmere Crypts", "levels": (27, 33), "seed": 8484, "rooms": 12, "from": "gloomfen_bog",
     "description": "The sunken abbey of Hollowmere, whose monks swore to serve beyond death and kept their word.",
     "theme": {"biome": "crypt", **CRYPT}},
    {"id": "stormspire_ruins", "name": "The Stormspire Ruins", "levels": (34, 41), "seed": 9595, "rooms": 13, "from": "shattered_steppe",
     "description": "The broken tower of a fallen empire's storm-mages, reclaimed by cultists who would call the lightning back.",
     "theme": {"biome": "ruin", "ground": "#4a4a50", "wall": "#5e5e66", "fog": "#20202a"}},
    {"id": "gloomroot_caverns", "name": "The Gloomroot Caverns", "levels": (40, 46), "seed": 10606, "rooms": 13, "from": "frostveil_tundra",
     "description": "Warm caves under the ice where roots as thick as towers feed on something old.",
     "theme": {"biome": "cave", **CAVE}},
    {"id": "deathless_sanctum", "name": "The Deathless Sanctum", "levels": (47, 53), "seed": 11717, "rooms": 14, "from": "ashen_reach",
     "description": "The hidden temple of the Deathless, where the faithful give up their mortality one ritual at a time.",
     "theme": {"biome": "temple", "ground": "#3a3640", "wall": "#4c4654", "fog": "#16121c"}},
    {"id": "obsidian_citadel", "name": "The Obsidian Citadel", "levels": (52, 58), "seed": 12828, "rooms": 15, "from": "ashen_reach", "raid": True,
     "description": "A fortress of black glass raised by the Obsidian Tyrant. A raid of many is needed to breach it.",
     "theme": {"biome": "citadel", "ground": "#1e1c22", "wall": "#2c2830", "fog": "#0e0c10"}},
    {"id": "emberwyrm_lair", "name": "The Emberwyrm's Lair", "levels": (55, 60), "seed": 13939, "rooms": 16, "from": "stormcrown_peaks", "raid": True,
     "description": "The burning heart of the Stormcrown, where the Emberwyrm sleeps on the hoard of a thousand years.",
     "theme": {"biome": "lair", "ground": "#3a2218", "wall": "#4e2c1e", "fog": "#200c06"}},
]

# ------------------------------------------------------------------------------------------- world NPCs
# Roles this generator owns in content/npcs.json (it keeps every other row).
WORLD_ROLES = ("binder", "teleporter", "ferry")
WORLD_TAG = "world"  # hub clones of trainers/vendors carry "origin": "world" so they can be told apart

# Hub services: (template id in npcs.json, new name). Templates are the test zone's NPCs; the clones
# get the hub's id prefix, the hub's zone, and the position of the hub scene's npc marker.
HUB_ROSTER = {
    "emberhold": {
        "sister_maelis": "Priestess Anwen Hale", "archmage_torvan": "Magister Corvane", "ser_aldric": "Ser Bertrand Ashcroft",
        "lady_vexa": "Dame Morwenna Vey", "warden_ilsa": "Huntmaster Garrow", "maestro_pell": "Minstrel Teodric",
        "elder_brannoch": "Elder Fenna Root", "seer_ruuka": "Spiritcaller Ugga", "magus_orrin": "Summoner Ilsabet",
        "mortician_sable": "Bonewright Castor", "mesmer_liora": "Mindweaver Selis",
        "banker_odell": "Banker Hollis Marr", "auctioneer_mirabel": "Brokerage Clerk Wenna", "provisioner_brannigan": "Provisioner Dunmore",
        "alchemist_oda": "Alchemist Theda", "outfitter_tamsin": "Outfitter Callum", "jeweler_lysa": "Jeweler Orsolya",
    },
    "thornwood": {
        "sister_maelis": "Hierophant Lirelle", "archmage_torvan": "Starreader Aethis", "ser_aldric": "Thornknight Caelen",
        "warden_ilsa": "Pathwarden Sylvi", "maestro_pell": "Songkeeper Merrow", "elder_brannoch": "Archdruid Oaklen",
        "magus_orrin": "Conjurer Faelith", "mesmer_liora": "Dreamspinner Nyra",
        "banker_odell": "Vaultkeeper Elowen", "auctioneer_mirabel": "Tradewarden Ysra", "provisioner_brannigan": "Forager Penrith",
        "alchemist_oda": "Herbalist Maeve", "outfitter_tamsin": "Leafcutter Aris", "jeweler_lysa": "Gemsinger Talia",
    },
    "cragmouth": {
        "sister_maelis": "High Abbot Durnholm", "archmage_torvan": "Runemaster Kezzik", "ser_aldric": "Oathkeeper Bralda",
        "lady_vexa": "Gravelord Vosk", "seer_ruuka": "Stonecaller Grunda", "magus_orrin": "Forgebinder Tikka",
        "mortician_sable": "Deathmonger Ulrec", "mesmer_liora": "Glamourist Pip Quillon",
        "banker_odell": "Coffermaster Grond", "auctioneer_mirabel": "Haggler Nix Brassbolt", "provisioner_brannigan": "Quartermaster Dorga",
        "alchemist_oda": "Distiller Fizzle", "outfitter_tamsin": "Hidemonger Brakka", "jeweler_lysa": "Stonecutter Hesketh",
    },
}

# Outposts in the wilds: vendors of the zone's band and a soulbinder. (template id, new name) lists.
OUTPOSTS = {
    "saltmarsh_camp": [("provisioner_brannigan", "Fenwife Hulda"), ("outfitter_tamsin", "Reedtanner Moss"), ("binder", "Soulbinder Asha")],
    "duskmere_camp": [("provisioner_brannigan", "Netmender Corrin"), ("smith_garrick", "Smith Oswin Grey"), ("alchemist_oda", "Tidewife Senna"),
                      ("binder", "Soulbinder Tamsk")],
    "steppe_camp": [("provisioner_brannigan", "Sutler Rhoswen"), ("armorer_varra", "Armorer Kael Dunmere"), ("jeweler_lysa", "Trinketeer Ozzo"),
                    ("binder", "Soulbinder Imra")],
    "ashen_camp": [("provisioner_brannigan", "Ashwalker Vey"), ("armsdealer_hesk", "Arms Dealer Morgrin"), ("alchemist_oda", "Cinderbrewer Lotte"),
                   ("binder", "Soulbinder Rhun")],
}

WAYSTONES = {"emberhold": "Stonewarden Oric", "thornwood": "Stonewarden Elnara", "cragmouth": "Stonewarden Bruk"}
SOULBINDERS = {"emberhold": "Soulbinder Hathra", "thornwood": "Soulbinder Ilyra", "cragmouth": "Soulbinder Thane Morrow"}
WAYSTONE_FEE = 50

# Ferries: (npc id, zone, name, x, z, yaw, destinations). Hub ferries stand at their scene's dock marker.
FERRIES = [
    ("emberhold_ferry", "emberhold", "Ferry Master Wilda", 150.0, 140.0, S, [("duskmere_landing", 30, 10.0)]),
    ("thornwood_ferry", "thornwood", "Bargemistress Feyn", 160.0, 50.0, E, [("duskmere_landing", 30, 10.0)]),
    ("duskmere_ferry", "duskmere_shore", "Ferry Master Haldane", 8.0, -320.0, S, [("emberhold_dock", 30, 10.0), ("thornwood_dock", 30, 10.0)]),
]
# Travel points that are not features: the docks (where ferries land). Hub docks match their scene's dock entry.
DOCK_POINTS = [
    ("emberhold_dock", "The Emberhold Docks", "emberhold", 150.0, 150.0, S),
    ("thornwood_dock", "The Thornwood Landing", "thornwood", 150.0, 60.0, W),
    ("duskmere_landing", "The Duskmere Landing", "duskmere_shore", 0.0, -300.0, S),
]


def r1(v: float) -> float:
    return round(v, 1)


def side_spot(half: float, side: str, off: float, inset: float) -> tuple[float, float]:
    if side == "west":
        return -half + inset, off
    if side == "east":
        return half - inset, off
    if side == "north":
        return off, -half + inset
    return off, half - inset


def load_mobs() -> list[dict]:
    return json.loads((CONTENT / "mobs.json").read_text(encoding="utf-8"))["mobs"]


def pick_mob(mobs: list[dict], level: int, biome: str, rng: random.Random) -> str:
    """A wild mob near `level` (named and boss mobs excluded), preferring the biome's families."""
    wild = [m for m in mobs if not m.get("named") and not m.get("boss") and not m.get("dungeon") and m["id"] != "grimjaw"
            and m.get("faction") not in ("guards",)]
    pref = [m for m in wild if biome in m.get("biomes", [])]
    for pool in (pref, wild):
        near = [m for m in pool if abs(m["level"] - level) <= 2]
        if near:
            return rng.choice(sorted(near, key=lambda m: m["id"]))["id"]
    return min(wild, key=lambda m: (abs(m["level"] - level), m["id"]))["id"]


def links_of(zid: str) -> list[tuple[str, str, str]]:
    """(side or hub gate, other zone, other side) for each link touching zid."""
    out = []
    for a, sa, b, sb, _ in LINKS:
        if a == zid:
            out.append((sa, b, sb))
        if b == zid:
            out.append((sb, a, sa))
    return out


def link_offset(zid: str, other: str) -> float:
    for a, _, b, _, off in LINKS:
        if {a, b} == {zid, other}:
            return off
    return 0.0


class ZoneBuild:
    """Props, collision and clear areas for one outdoor zone."""

    def __init__(self, half: float):
        self.half = half
        self.props: list[dict] = []
        self.obstacles: list[dict] = []
        self.walls: list[dict] = []
        self.clear: list[tuple[float, float, float]] = []

    def place(self, prop: str, x: float, z: float, yaw: float, keep_clear: float = 0.0) -> None:
        self.props.append({"prop": prop, "x": r1(x), "z": r1(z), "yaw": round(yaw, 3)})
        c, s = math.cos(yaw), math.sin(yaw)
        for shape in PROPS[prop]["collision"]:
            if shape[0] == "cyl":
                _, rad, lx, lz = shape
                self.obstacles.append({"x": r1(x + lx * c + lz * s), "z": r1(z - lx * s + lz * c), "radius": rad})
            else:
                _, bsx, bsz, lx, lz, byaw = shape
                self.walls.append({"x": r1(x + lx * c + lz * s), "z": r1(z - lx * s + lz * c), "hx": bsx / 2, "hz": bsz / 2,
                                   "yaw": round(yaw + math.radians(byaw), 4)})
        if keep_clear:
            self.clear.append((x, z, keep_clear))

    def is_clear(self, x: float, z: float, roads: list) -> bool:
        if any(math.hypot(x - cx, z - cz) < cr for cx, cz, cr in self.clear):
            return False
        for (ax, az), (bx, bz) in roads:
            dx, dz = bx - ax, bz - az
            ll = dx * dx + dz * dz
            t = 0.0 if ll == 0 else max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / ll))
            if math.hypot(ax + dx * t - x, az + dz * t - z) < 9.0:
                return False
        return True


def outdoor(spec: dict, mobs: list[dict], npcs: list[dict], travel: list[dict]) -> dict:
    rng = random.Random(spec["seed"])
    half = spec["half"]
    biome = BIOMES[spec["biome"]]
    zid = spec["id"]
    z = {"id": zid, "name": spec["name"], "kind": "outdoor", "levels": {"min": spec["levels"][0], "max": spec["levels"][1]},
         "description": spec["description"], "bounds": {"min_x": -half, "min_z": -half, "max_x": half, "max_z": half}}
    n_hills, lo, hi = biome["hills"]
    hills = [[r1(rng.uniform(-half, half)), r1(rng.uniform(-half, half)), r1(rng.uniform(40, 150)), r1(rng.uniform(lo, hi))]
             for _ in range(n_hills)]
    z["terrain"] = {"base": 0.0, "hills": hills}
    b = ZoneBuild(half)
    entries: list[dict] = []
    lines: list[dict] = []
    # Sides that link to neighbours: a line at the edge and an arrival entry 40 m in.
    for side, other, other_side in links_of(zid):
        off = link_offset(zid, other)
        lx, lz = side_spot(half, side, off, 6.0)
        ex, ez = side_spot(half, side, off, 40.0)
        along_x = side in ("north", "south")
        lines.append({"id": f"to_{other}", "x": lx, "z": lz, "hx": 30.0 if along_x else 6.0, "hz": 6.0 if along_x else 30.0,
                      "to": other, "entry": other_side[4:] if other_side.startswith("hub:") else f"from_{zid}"})
        entries.append({"id": f"from_{other}", "x": ex, "z": ez, "yaw": round(FACING[side], 4)})
        b.clear.append((lx, lz, 40.0))
        b.clear.append((ex, ez, 14.0))
    features = [f for f in FEATURES if f[0] == zid]
    for _, kind, fx, fz, arg in features:
        if kind == "dungeon":
            b.place("cave_mouth", fx, fz, 0.0, 22.0)
            lines.append({"id": f"to_{arg}", "x": fx, "z": r1(fz - 1.0), "hx": 3.5, "hz": 1.5, "to": arg, "entry": "entrance"})
            entries.append({"id": f"from_{arg}", "x": fx, "z": r1(fz + 20.0), "yaw": round(S, 4)})
        elif kind in ("ring", "spire"):
            for i in range(6 if kind == "ring" else 4):
                a = 2 * math.pi * i / (6 if kind == "ring" else 4)
                r = 9.0 if kind == "ring" else 6.0
                b.place("ruin_pillar" if kind == "ring" else "crystal", fx + math.cos(a) * r, fz + math.sin(a) * r, a)
            b.clear.append((fx, fz, 16.0))
            travel.append({"id": arg, "name": TRAVEL_NAMES[arg], "zone": zid, "x": fx, "z": fz, "yaw": 0.0})
        elif kind == "outpost":
            for i, (dx, dz) in enumerate([(-12, -8), (12, -8), (-12, 10), (12, 10)]):
                b.place("tent", fx + dx, fz + dz, (math.pi / 2) * (i % 2))
            b.place("brazier", fx, fz, 0.0)
            b.clear.append((fx, fz, 26.0))
            for i, (template, name) in enumerate(OUTPOSTS[arg]):
                a = 2 * math.pi * i / len(OUTPOSTS[arg])
                nx, nz = r1(fx + math.cos(a) * 5.0), r1(fz + math.sin(a) * 5.0)
                if template == "binder":
                    npcs.append({"id": f"{arg.split('_')[0]}_soulbinder", "name": name, "role": "binder", "model": "humanoid", "level": 60, "zone": zid,
                                 "x": nx, "z": nz, "yaw": 0.0, "greeting": "Even out here, a soul can find its anchor.", "travel": [{"dest": "bind"}]})
                else:
                    npcs.append(clone(template, f"{arg.split('_')[0]}_{template.split('_')[0]}", name, zid, nx, nz))
        elif kind == "dock":
            b.place("dock", fx, fz, 0.0)
            b.place("boat", fx + 10.0, fz - 4.0, 0.0)
            b.clear.append((fx, fz, 30.0))
    # Roads from every entry to the zone's middle keep the way clear of props.
    roads = [((e["x"], e["z"]), (0.0, 0.0)) for e in entries]
    margin = 15.0
    area = (2 * half / 100.0) ** 2
    for prop, density in biome["scatter"]:
        count = int(density * area)
        placed = tries = 0
        while placed < count and tries < count * 30:
            tries += 1
            x = rng.uniform(-half + margin, half - margin)
            zz = rng.uniform(-half + margin, half - margin)
            if not b.is_clear(x, zz, roads):
                continue
            b.place(prop, x, zz, rng.uniform(-math.pi, math.pi))
            b.clear.append((x, zz, 4.0))
            placed += 1
    # Camps: level rises with distance from the first (lowest) entry.
    start = (entries[0]["x"], entries[0]["z"]) if entries else (0.0, 0.0)
    far = max(math.hypot(start[0] - cx, start[1] - cz) for cx, cz in [(-half, -half), (half, -half), (-half, half), (half, half)])
    spawns = []
    lvl_lo, lvl_hi = spec["levels"]
    tries = 0
    while len(spawns) < spec["camps"] and tries < 4000:
        tries += 1
        x = rng.uniform(-half + 40, half - 40)
        zz = rng.uniform(-half + 40, half - 40)
        if any(math.hypot(x - cx, zz - cz) < cr + 10 for cx, cz, cr in b.clear if cr > 5) or \
           any(math.hypot(x - s["x"], zz - s["z"]) < 70 for s in spawns):
            continue
        t = min(1.0, math.hypot(x - start[0], zz - start[1]) / far)
        lvl = round(lvl_lo + (lvl_hi - lvl_lo) * t)
        spawns.append({"mob": pick_mob(mobs, lvl, spec["biome"], rng), "x": r1(x), "z": r1(zz), "count": 3 if lvl < 30 else 2, "radius": 14.0})
    z["safe_spawn"] = {"x": entries[0]["x"], "y": 0.0, "z": entries[0]["z"], "yaw": entries[0]["yaw"]} if entries else \
        {"x": 0.0, "y": 0.0, "z": 0.0, "yaw": 0.0}
    z["theme"] = {"biome": spec["biome"], **spec["theme"]}
    z["props"] = b.props
    z["obstacles"] = b.obstacles
    if b.walls:
        z["walls"] = b.walls
    z["entries"] = entries
    z["zone_lines"] = lines
    z["spawns"] = spawns
    return z


def clone(template_id: str, new_id: str, name: str, zone: str, x: float, z: float, yaw: float = 0.0) -> dict:
    t = TEMPLATES[template_id]
    row = {k: v for k, v in t.items() if k not in ("id", "name", "zone", "x", "z", "yaw")}
    return {"id": new_id, "name": name, **row, "zone": zone, "x": x, "z": z, "yaw": yaw, "origin": WORLD_TAG}


TEMPLATES: dict[str, dict] = {}


def hub(spec: dict) -> dict:
    half = spec["half"]
    sx, sz, syaw = spec["spawn"]
    return {"id": spec["id"], "name": spec["name"], "kind": "hub", "levels": {"min": 1, "max": 60},
            "description": spec["description"], "bounds": {"min_x": -half, "min_z": -half, "max_x": half, "max_z": half},
            "ground_y": 0.0, "safe_spawn": {"x": sx, "y": 0.0, "z": sz, "yaw": round(syaw, 4)},
            "scene": f"res://zones/{spec['id']}.tscn", "theme": spec["theme"]}


def hub_npcs(spec: dict) -> list[dict]:
    hid = spec["id"]
    out = []
    for template, name in HUB_ROSTER[hid].items():
        out.append(clone(template, f"{hid}_{template}", name, hid, 0.0, 0.0))  # the scene's marker places it
    out.append({"id": f"{hid}_soulbinder", "name": SOULBINDERS[hid], "role": "binder", "model": "humanoid", "level": 60, "zone": hid,
                "x": 0.0, "z": 0.0, "yaw": 0.0, "greeting": "The city remembers every soul it shelters. Shall it remember yours?",
                "travel": [{"dest": "bind"}]})
    others = [h["id"] for h in HUBS if h["id"] != hid]
    out.append({"id": f"{hid}_stonewarden", "name": WAYSTONES[hid], "role": "teleporter", "model": "humanoid", "level": 60, "zone": hid,
                "x": 0.0, "z": 0.0, "yaw": 0.0, "greeting": "The waystones remember their sisters. For a little silver, so will you.",
                "travel": [{"dest": o, "fee": WAYSTONE_FEE} for o in others]})
    return out


def dungeon(spec: dict, mobs: list[dict]) -> dict:
    did = spec["id"]
    mine = [m for m in mobs if m.get("dungeon") == did]
    trash = sorted([m for m in mine if not m.get("named")], key=lambda m: (m["level"], m["id"]))
    named = sorted([m for m in mine if m.get("named") and not m.get("boss")], key=lambda m: m["id"])
    boss = [m for m in mine if m.get("boss")]
    if not trash or len(boss) != 1:
        sys.exit(f"gen_world: dungeon {did} needs trash and one boss in mobs.json (run tools/gen_mobs.py)")
    raid = spec.get("raid", False)
    spawns = [{"mob": m["id"], "room": "all", "count": 2 if raid else 1, "radius": 6.0} for m in trash]
    # Each named rare haunts one room, in place of its trash type (rooms counted back from the boss's).
    for i, n in enumerate(named):
        spawns.append({"mob": n["rare_of"], "room": max(1, spec["rooms"] - 2 - i), "count": 1, "radius": 4.0, "rare": n["id"], "rare_chance": 0.25})
    spawns.append({"mob": boss[0]["id"], "room": "boss", "count": 1, "radius": 0.0})
    spawns.append({"mob": trash[-1]["id"], "room": "boss", "count": 2, "radius": 8.0})
    return {"id": did, "name": spec["name"], "kind": "dungeon", "levels": {"min": spec["levels"][0], "max": spec["levels"][1]},
            "description": spec["description"], "dungeon": {"seed": spec["seed"], "rooms": spec["rooms"], "cell": 4.0},
            "interest_radius": 60.0, "theme": spec["theme"], **({"raid": True} if raid else {}),
            "entries": [{"id": "entrance", "room": "entrance"}],
            "zone_lines": [{"id": "exit", "room": "entrance", "hx": 3.0, "hz": 1.5, "to": spec["from"], "entry": f"from_{did}"}],
            "spawns": spawns}


def main() -> int:
    check = "--check" in sys.argv
    mobs = load_mobs()
    npc_path = CONTENT / "npcs.json"
    all_npcs = json.loads(npc_path.read_text(encoding="utf-8"))["npcs"]
    # Templates: the test zone's originals (never our own clones).
    for n in all_npcs:
        if n.get("origin") != WORLD_TAG and n.get("role") not in WORLD_ROLES:
            TEMPLATES[n["id"]] = n
    kept = [n for n in all_npcs if n.get("origin") != WORLD_TAG and n.get("role") not in WORLD_ROLES]
    world_npcs: list[dict] = []
    travel: list[dict] = []
    for h in HUBS:
        world_npcs += hub_npcs(h)
    docs = [hub(h) for h in HUBS]
    docs += [outdoor(o, mobs, world_npcs, travel) for o in OUTDOOR]
    docs += [dungeon(d, mobs) for d in DUNGEONS]
    for fid, zone, name, x, z, yaw, dests in FERRIES:
        world_npcs.append({"id": fid, "name": name, "role": "ferry", "model": "humanoid", "level": 60, "zone": zone, "x": x, "z": z,
                           "yaw": round(yaw, 4), "greeting": "All aboard who are boarding! Mind the gap.",
                           "travel": [{"dest": d, "fee": fee, "delay": delay} for d, fee, delay in dests]})
    for pid, name, zone, x, z, yaw in DOCK_POINTS:
        travel.append({"id": pid, "name": name, "zone": zone, "x": x, "z": z, "yaw": round(yaw, 4)})
    travel.sort(key=lambda t: t["id"])
    outputs: list[tuple[Path, str]] = [(ZONES / f"{d['id']}.json", format_zone(d)) for d in docs]
    outputs.append((npc_path, render_npcs(kept + world_npcs)))
    travel_doc = {"_comment": "Named places travel can reach (generated by tools/gen_world.py): Druid rings, Wizard spires and ferry docks. "
                              "Waystones, ferries and portal spells name these or a city.", "points": travel}
    lines = ["{", '  "_comment": ' + json.dumps(travel_doc["_comment"]) + ",", '  "points": [']
    lines += ["    " + json.dumps(t) + ("," if i < len(travel) - 1 else "") for i, t in enumerate(travel)]
    lines += ["  ]", "}", ""]
    outputs.append((CONTENT / "travel.json", "\n".join(lines)))
    stale = []
    for path, text in outputs:
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
    if check and stale:
        print("stale world files (run python3 tools/gen_world.py):", ", ".join(stale))
        return 1
    if not check:
        print(f"wrote {len(docs)} zone files, {len(world_npcs)} world NPCs, {len(travel)} travel points")
    return 0


if __name__ == "__main__":
    sys.exit(main())
