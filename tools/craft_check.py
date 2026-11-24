"""Phase 13 content checks for content/crafts.json and the station NPCs (mirrors server/src/game/content_crafts.cpp),
plus what the server cannot see: that every recipe can be made from what the world offers, and that each
station stands clear of its city scene's walls and props.

Used by tools/validate_content.py.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_crafts import vendor_profits  # noqa: E402  (the same rule the generator prices by)

RULE_KEYS = ("max_skill", "combine_seconds", "success_max", "success_min", "success_per_point", "skill_up_base", "skill_up_per_point",
             "skill_up_max", "fail_loss_chance")


def _inside_box(x: float, z: float, w: dict, margin: float) -> bool:
    """A point in a wall's frame, as the server takes it (game/world_geom.cpp box_local)."""
    c, s = math.cos(w.get("yaw", 0.0)), math.sin(w.get("yaw", 0.0))
    dx, dz = x - w.get("x", 0.0), z - w.get("z", 0.0)
    lx, lz = dx * c - dz * s, dx * s + dz * c
    return abs(lx) < w.get("hx", 0.0) + margin and abs(lz) < w.get("hz", 0.0) + margin


def _key(v):
    """A reference fit for a dict or set lookup: a string, or None (the field's own check reports a bad one)."""
    return v if isinstance(v, str) else None


