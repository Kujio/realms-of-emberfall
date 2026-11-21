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
    validate_combat(c, content_dir, rules, races, classes, zones)
    return c.errors


STAT_KEYS = ("str", "sta", "agi", "dex", "wis", "int", "cha")
ABILITY_TYPES = {"nuke", "heal", "dot", "hot", "buff", "debuff", "cc", "pet", "utility", "passive", "discipline"}
EFFECT_TYPES = {"damage", "stun", "threat", "taunt", "buff", "heal_pct", "restore_endurance_pct", "area_threat"}
BUFF_STATS = {"damage_taken_pct", "melee_damage_pct", "crit_chance_pct"}
RULE_SECTIONS = {
    "xp": ("base", "level_exponent", "growth", "kill_base", "group_bonus_per_member"),
    "con": ("blue_divisor", "green_divisor"),
    "combat": ("gcd_seconds", "melee_range", "loot_range", "event_radius", "in_combat_seconds", "base_hit_chance",
               "min_hit_chance", "max_hit_chance", "crit_multiplier", "max_mitigation", "max_avoid_chance"),
    "death": ("xp_loss_fraction", "min_level_for_xp_loss", "player_corpse_seconds", "mob_corpse_seconds"),
    "mob_scaling": ("hp_base", "hp_per_level", "hp_quad", "max_hit_base", "max_hit_per_level", "max_hit_quad",
                    "ac_per_level", "skill_per_level", "chase_seconds", "flee_seconds", "aggro_scan_ticks"),
}


