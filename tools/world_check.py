"""World checks for tools/validate_content.py (Phase 6): zone files, scene exports, zone lines and
entries, travel, and reachability. Mirrors server/src/game/content_world.cpp.

Zones live one per file in content/zones/*.json; a hand-authored hub's collision and markers come
from content/zones/scenes/<id>.json (exported from client/zones/<id>.tscn) and are merged after the
zone's own lists, as the server does. Dungeon layouts are generated from their seed; the validator
needs only their size (rooms are placed server- and client-side).
"""
from __future__ import annotations

import json
import math
from collections import deque
from pathlib import Path

ZONE_KINDS = {"hub", "outdoor", "dungeon", "test"}
MERGED = ("obstacles", "walls", "zone_lines", "entries", "spawns")


def dungeon_bounds(d: dict) -> tuple[float, float, float, float]:
    """Same formula as dungeon_gen.cpp: a (g+1) x (g+1) grid of sectors, g*g >= rooms."""
    rooms = int(d.get("rooms", 8))
    sector = int(d.get("sector", 12))
    cell = float(d.get("cell", 4.0))
    g = 1
    while g * g < rooms:
        g += 1
    g += 1
    half = g * sector * cell / 2
    return -half, -half, half, half


def load_zones(content_dir: Path, c) -> list[dict]:
    """Every zone file, scene exports merged, with the server's defaults filled in."""
    zdir = content_dir / "zones"
    if not zdir.is_dir():
        c.err(f"missing directory {zdir}")
        return []
    scenes: dict[str, dict] = {}
    sdir = zdir / "scenes"
    if sdir.is_dir():
        for p in sorted(sdir.glob("*.json")):
            try:
                sj = json.loads(p.read_text(encoding="utf-8"))
            except json.JSONDecodeError as e:
                c.err(f"{p}: invalid JSON: {e}")
                continue
            zid = sj.get("zone")
            if not isinstance(zid, str):
                c.err(f"{p}: missing 'zone'")
                continue
            if zid in scenes:
                c.err(f"zones/scenes: two scene exports for zone '{zid}'")
            scenes[zid] = sj
    zones = []
    for p in sorted(zdir.glob("*.json")):
        try:
            z = json.loads(p.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            c.err(f"{p}: invalid JSON: {e}")
            continue
        if not isinstance(z, dict):
            c.err(f"{p}: must be a JSON object")
            continue
        zid = z.get("id")
        sj = scenes.pop(zid, None)
        if sj:
            for key in MERGED:
                if isinstance(sj.get(key), list):
                    z[key] = list(z.get(key, [])) + sj[key]
            z["_npc_placements"] = sj.get("npcs", [])
        elif z.get("scene"):
            c.err(f"zone '{zid}': scene '{z['scene']}' has no export in zones/scenes (run the zone export tool)")
        z.setdefault("interest_radius", 80.0)
        z.setdefault("say_radius", 60.0)
        z.setdefault("ground_y", float(z.get("terrain", {}).get("base", 0.0)) if isinstance(z.get("terrain"), dict) else 0.0)
        if isinstance(z.get("dungeon"), dict):
            mnx, mnz, mxx, mxz = dungeon_bounds(z["dungeon"])
            z["bounds"] = {"min_x": mnx, "min_z": mnz, "max_x": mxx, "max_z": mxz}
            z.setdefault("safe_spawn", {"x": 0.0, "y": 0.0, "z": 0.0, "yaw": 0.0, "_dungeon": True})
        zones.append(z)
    for zid in scenes:
        c.err(f"zones/scenes: export for unknown zone '{zid}'")
    return zones


def _inside(b: dict, x: float, z: float) -> bool:
    return b.get("min_x", 0) <= x <= b.get("max_x", 0) and b.get("min_z", 0) <= z <= b.get("max_z", 0)


def _in_line(line: dict, x: float, z: float) -> bool:
    if "x" not in line or "z" not in line:
        return False
    yaw = float(line.get("yaw", 0.0))
    dx, dz = x - float(line["x"]), z - float(line["z"])
    lx = dx * math.cos(yaw) - dz * math.sin(yaw)
    lz = dx * math.sin(yaw) + dz * math.cos(yaw)
    return abs(lx) <= float(line.get("hx", 4.0)) and abs(lz) <= float(line.get("hz", 4.0))


def _room_ok(room, rooms: int) -> bool:
    if room in ("entrance", "boss", "all"):
        return True
    try:
        return 0 <= int(room) < rooms
    except (TypeError, ValueError):
        return False


def validate_world(c, zones: list[dict], cities: list[dict], npcs: list[dict], travel: list[dict], max_level: int) -> None:
    by_id = {z.get("id"): z for z in zones}
    for z in zones:
        zid = z.get("id", "?")
        w = f"zone '{zid}'"
        if z.get("kind", "outdoor") not in ZONE_KINDS:
            c.err(f"{w}: kind must be hub, outdoor, dungeon or test")
        lv = z.get("levels", {"min": 1, "max": max_level})
        if not (isinstance(lv, dict) and 1 <= lv.get("min", 1) <= lv.get("max", max_level) <= max_level):
            c.err(f"{w}: levels must satisfy 1 <= min <= max <= max_level")
        b = z.get("bounds") or {}
        if b and (b.get("max_x", 0) - b.get("min_x", 0) > 4000 or b.get("max_z", 0) - b.get("min_z", 0) > 4000):
            c.err(f"{w}: a zone may be at most 4 km across")
        dungeon = z.get("dungeon") if isinstance(z.get("dungeon"), dict) else None
        rooms = int(dungeon.get("rooms", 8)) if dungeon else 0
        if dungeon and not 2 <= rooms <= 64:
            c.err(f"{w}: dungeon rooms must be 2..64")
        for sp in z.get("spawns", []):
            if "room" in sp:
                if not dungeon:
                    c.err(f"{w}: spawn for '{sp.get('mob')}' names a room outside a dungeon")
                elif not _room_ok(sp["room"], rooms):
                    c.err(f"{w}: spawn for '{sp.get('mob')}' names room '{sp['room']}', which the layout does not have")
        entry_ids = set()
        lines = z.get("zone_lines", [])
        for e in z.get("entries", []):
            eid = e.get("id")
            if eid in entry_ids:
                c.err(f"{w}: entry '{eid}' is defined twice")
            entry_ids.add(eid)
            if "room" in e:
                if not dungeon or not _room_ok(e["room"], rooms):
                    c.err(f"{w}: entry '{eid}' names an unknown room")
                continue
            if b and not _inside(b, float(e.get("x", 0)), float(e.get("z", 0))):
                c.err(f"{w}: entry '{eid}' is outside bounds")
            for l in lines:
                if _in_line(l, float(e.get("x", 0)), float(e.get("z", 0))):
                    c.err(f"{w}: entry '{eid}' lies inside zone line '{l.get('id')}' (players would bounce)")
        line_ids = set()
        for l in lines:
            lid = l.get("id")
            if lid in line_ids:
                c.err(f"{w}: zone line '{lid}' is defined twice")
            line_ids.add(lid)
            to = by_id.get(l.get("to"))
            if to is None:
                c.err(f"{w}: zone line '{lid}' leads to unknown zone '{l.get('to')}'")
            elif l.get("entry") not in {e.get("id") for e in to.get("entries", [])}:
                c.err(f"{w}: zone line '{lid}' arrives at unknown entry '{l.get('entry')}' in '{l.get('to')}'")
            if "room" not in l and b and not _inside(b, float(l.get("x", 0)), float(l.get("z", 0))):
                c.err(f"{w}: zone line '{lid}' is outside bounds")
    tp_ids = set()
    for t in travel:
        tid = t.get("id")
        if tid in tp_ids:
            c.err(f"travel point '{tid}': defined twice")
        tp_ids.add(tid)
        z = by_id.get(t.get("zone"))
        if z is None:
            c.err(f"travel point '{tid}': unknown zone '{t.get('zone')}'")
        elif z.get("bounds") and not _inside(z["bounds"], float(t.get("x", 0)), float(t.get("z", 0))):
            c.err(f"travel point '{tid}': outside its zone")
    city_ids = {ci.get("id") for ci in cities}
    placed = {p.get("id") for z in zones for p in z.get("_npc_placements", [])}
    npc_ids = {n.get("id") for n in npcs}
    for pid in placed - npc_ids:
        c.err(f"a scene places unknown npc '{pid}'")
    for n in npcs:
        role = n.get("role")
        w = f"npc '{n.get('id')}'"
        if role not in ("teleporter", "ferry", "binder"):
            if n.get("travel"):
                c.err(f"{w}: only teleporters, ferries and binders offer travel")
            continue
        opts = n.get("travel", [])
        if role == "binder" and (len(opts) != 1 or opts[0].get("dest") != "bind"):
            c.err(f"{w}: a binder offers exactly one option, dest 'bind'")
        if role != "binder" and not 1 <= len(opts) <= 16:
            c.err(f"{w}: a teleporter or ferry needs 1..16 destinations")
        for o in opts:
            d = o.get("dest")
            if d == "bind":
                continue
            if d not in city_ids and d not in tp_ids:
                c.err(f"{w}: travels to unknown destination '{d}'")
    # Reachability by zone lines, from (and back to) the starting cities' zones.
    starts = {ci.get("zone") for ci in cities if by_id.get(ci.get("zone"), {}).get("kind") != "test"}
    if not starts:
        return
    fwd: dict[str, set] = {}
    back: dict[str, set] = {}
    for z in zones:
        for l in z.get("zone_lines", []):
            fwd.setdefault(z.get("id"), set()).add(l.get("to"))
            back.setdefault(l.get("to"), set()).add(z.get("id"))

    def reach(g: dict[str, set]) -> set:
        seen = set(starts)
        q = deque(starts)
        while q:
            at = q.popleft()
            for n2 in g.get(at, ()):
                if n2 not in seen:
                    seen.add(n2)
                    q.append(n2)
        return seen

    from_city, to_city = reach(fwd), reach(back)
    for z in zones:
        if z.get("kind") == "test":
            continue
        if z.get("id") not in from_city:
            c.err(f"zone '{z.get('id')}' cannot be reached from any starting city by zone lines")
        if z.get("id") not in to_city:
            c.err(f"zone '{z.get('id')}' has no way back to a starting city by zone lines")