def check_crafts(c, doc, items: dict, npcs: list[dict], skill_ids: set, zones: list[dict], loot: list[dict], game_rules: dict,
                 mobs: list[dict] | None = None) -> None:
    if not isinstance(doc, dict):
        c.err("crafts.json: must be a JSON object")
        return
    rules = c.obj(doc.get("rules"), "crafts.rules")
    max_skill = 300
    if isinstance(rules, dict):
        for k in RULE_KEYS:
            if k == "max_skill":
                max_skill = c.integer(rules, k, "crafts.rules")
            else:
                c.number(rules, k, "crafts.rules")
        if not 1 <= max_skill <= 1000:
            c.err("crafts.rules.max_skill must be 1..1000")
        num = {k: rules.get(k) for k in RULE_KEYS if isinstance(rules.get(k), (int, float)) and not isinstance(rules.get(k), bool)}
        if "combine_seconds" in num and not 0.1 <= num["combine_seconds"] <= 60:
            c.err("crafts.rules.combine_seconds must be 0.1..60")
        unit = ("success_max", "success_min", "success_per_point", "skill_up_base", "skill_up_per_point", "skill_up_max", "fail_loss_chance")
        for k in unit:
            if k in num and not 0 <= num[k] <= 1:
                c.err(f"crafts.rules.{k} must be 0..1")
        if num.get("success_min", 0) > num.get("success_max", 1):
            c.err("crafts.rules: success_min must not exceed success_max")
        if num.get("skill_up_base", 0) > num.get("skill_up_max", 1):
            c.err("crafts.rules: skill_up_base must not exceed skill_up_max")

    tradeskills = c.list_of(doc, "tradeskills", "crafts.json")
    ts_by_id: dict[str, dict] = {}
    stations: dict[str, str] = {}
    for t in tradeskills:
        tid = c.string(t, "id", "crafts.tradeskills")
        w = f"tradeskill '{tid}'"
        st = ""
        for key, most in (("name", 32), ("station", 32), ("station_name", 48)):
            v = c.string(t, key, w)
            if not 1 <= len(v) <= most:
                c.err(f"{w}: {key} must be 1..{most} characters")
            if key == "station":
                st = v  # the checked string ("" when it was not one)
        if not 1 <= len(tid) <= 32:
            c.err(f"{w}: id must be 1..32 characters")
        if tid in ts_by_id:
            c.err(f"{w}: listed twice")
        if tid in skill_ids:
            c.err(f"{w}: has the id of a combat skill in skills.json")
        if st in stations:
            c.err(f"{w}: shares its station kind '{st}' with tradeskill '{stations[st]}'")
        if st:
            stations[st] = tid
        if tid:
            ts_by_id[tid] = t
    if len(tradeskills) > 16:
        c.err("crafts.tradeskills: at most 16")

    recipes = c.list_of(doc, "recipes", "crafts.json")
    seen_ids: set = set()
    used: set = set()
    top: dict[str, int] = {}  # tradeskill -> its highest trivial
    for r in recipes:
        rid = c.string(r, "id", "crafts.recipes")
        w = f"recipe '{rid}'"
        if not 1 <= len(rid) <= 48:
            c.err(f"{w}: id must be 1..48 characters")
        if rid in seen_ids:
            c.err(f"{w}: listed twice")
        seen_ids.add(rid)
        ts = _key(r.get("tradeskill"))
        if ts not in ts_by_id:
            c.err(f"{w}: unknown tradeskill '{r.get('tradeskill')}'")
        else:
            used.add(ts)
        trivial = c.integer(r, "trivial", w)
        if not 1 <= trivial <= max_skill:
            c.err(f"{w}: trivial must be 1..max_skill")
        if ts in ts_by_id:
            top[ts] = max(top.get(ts, 0), trivial)
        res = c.obj(r.get("result"), f"{w}.result")
        out = items.get(_key(res.get("item"))) if isinstance(res, dict) else None
        if isinstance(res, dict):
            n = c.integer(res, "count", f"{w}.result")
            if out is None:
                c.err(f"{w}: makes unknown item '{res.get('item')}'")
            else:
                if not 1 <= n <= max(1, out.get("stack", 1)):
                    c.err(f"{w}: makes 1..stack of its item")
                if out.get("summoned"):
                    c.err(f"{w}: cannot make a summoned item")
                if out.get("lore") and n != 1:
                    c.err(f"{w}: makes a lore item, so only one at a time")
        ings = r.get("ingredients")
        if not isinstance(ings, list) or not 1 <= len(ings) <= 8:
            c.err(f"{w}: needs 1..8 ingredients")
            ings = ings if isinstance(ings, list) else []
        names: set = set()
        for ing in ings:
            iid = _key(ing.get("item")) if isinstance(ing, dict) else None
            d = items.get(iid)
            if d is None:
                c.err(f"{w}: uses unknown item '{iid}'")
            elif d.get("summoned") or d.get("kind") == "bag":
                c.err(f"{w}: cannot use '{iid}' (summoned items and bags are not materials)")
            cnt = ing.get("count") if isinstance(ing, dict) else None
            if not isinstance(cnt, int) or isinstance(cnt, bool) or not 1 <= cnt <= 100:
                c.err(f"{w}: uses 1..100 of each ingredient")
            if iid is not None and iid in names:
                c.err(f"{w}: lists '{iid}' twice")
            if iid is not None:
                names.add(iid)
            if isinstance(res, dict) and iid == res.get("item"):
                c.err(f"{w}: uses what it makes")
        tools = r.get("tools", [])
        if not isinstance(tools, list) or len(tools) > 4:
            c.err(f"{w}: needs at most 4 tools")
            tools = []
        for tool in tools:
            tool = _key(tool)
            if tool not in items:
                c.err(f"{w}: needs unknown tool '{tool}'")
            if tool in names:
                c.err(f"{w}: lists '{tool}' as both a tool and an ingredient")

    # A skill is taught only below a recipe's trivial: each tradeskill needs one trivial at max_skill to reach its cap.
    for tid in ts_by_id:
        best = top.get(tid, 0)
        if best < max_skill:
            c.err(f"tradeskill '{tid}': no recipe teaches it to max_skill {max_skill} (the highest trivial is {best})")

    # Stations: a known kind, nothing else on them, clear of the scene's walls and props; every tradeskill with
    # recipes has one.
    zone_by_id = {z.get("id"): z for z in zones if isinstance(z, dict) and isinstance(z.get("id"), str)}
    placed: set = set()
    for n in npcs:
        if n.get("role") != "station":
            if n.get("station"):
                c.err(f"npc '{n.get('id')}': only stations name a station kind")
            continue
        w = f"npc '{n.get('id')}'"
        kind = _key(n.get("station"))
        if kind not in stations:
            c.err(f"{w}: a station needs the station kind of a tradeskill (got '{n.get('station')}')")
        if n.get("classes") or n.get("sells") or n.get("stock") or n.get("travel"):
            c.err(f"{w}: a station teaches, sells and offers travel to nothing")
        z = zone_by_id.get(_key(n.get("zone")))
        if z is None:
            c.err(f"{w}: unknown zone '{n.get('zone')}'")
            continue
        if kind in stations:
            placed.add(kind)  # only a station in a real zone counts
        x, zz = n.get("x", 0.0), n.get("z", 0.0)
        for wall in z.get("walls", []):
            if _inside_box(x, zz, wall, 1.0):
                c.err(f"{w}: stands inside a wall of the '{z.get('id')}' scene")
        for o in z.get("obstacles", []):
            if math.hypot(x - o.get("x", 0.0), zz - o.get("z", 0.0)) < o.get("radius", 0.0) + 1.0:
                c.err(f"{w}: stands inside a prop of the '{z.get('id')}' scene")
    for tid, t in ts_by_id.items():
        if tid in used and _key(t.get("station")) not in placed:
            c.err(f"tradeskill '{tid}': has recipes but no '{t.get('station')}' station")

    # Everything a recipe needs can be bought, looted or made.
    def positive(v) -> bool:
        return isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0

    # Recipes whose shape is sound (the errors above name the rest): these take part in the analyses below.
    def sound(r: dict) -> bool:
        res, ings, tools = r.get("result"), r.get("ingredients"), r.get("tools", [])
        return (isinstance(res, dict) and _key(res.get("item")) in items and isinstance(res.get("count"), int) and res["count"] >= 1
                and isinstance(ings, list) and bool(ings) and isinstance(tools, list) and all(_key(t) in items for t in tools)
                and all(isinstance(i, dict) and _key(i.get("item")) in items and isinstance(i.get("count"), int) and i["count"] >= 1 for i in ings))
    good = [r for r in recipes if sound(r)]
    def given(d: dict, key: str, default) -> bool:  # positive, taking an omitted value as the server's default
        return positive(d.get(key, default))

    have = {_key(s.get("item")) for n in npcs for s in n.get("stock", []) if isinstance(s, dict)}
    # Only the loot of monsters the world spawns (a zone's spawns and their named rares), when the mobs are known.
    awarded = None
    if mobs is not None:
        placed_mobs = {_key(s.get(k)) for z in zones if isinstance(z, dict) for s in z.get("spawns", []) if isinstance(s, dict)
                       for k in ("mob", "rare")}
        awarded = {_key(m.get("loot")) for m in mobs if isinstance(m, dict) and _key(m.get("id")) in placed_mobs}
    for t in loot:
        if not isinstance(t, dict) or (awarded is not None and _key(t.get("id")) not in awarded):
            continue  # no spawned monster carries it
        if not given(t, "max_drops", 1):
            continue  # a table that awards nothing
        have |= {_key(d.get("item")) for d in t.get("drops", []) if isinstance(d, dict) and positive(d.get("chance"))}
        have |= {_key(e.get("item")) for p in t.get("pools", []) if isinstance(p, dict) and given(p, "chance", 1.0) and given(p, "rolls", 1)
                 for e in p.get("entries", []) if isinstance(e, dict) and given(e, "weight", 1.0)}
    grew = True
    while grew:
        grew = False
        for r in good:
            need = [i["item"] for i in r["ingredients"]] + list(r.get("tools", []))
            if r["result"]["item"] not in have and all(x in have for x in need):
                have.add(r["result"]["item"])
                grew = True
    for r in good:
        need = [i["item"] for i in r["ingredients"]] + list(r.get("tools", []))
        missing = sorted({x for x in need if x not in have})
        if missing:  # every input, even of a recipe whose result some vendor also sells
            c.err(f"recipe '{r.get('id')}': cannot be made from what vendors sell, monsters drop or recipes make (missing {missing[:3]})")

    # No buy-craft-sell loop: a recipe made only from bought things never sells back for more than they cost.
    econ = game_rules.get("economy") if isinstance(game_rules, dict) else None
    if isinstance(econ, dict) and all(isinstance(econ.get(k), (int, float)) for k in ("min_buy", "max_sell")):
        sold = {s.get("item") for n in npcs for s in n.get("stock", []) if isinstance(s, dict)}
        for rid, cost, paid, raws in vendor_profits(good, items, sold, econ):
            c.err(f"recipe '{rid}': made from bought {', '.join(raws[:3])} for {cost:.0f} copper, it sells for {paid} (raise the materials' value)")


def validate_crafts(c, content_dir: Path, load, items: dict, npcs: list[dict], zones: list[dict]) -> None:
    skills = load(content_dir / "skills.json", c)
    skill_ids = {s.get("id") for s in skills.get("skills", [])} if isinstance(skills, dict) else set()
    loot = load(content_dir / "loot.json", c)
    tables = loot.get("loot_tables", []) if isinstance(loot, dict) else []
    rules = load(content_dir / "rules.json", c)
    mobs = load(content_dir / "mobs.json", c)
    mob_list = mobs.get("mobs") if isinstance(mobs, dict) and isinstance(mobs.get("mobs"), list) else None
    check_crafts(c, load(content_dir / "crafts.json", c), items, npcs, skill_ids, zones, tables, rules if isinstance(rules, dict) else {},
                 mob_list)
