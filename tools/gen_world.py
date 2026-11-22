#!/usr/bin/env python3
"""World generator: writes the outdoor and dungeon zone files (content/zones/<id>.json) from the
specs below. Hubs are hand-authored Godot scenes (client/zones/<id>.tscn, exported by
client/tools/export_zones.gd); their zone files here carry only metadata.

Outdoor zones get round hills, scattered props (trees, rocks, ruins) from tools/world_props.py
(collision from the same table the prop scenes use), camps of mobs, entries and zone lines.
Dungeons get their seeded layout parameters (the server and client build the rooms) and room spawns.
Everything is deterministic: the same specs give the same files.

  python3 tools/gen_world.py          # write
  python3 tools/gen_world.py --check  # exit 1 if a committed zone file is stale
"""
from __future__ import annotations

import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from world_props import PROPS  # noqa: E402
from zone_format import format_zone  # noqa: E402
from npc_order import render as render_npcs  # noqa: E402

import json  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ZONES = ROOT / "content" / "zones"

E, W, N, S = -math.pi / 2, math.pi / 2, 0.0, math.pi  # yaw facing east (+x), west, north (-z), south (+z)

# ------------------------------------------------------------------------------------------- hubs

HUBS = [
    {"id": "emberhold", "name": "Emberhold", "levels": (1, 60), "half": 200.0, "spawn": (0.0, 15.0, S),
     "description": "A walled river city built around the Great Forge, whose fire has not gone out in four hundred years.",
     "theme": {"biome": "town", "ground": "#6b6350", "sky": "#4a6491", "fog": "#b39b85", "road": "#8a7a62"}},
]

# ------------------------------------------------------------------------------------------- outdoor zones

OUTDOOR = [
    {"id": "emberglade", "name": "The Emberglade", "levels": (1, 8), "half": 400.0, "seed": 101,
     "description": "Rolling meadows east of Emberhold, where ember rats nest in the warm earth and brigands watch the road.",
     "theme": {"biome": "meadow", "ground": "#5d7a3e", "sky": "#5a7fb5", "fog": "#c4c9b0"},
     "hills": 22, "hill_height": (2.0, 9.0), "scatter": [("tree_oak", 160), ("rock", 70), ("boulder", 14), ("reeds", 40)],
     "entries": [("emberhold_road", -370.0, 0.0, E), ("warren_mouth", 250.0, -180.0, S)],
     "lines": [("to_emberhold", -392.0, 0.0, 6.0, 40.0, "emberhold", "east_gate")],
     "features": [("cave_mouth", 250.0, -200.0, 0.0, ("to_rattlebone", 0.0, -1.0, 3.5, 1.5, "rattlebone_warren", "entrance"))],
     "roads": [((-370.0, 0.0), (250.0, -180.0))],
     "camps": [("ember_rat", -250, 40, 5), ("ember_rat", -200, -60, 5), ("dusk_beetle", -150, 120, 4), ("mangy_cur", -60, -120, 4),
               ("reed_adder", 40, 150, 4), ("road_brigand", 60, -40, 4), ("dusk_beetle", 150, 80, 4), ("grave_rattler", 220, -120, 3),
               ("thornback_boar", 300, 140, 3), ("road_brigand", 180, -260, 3), ("mangy_cur", -300, 220, 4), ("reed_adder", -120, 300, 4)]},
]

# ------------------------------------------------------------------------------------------- dungeons

DUNGEONS = [
    {"id": "rattlebone_warren", "name": "Rattlebone Warren", "levels": (2, 10), "seed": 4242, "rooms": 9,
     "description": "Burrows and bone-pits under the Emberglade, where the restless dead of an old battlefield gather.",
     "theme": {"biome": "crypt", "ground": "#4a4038", "wall": "#5a5048", "fog": "#2a2420", "light": "#ffb070"},
     "exit": ("emberglade", "warren_mouth"),
     "spawns": [("ember_rat", "all", 2), ("grave_rattler", "all", 1), ("grave_rattler", "boss", 3)]},
]


