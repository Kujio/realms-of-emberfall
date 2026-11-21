#!/usr/bin/env python3
"""Validate /content JSON: structure, id format, uniqueness, and cross-references.

Mirrors the server's startup validation (server/src/game/content.cpp) so content errors are
caught in CI before a server ever boots. Exit code 0 = valid.

Usage: python tools/validate_content.py [content_dir]
"""
from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ID_RE = re.compile(r"^[a-z0-9_]{1,32}$")


class Checker:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def obj(self, v, where: str) -> dict:
        if not isinstance(v, dict):
            self.err(f"{where}: must be an object")
            return {}
        return v

    def string(self, d: dict, key: str, where: str, required: bool = True) -> str:
        if key not in d:
            if required:
                self.err(f"{where}: missing '{key}'")
            return ""
        if not isinstance(d[key], str):
            self.err(f"{where}: '{key}' must be a string")
            return ""
        return d[key]

    def number(self, d: dict, key: str, where: str) -> float:
        v = d.get(key)
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v):
            self.err(f"{where}: '{key}' must be a finite number")
            return 0.0
        return float(v)

    def integer(self, d: dict, key: str, where: str) -> int:
        v = d.get(key)
        if isinstance(v, bool) or not isinstance(v, int):
            self.err(f"{where}: '{key}' must be an integer")
            return 0
        return v

    def str_list(self, d: dict, key: str, where: str) -> list[str]:
        v = d.get(key)
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
            self.err(f"{where}: '{key}' must be an array of strings")
            return []
        return v

    def spawn(self, d: dict, key: str, where: str) -> tuple[float, float]:
        s = self.obj(d.get(key), f"{where}.{key}")
        for k in ("x", "y", "z", "yaw"):
            self.number(s, k, f"{where}.{key}")
        return (float(s.get("x", 0) or 0), float(s.get("z", 0) or 0))

    def list_of(self, doc, key: str, file: str) -> list[dict]:
        if not isinstance(doc, dict) or not isinstance(doc.get(key), list):
            self.err(f"{file}: top-level '{key}' array is required")
            return []
        return [self.obj(e, f"{key}[{i}]") for i, e in enumerate(doc[key])]

    def ids(self, items: list[dict], kind: str) -> set[str]:
        seen: set[str] = set()
        for i, it in enumerate(items):
            iid = self.string(it, "id", f"{kind}[{i}]")
            name = self.string(it, "name", f"{kind}[{i}]")
            if iid and not ID_RE.match(iid):
                self.err(f"{kind} id '{iid}' must be 1-32 chars of [a-z0-9_]")
            if iid in seen:
                self.err(f"duplicate {kind} id '{iid}'")
            if not 1 <= len(name) <= 48:
                self.err(f"{kind} '{iid}': name must be 1-48 chars")
            seen.add(iid)
        return seen


def load(path: Path, c: Checker):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        c.err(f"missing file {path}")
    except json.JSONDecodeError as e:
        c.err(f"{path}: invalid JSON: {e}")
    return {}


def validate(content_dir: Path) -> list[str]:
    c = Checker()
    rules = c.obj(load(content_dir / "rules.json", c), "rules")
    races = c.list_of(load(content_dir / "races.json", c), "races", "races.json")
    classes = c.list_of(load(content_dir / "classes.json", c), "classes", "classes.json")
    cities = c.list_of(load(content_dir / "cities.json", c), "cities", "cities.json")
    zones = c.list_of(load(content_dir / "zones.json", c), "zones", "zones.json")

    if rules:
        if not 1 <= c.integer(rules, "max_level", "rules") <= 255:
            c.err("rules.max_level must be 1..255")
        if not 1 <= c.integer(rules, "max_characters_per_account", "rules") <= 16:
            c.err("rules.max_characters_per_account must be 1..16")
        mv = c.obj(rules.get("movement"), "rules.movement")
        if mv:
            if not 0 < c.number(mv, "run_speed", "rules.movement") <= 50:
                c.err("rules.movement.run_speed must be in (0, 50]")
            c.integer(mv, "input_budget_max", "rules.movement")
            if c.number(mv, "desync_tolerance", "rules.movement") <= 0:
                c.err("rules.movement.desync_tolerance must be > 0")
            if c.number(mv, "teleport_tolerance", "rules.movement") < 1:
                c.err("rules.movement.teleport_tolerance must be >= 1")

    class_ids = c.ids(classes, "class")
    city_ids = c.ids(cities, "city")
    zone_ids = c.ids(zones, "zone")
    c.ids(races, "race")
    for k in ("races", "classes", "cities", "zones"):
        if not locals()[k]:
            c.err(f"at least one entry in {k} is required")

    for z in classes:
        c.string(z, "archetype", f"class '{z.get('id')}'")

    zone_bounds: dict[str, tuple[float, float, float, float]] = {}
    for z in zones:
        zid = z.get("id", "?")
        b = c.obj(z.get("bounds"), f"zone '{zid}'.bounds")
        mnx, mnz, mxx, mxz = (c.number(b, k, f"zone '{zid}'.bounds") for k in ("min_x", "min_z", "max_x", "max_z"))
        if not (mnx < mxx and mnz < mxz):
            c.err(f"zone '{zid}': bounds min must be < max")
        zone_bounds[zid] = (mnx, mnz, mxx, mxz)
        c.number(z, "ground_y", f"zone '{zid}'")
        if not 10 <= c.number(z, "interest_radius", f"zone '{zid}'") <= 500:
            c.err(f"zone '{zid}': interest_radius must be 10..500")
        if c.number(z, "say_radius", f"zone '{zid}'") <= 0:
            c.err(f"zone '{zid}': say_radius must be > 0")
        sx, sz = c.spawn(z, "safe_spawn", f"zone '{zid}'")
        if not (mnx <= sx <= mxx and mnz <= sz <= mxz):
            c.err(f"zone '{zid}': safe_spawn is outside bounds")

    for city in cities:
        cid = city.get("id", "?")
        zid = c.string(city, "zone", f"city '{cid}'")
        sx, sz = c.spawn(city, "spawn", f"city '{cid}'")
        if zid not in zone_ids:
            c.err(f"city '{cid}': unknown zone '{zid}'")
        elif zid in zone_bounds:
            mnx, mnz, mxx, mxz = zone_bounds[zid]
            if not (mnx <= sx <= mxx and mnz <= sz <= mxz):
                c.err(f"city '{cid}': spawn is outside zone bounds")

    for r in races:
        rid = r.get("id", "?")
        allowed = c.str_list(r, "allowed_classes", f"race '{rid}'")
        rc = c.str_list(r, "cities", f"race '{rid}'")
        if not allowed:
            c.err(f"race '{rid}': allowed_classes must not be empty")
        if not rc:
            c.err(f"race '{rid}': cities must not be empty")
        if len(allowed) > 32 or len(rc) > 8:
            c.err(f"race '{rid}': too many classes (max 32) or cities (max 8)")
        for cl in allowed:
            if cl not in class_ids:
                c.err(f"race '{rid}': unknown class '{cl}'")
        for ci in rc:
            if ci not in city_ids:
                c.err(f"race '{rid}': unknown city '{ci}'")
    return c.errors


def main() -> int:
    content_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "content"
    errors = validate(content_dir)
    if errors:
        print(f"content validation failed ({len(errors)} problem{'s' if len(errors) != 1 else ''}):", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"content OK ({content_dir})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