def validate_combat(c: Checker, content_dir: Path, rules: dict, races: list[dict], classes: list[dict], zones: list[dict]) -> None:
    """Phase 2 data: stats, skills, abilities, items, loot tables, mobs, and spawns."""
    max_level = rules.get("max_level", 60) if isinstance(rules.get("max_level"), int) else 60
    for sec, keys in RULE_SECTIONS.items():
        d = c.obj(rules.get(sec), f"rules.{sec}")
        for k in keys:
            c.number(d, k, f"rules.{sec}")
    xp = rules.get("xp") if isinstance(rules.get("xp"), dict) else {}
    cm = c.obj(xp.get("con_multipliers"), "rules.xp.con_multipliers")
    for k in ("grey", "green", "blue", "white", "yellow", "red"):
        if not 0 <= c.number(cm, k, "rules.xp.con_multipliers") <= 10:
            c.err(f"rules.xp.con_multipliers.{k} must be 0..10")
    if isinstance(xp.get("growth"), (int, float)) and not 1.0 <= xp["growth"] <= 1.5:
        c.err("rules.xp.growth must be in [1, 1.5]")
    try:
        total = sum(float(xp["base"]) * l ** float(xp["level_exponent"]) * float(xp["growth"]) ** (l - 1)
                    for l in range(1, min(int(max_level), 255)))
        if not math.isfinite(total) or total >= 1e15:
            c.err("rules.xp: total XP to max_level must stay below 1e15")
    except (KeyError, TypeError, ValueError, OverflowError):
        pass  # the field checks above already reported the problem

    for r in races:
        st = c.obj(r.get("stats"), f"race '{r.get('id')}'.stats")
        for k in STAT_KEYS:
            if not 1 <= c.integer(st, k, f"race '{r.get('id')}'.stats") <= 255:
                c.err(f"race '{r.get('id')}': stat {k} must be 1..255")

    skills_doc = load(content_dir / "skills.json", c)
    skills = c.list_of(skills_doc, "skills", "skills.json")
    skill_ids = c.ids(skills, "skill")
    defaults = c.obj(skills_doc.get("defaults") if isinstance(skills_doc, dict) else {}, "skills.defaults")

    def cap_ok(cap, where: str) -> int:
        cap = c.obj(cap, where)
        lvl = c.integer(cap, "level", where)
        c.number(cap, "per_level", where)
        mx = c.integer(cap, "max", where)
        if not 1 <= lvl <= max_level or not 1 <= mx <= 1000:
            c.err(f"{where}: invalid skill cap")
        return lvl

    for sid, cap in defaults.items():
        if sid not in skill_ids:
            c.err(f"skills.defaults: unknown skill '{sid}'")
        cap_ok(cap, f"skills.defaults.{sid}")

    class_skills: dict[str, dict[str, int]] = {}
    for cl in classes:
        cid = cl.get("id", "?")
        cb = c.obj(cl.get("combat"), f"class '{cid}'.combat")
        for k in ("hp_base", "hp_per_level", "mana_base", "mana_per_level", "endurance_base", "endurance_per_level",
                  "ac_per_level", "weapon_damage_base", "weapon_damage_per_level", "weapon_delay", "crit_chance"):
            c.number(cb, k, f"class '{cid}'.combat")
        if isinstance(cb.get("weapon_delay"), (int, float)) and not 0.5 <= cb["weapon_delay"] <= 10:
            c.err(f"class '{cid}': weapon_delay must be 0.5..10 s")
        levels = {sid: cap_ok(cap, f"skills.defaults.{sid}") for sid, cap in defaults.items()}
        for sid, cap in c.obj(cl.get("skills", {}), f"class '{cid}'.skills").items():
            if sid not in skill_ids:
                c.err(f"class '{cid}': unknown skill '{sid}'")
            levels[sid] = cap_ok(cap, f"class '{cid}'.skills.{sid}")
        for need in ("offense", "defense"):
            if need not in levels:
                c.err(f"class '{cid}': needs offense and defense skills")
        class_skills[cid] = levels

    abilities = c.list_of(load(content_dir / "abilities.json", c), "abilities", "abilities.json")
    c.ids(abilities, "ability")
    for a in abilities:
        aid = a.get("id", "?")
        w = f"ability '{aid}'"
        cls = c.string(a, "class", w)
        lvl = c.integer(a, "level", w)
        typ = c.string(a, "type", w)
        if cls not in class_skills:
            c.err(f"{w}: unknown class '{cls}'")
        if not 1 <= lvl <= max_level:
            c.err(f"{w}: level must be 1..max_level")
        if typ not in ABILITY_TYPES:
            c.err(f"{w}: unknown type '{typ}'")
        skill = a.get("skill")
        if skill is not None:
            if skill not in skill_ids:
                c.err(f"{w}: unknown skill '{skill}'")
            elif cls in class_skills and skill not in class_skills[cls]:
                c.err(f"{w}: class '{cls}' lacks skill '{skill}'")
            elif typ == "passive" and cls in class_skills and class_skills[cls].get(skill) != lvl:
                c.err(f"{w}: passive level must match the class skill unlock level")
        if typ == "passive":
            if skill is None:
                c.err(f"{w}: passive abilities must name a skill")
            continue
        if c.string(a, "resource", w) not in ("none", "endurance", "mana"):
            c.err(f"{w}: resource must be none, endurance, or mana")
        for k in ("cast_time", "cooldown", "range"):
            c.number(a, k, w)
        c.integer(a, "cost", w)
        if a.get("cast_time") not in (0, 0.0):
            c.err(f"{w}: cast times arrive with the spell engine (Phase 3); use 0")
        if not isinstance(a.get("gcd"), bool):
            c.err(f"{w}: 'gcd' must be true or false")
        tgt = c.string(a, "target", w)
        if tgt not in ("enemy", "self"):
            c.err(f"{w}: target must be enemy or self")
        if tgt == "enemy" and not (isinstance(a.get("range"), (int, float)) and a["range"] > 0):
            c.err(f"{w}: enemy-target abilities need a range")
        if len(a.get("description", "")) > 256:
            c.err(f"{w}: description must be at most 256 chars")
        effects = a.get("effects")
        if not isinstance(effects, list) or not effects:
            c.err(f"{w}: effects must be a non-empty array")
            continue
        for i, e in enumerate(effects):
            e = c.obj(e, f"{w}.effects[{i}]")
            et = c.string(e, "type", f"{w}.effects[{i}]")
            if et not in EFFECT_TYPES:
                c.err(f"{w}.effects[{i}]: unknown effect type '{et}'")
            if et == "buff":
                if e.get("stat") not in BUFF_STATS:
                    c.err(f"{w}.effects[{i}]: unknown buff stat")
                if not (isinstance(e.get("duration"), (int, float)) and e["duration"] > 0):
                    c.err(f"{w}.effects[{i}]: buff duration must be > 0")
            if et == "stun" and not (isinstance(e.get("duration"), (int, float)) and e["duration"] > 0):
                c.err(f"{w}.effects[{i}]: stun duration must be > 0")
            if et == "area_threat" and not (isinstance(e.get("radius"), (int, float)) and e["radius"] > 0):
                c.err(f"{w}.effects[{i}]: area_threat radius must be > 0")

    items = c.list_of(load(content_dir / "items.json", c), "items", "items.json")
    item_ids = c.ids(items, "item")
    stacks = {}
    for it in items:
        w = f"item '{it.get('id')}'"
        c.string(it, "kind", w)
        st = c.integer(it, "stack", w)
        stacks[it.get("id")] = st
        if not 1 <= st <= 1000:
            c.err(f"{w}: stack must be 1..1000")
        if c.integer(it, "value", w) < 0:
            c.err(f"{w}: value must be >= 0")

    loot = c.list_of(load(content_dir / "loot.json", c), "loot_tables", "loot.json")
    loot_ids: set[str] = set()
    for t in loot:
        tid = c.string(t, "id", "loot table")
        if tid in loot_ids:
            c.err(f"duplicate loot table id '{tid}'")
        loot_ids.add(tid)
        w = f"loot table '{tid}'"
        lo, hi = c.integer(t, "coin_min", w), c.integer(t, "coin_max", w)
        if not 0 <= lo <= hi:
            c.err(f"{w}: need 0 <= coin_min <= coin_max")
        if not 0 <= c.integer(t, "max_drops", w) <= 16:
            c.err(f"{w}: max_drops must be 0..16")
        for d in t.get("drops", []) if isinstance(t.get("drops"), list) else []:
            d = c.obj(d, w)
            item = c.string(d, "item", w)
            if item not in item_ids:
                c.err(f"{w}: unknown item '{item}'")
            if not 0 <= c.number(d, "chance", w) <= 1:
                c.err(f"{w}: chance must be 0..1")
            mn, mx = c.integer(d, "min", w), c.integer(d, "max", w)
            if not 1 <= mn <= mx or (item in stacks and mx > stacks[item]):
                c.err(f"{w}: need 1 <= min <= max <= stack")

    mobs = c.list_of(load(content_dir / "mobs.json", c), "mobs", "mobs.json")
    mob_ids = c.ids(mobs, "mob")
    for m in mobs:
        w = f"mob '{m.get('id')}'"
        if not 1 <= c.integer(m, "level", w) <= max_level:
            c.err(f"{w}: level must be 1..max_level")
        c.string(m, "model", w)
        c.string(m, "faction", w)
        for k in ("scale", "hp_mult", "damage_mult", "attack_delay", "run_speed", "aggro_radius", "assist_radius",
                  "flee_hp_pct", "wander_radius", "leash_radius", "respawn_seconds"):
            c.number(m, k, w)
        for k in ("assist", "stun_immune"):
            if not isinstance(m.get(k), bool):
                c.err(f"{w}: '{k}' must be true or false")
        if isinstance(m.get("leash_radius"), (int, float)) and m["leash_radius"] < 10:
            c.err(f"{w}: leash_radius must be >= 10")
        if isinstance(m.get("flee_hp_pct"), (int, float)) and not 0 <= m["flee_hp_pct"] < 1:
            c.err(f"{w}: flee_hp_pct must be in [0, 1)")
        if m.get("loot") and m["loot"] not in loot_ids:
            c.err(f"{w}: unknown loot table '{m['loot']}'")

    for z in zones:
        zid = z.get("id", "?")
        b = z.get("bounds") if isinstance(z.get("bounds"), dict) else {}
        for i, sp in enumerate(z.get("spawns", []) if isinstance(z.get("spawns"), list) else []):
            sp = c.obj(sp, f"zone '{zid}'.spawns[{i}]")
            mob = c.string(sp, "mob", f"zone '{zid}'.spawns[{i}]")
            if mob not in mob_ids:
                c.err(f"zone '{zid}': spawn references unknown mob '{mob}'")
            x, zz = c.number(sp, "x", f"zone '{zid}'.spawns[{i}]"), c.number(sp, "z", f"zone '{zid}'.spawns[{i}]")
            if b and not (b.get("min_x", 0) <= x <= b.get("max_x", 0) and b.get("min_z", 0) <= zz <= b.get("max_z", 0)):
                c.err(f"zone '{zid}': spawn for '{mob}' is outside bounds")
            if not 1 <= sp.get("count", 1) <= 20:
                c.err(f"zone '{zid}': spawn count for '{mob}' must be 1..20")


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