# ------------------------------------------------------------------------------------------- world NPCs
# Roles this generator owns in content/npcs.json. Hub scenes place them (npc markers); x/z here is the fallback.
WORLD_ROLES = ("binder", "teleporter", "ferry")
WORLD_NPCS = [
    {"id": "emberhold_soulbinder", "name": "Soulbinder Hathra", "role": "binder", "model": "humanoid", "level": 60, "zone": "emberhold",
     "x": 38.0, "z": -36.0, "yaw": 3.14, "greeting": "The Flame remembers every soul it warms. Shall it remember yours?",
     "travel": [{"dest": "bind"}]},
    {"id": "emberhold_stonewarden", "name": "Stonewarden Oric", "role": "teleporter", "model": "humanoid", "level": 60, "zone": "emberhold",
     "x": 4.0, "z": 45.0, "yaw": 3.14, "greeting": "The waystones remember their sisters. For a little silver, so will you.",
     "travel": [{"dest": "thornwood", "fee": 50}, {"dest": "cragmouth", "fee": 50}]},
]


def r1(v: float) -> float:
    return round(v, 1)


def outdoor(spec: dict) -> dict:
    rng = random.Random(spec["seed"])
    half = spec["half"]
    z = {"id": spec["id"], "name": spec["name"], "kind": "outdoor", "levels": {"min": spec["levels"][0], "max": spec["levels"][1]},
         "description": spec["description"], "bounds": {"min_x": -half, "min_z": -half, "max_x": half, "max_z": half}}
    hills = []
    lo, hi = spec["hill_height"]
    for _ in range(spec["hills"]):
        hills.append([r1(rng.uniform(-half, half)), r1(rng.uniform(-half, half)), r1(rng.uniform(40, 140)), r1(rng.uniform(lo, hi))])
    z["terrain"] = {"base": 0.0, "hills": hills}
    ent = spec["entries"]
    sx, sz, syaw = ent[0][1], ent[0][2], ent[0][3]
    z["safe_spawn"] = {"x": sx, "y": 0.0, "z": sz, "yaw": syaw}
    z["theme"] = spec["theme"]
    # Places that stay clear of scatter: entries, lines, features, camps, and roads between entries.
    clear: list[tuple[float, float, float]] = [(e[1], e[2], 12.0) for e in ent]
    clear += [(l[1], l[2], max(l[3], l[4]) + 8.0) for l in spec["lines"]]
    clear += [(f[1], f[2], 20.0) for f in spec["features"]]
    clear += [(c[1], c[2], 18.0) for c in spec["camps"]]
    roads = spec.get("roads", [])

    def near_road(x: float, zz: float) -> bool:
        for (ax, az), (bx, bz) in roads:
            dx, dz = bx - ax, bz - az
            t = max(0.0, min(1.0, ((x - ax) * dx + (zz - az) * dz) / (dx * dx + dz * dz)))
            if math.hypot(ax + dx * t - x, az + dz * t - zz) < 9.0:
                return True
        return False

    props: list[dict] = []
    obstacles: list[dict] = []
    walls: list[dict] = []

    def place(prop: str, x: float, zz: float, yaw: float) -> None:
        props.append({"prop": prop, "x": r1(x), "z": r1(zz), "yaw": round(yaw, 3)})
        c, s = math.cos(yaw), math.sin(yaw)
        for shape in PROPS[prop]["collision"]:
            if shape[0] == "cyl":
                _, rad, lx, lz = shape
                obstacles.append({"x": r1(x + lx * c + lz * s), "z": r1(zz - lx * s + lz * c), "radius": rad})
            else:
                _, bsx, bsz, lx, lz, byaw = shape
                walls.append({"x": r1(x + lx * c + lz * s), "z": r1(zz - lx * s + lz * c), "hx": bsx / 2, "hz": bsz / 2,
                              "yaw": round(yaw + math.radians(byaw), 4)})

    lines = []
    entries = [{"id": e[0], "x": e[1], "z": e[2], "yaw": round(e[3], 4)} for e in ent]
    for l in spec["lines"]:
        lines.append({"id": l[0], "x": l[1], "z": l[2], "hx": l[3], "hz": l[4], "to": l[5], "entry": l[6]})
    for f in spec["features"]:
        prop, fx, fz, fyaw, line = f
        place(prop, fx, fz, fyaw)
        if line:
            lid, lx, lz, hx, hz, to, entry = line
            lines.append({"id": lid, "x": r1(fx + lx), "z": r1(fz + lz), "hx": hx, "hz": hz, "to": to, "entry": entry})
    margin = 15.0
    for prop, count in spec["scatter"]:
        placed = 0
        tries = 0
        while placed < count and tries < count * 30:
            tries += 1
            x = rng.uniform(-half + margin, half - margin)
            zz = rng.uniform(-half + margin, half - margin)
            if any(math.hypot(x - cx, zz - cz) < cr for cx, cz, cr in clear) or near_road(x, zz):
                continue
            place(prop, x, zz, rng.uniform(-math.pi, math.pi))
            clear.append((x, zz, 4.0))
            placed += 1
    z["props"] = props
    z["obstacles"] = obstacles
    if walls:
        z["walls"] = walls
    z["entries"] = entries
    z["zone_lines"] = lines
    z["spawns"] = [{"mob": c[0], "x": float(c[1]), "z": float(c[2]), "count": c[3], "radius": 14.0} for c in spec["camps"]]
    return z


def hub(spec: dict) -> dict:
    half = spec["half"]
    sx, sz, syaw = spec["spawn"]
    return {"id": spec["id"], "name": spec["name"], "kind": "hub", "levels": {"min": spec["levels"][0], "max": spec["levels"][1]},
            "description": spec["description"], "bounds": {"min_x": -half, "min_z": -half, "max_x": half, "max_z": half},
            "ground_y": 0.0, "safe_spawn": {"x": sx, "y": 0.0, "z": sz, "yaw": round(syaw, 4)},
            "scene": f"res://zones/{spec['id']}.tscn", "theme": spec["theme"]}


def dungeon(spec: dict) -> dict:
    to, entry = spec["exit"]
    return {"id": spec["id"], "name": spec["name"], "kind": "dungeon", "levels": {"min": spec["levels"][0], "max": spec["levels"][1]},
            "description": spec["description"], "dungeon": {"seed": spec["seed"], "rooms": spec["rooms"], "cell": 4.0},
            "interest_radius": 60.0, "theme": spec["theme"],
            "entries": [{"id": "entrance", "room": "entrance"}],
            "zone_lines": [{"id": "exit", "room": "entrance", "hx": 3.0, "hz": 1.5, "to": to, "entry": entry}],
            "spawns": [{"mob": m, "room": room, "count": n, "radius": 6.0} for m, room, n in spec["spawns"]]}


def main() -> int:
    check = "--check" in sys.argv
    docs = [hub(h) for h in HUBS] + [outdoor(o) for o in OUTDOOR] + [dungeon(d) for d in DUNGEONS]
    stale = []
    for d in docs:
        path = ZONES / f"{d['id']}.json"
        text = format_zone(d)
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
    npc_path = ROOT / "content" / "npcs.json"
    kept = [n for n in json.loads(npc_path.read_text(encoding="utf-8"))["npcs"] if n.get("role") not in WORLD_ROLES]
    npc_text = render_npcs(kept + WORLD_NPCS)
    if check:
        if npc_path.read_text(encoding="utf-8") != npc_text:
            stale.append("npcs.json")
    else:
        npc_path.write_text(npc_text, encoding="utf-8", newline="\n")
    if check and stale:
        print("stale zone files (run python3 tools/gen_world.py):", ", ".join(stale))
        return 1
    if not check:
        print(f"wrote {len(docs)} zone files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
