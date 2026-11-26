"""Phase 6g checks for tools/validate_content.py: mob levels against zone bands, a path from level 1 to
60 with no gaps, quests, factions and mob specials. Mirrors the server's content checks (content_world.cpp,
content_quests.cpp) so a broken file fails in CI before a server ever starts.
"""
from __future__ import annotations

# How far a camp may stray from its zone's band (bosses and named rares may sit at the top of it).
BAND_SLACK = 2
SPECIAL_TYPES = {"proc", "cast", "aoe", "enrage", "enrage_timer", "rampage", "summon", "adds", "support"}
MOB_SPELL_EFFECTS = {"damage", "dot", "debuff", "slow", "snare", "root", "stun", "fear", "mez"}
SUPPORT_SPELL_EFFECTS = {"heal", "hot", "buff"}  # Phase 15: friendly mob spells, cast by support specials


def check_bands(c, zones: list[dict], mobs: dict, max_level: int) -> None:
    """Every spawn's mob fits its zone's band; every level 1..max is covered by some reachable zone."""
    covered = [False] * (max_level + 2)
    for z in zones:
        kind = z.get("kind", "outdoor")
        if kind == "test":
            continue
        lv = z.get("levels", {}) or {}
        lo, hi = int(lv.get("min", 1)), int(lv.get("max", max_level))
        if kind in ("outdoor", "dungeon"):
            for L in range(lo, hi + 1):
                if 1 <= L <= max_level:
                    covered[L] = True
        for sp in z.get("spawns", []):
            for mid in (sp.get("mob"), sp.get("rare")):
                m = mobs.get(mid or "")
                if not m or m.get("guard"):
                    continue
                if kind == "hub":
                    c.err(f"zone '{z.get('id')}': hubs hold only guards, not '{mid}'")
                    continue
                if not lo - BAND_SLACK <= int(m.get("level", 1)) <= hi + BAND_SLACK:
                    c.err(f"zone '{z.get('id')}' (levels {lo}-{hi}): mob '{mid}' is level {m.get('level')}, outside the band")
    gaps = [L for L in range(1, max_level + 1) if not covered[L]]
    if gaps:
        c.err(f"no outdoor zone or dungeon covers levels {gaps[0]}..{gaps[-1]} (a dead end in the 1-{max_level} path)")


def check_mobs(c, mobs: dict, mob_spells: list[dict], factions: list[dict], races: set) -> None:
    fac_ids = set()
    for f in factions:
        fid = f.get("id")
        if fid in fac_ids:
            c.err(f"faction '{fid}': defined twice")
        fac_ids.add(fid)
        if not -2000 <= int(f.get("default", 0)) <= 2000:
            c.err(f"faction '{fid}': default must be within -2000..2000")
        for r in (f.get("race_mods") or {}):
            if r not in races:
                c.err(f"faction '{fid}': race_mods names unknown race '{r}'")
    for f in factions:
        for other, v in (f.get("hits") or {}).items():
            if other not in fac_ids:
                c.err(f"faction '{f.get('id')}': hits name unknown faction '{other}'")
            if not -500 <= int(v) <= 500:
                c.err(f"faction '{f.get('id')}': a hit must be within -500..500")
    spell_ids = set()
    friendly_ids = set()
    for s in mob_spells:
        sid = s.get("id")
        if sid in spell_ids:
            c.err(f"mob spell '{sid}': defined twice")
        spell_ids.add(sid)
        target = s.get("target")
        if target not in ("enemy", "friendly"):
            c.err(f"mob spell '{sid}': target must be enemy or friendly")
        if target == "friendly":
            friendly_ids.add(sid)
        allowed = SUPPORT_SPELL_EFFECTS if target == "friendly" else MOB_SPELL_EFFECTS
        for e in s.get("effects", []):
            if e.get("type") not in allowed:
                what = "heals and buffs only" if target == "friendly" else "harmful effects only"
                c.err(f"mob spell '{sid}': effect '{e.get('type')}' is not allowed ({what})")
    for mid, m in mobs.items():
        if fac_ids and m.get("faction") not in fac_ids:
            c.err(f"mob '{mid}': unknown faction '{m.get('faction')}'")
        if m.get("boss") and not m.get("named"):
            c.err(f"mob '{mid}': a boss must also be named")
        for s in m.get("specials", []):
            t = s.get("type")
            if t not in SPECIAL_TYPES:
                c.err(f"mob '{mid}': unknown special type '{t}'")
                continue
            if t in ("proc", "cast", "aoe", "support") and s.get("effect") not in spell_ids:
                c.err(f"mob '{mid}': special names unknown mob spell '{s.get('effect')}'")
            elif t in ("proc", "cast", "aoe", "support") and (t == "support") != (s.get("effect") in friendly_ids):
                c.err(f"mob '{mid}': " + ("support casts a friendly mob spell" if t == "support" else f"{t} casts an enemy mob spell"))
            if t == "support":
                w = f"mob '{mid}' support"
                if not 2 <= c.number(s, "every", w) <= 600:
                    c.err(f"mob '{mid}': support needs 'every' of 2..600 seconds")
                if not 0 < c.number(s, "range", w) <= 100:
                    c.err(f"mob '{mid}': support needs a range of (0, 100]")
                if "below" in s and not 0 <= c.number(s, "below", w) < 1:
                    c.err(f"mob '{mid}': support 'below' must be in [0, 1)")
            if t in ("proc", "rampage") and not 0 < float(s.get("chance", 0)) <= 1:
                c.err(f"mob '{mid}': {t} needs a chance in (0, 1]")
            if t in ("cast", "aoe", "summon") and not 2 <= float(s.get("every", 0)) <= 600:
                c.err(f"mob '{mid}': {t} needs 'every' of 2..600 seconds")
            if t == "adds":
                add = mobs.get(s.get("mob", ""))
                if add is None:
                    c.err(f"mob '{mid}': adds name unknown mob '{s.get('mob')}'")
                elif add.get("boss"):
                    c.err(f"mob '{mid}': adds cannot be bosses")
                at = s.get("at", [])
                if not at or any(not 0 < float(a) < 1 for a in at) or any(at[i] >= at[i - 1] for i in range(1, len(at))):
                    c.err(f"mob '{mid}': adds 'at' must fall from below 1 toward 0")
            if t == "enrage" and not 0 < float(s.get("below", 0)) < 1:
                c.err(f"mob '{mid}': enrage needs 'below' in (0, 1)")
            if t == "enrage_timer" and not 10 <= float(s.get("seconds", 0)) <= 3600:
                c.err(f"mob '{mid}': enrage_timer needs 'seconds' of 10..3600")


def check_quests(c, quests: list[dict], npcs: dict, mobs: dict, items: dict, factions: set, classes: set, max_level: int, spawned: set) -> None:
    ids = set()
    by_id = {q.get("id"): q for q in quests}

    def qnpc(w: str, nid, what: str) -> None:
        n = npcs.get(nid or "")
        if n is None:
            c.err(f"{w}: {what} '{nid}' is not an NPC")
        elif n.get("role") != "questgiver":
            c.err(f"{w}: {what} '{nid}' must have role questgiver")

    for q in quests:
        qid = q.get("id")
        w = f"quest '{qid}'"
        if qid in ids:
            c.err(f"{w}: defined twice")
        ids.add(qid)
        qnpc(w, q.get("giver"), "giver")
        if q.get("turn_in"):
            qnpc(w, q["turn_in"], "turn_in")
        lvl = int(q.get("level", 1))
        mn = int(q.get("min_level", max(1, lvl - 5)))
        if not 1 <= mn <= lvl <= max_level:
            c.err(f"{w}: levels must satisfy 1 <= min_level <= level <= max_level")
        for cl in q.get("classes", []):
            if cl not in classes:
                c.err(f"{w}: unknown class '{cl}'")
        for pre in q.get("prereqs", []):
            if pre not in by_id:
                c.err(f"{w}: unknown prerequisite '{pre}'")
            elif by_id[pre].get("repeatable"):
                c.err(f"{w}: prerequisite '{pre}' is repeatable, which never counts as done")
        steps = q.get("steps", [])
        if not 1 <= len(steps) <= 12:
            c.err(f"{w}: needs 1..12 steps")
        for s in steps:
            t = s.get("type")
            if t == "kill":
                for m in s.get("mobs", []):
                    if m not in mobs:
                        c.err(f"{w}: kill step names unknown mob '{m}'")
                    elif m not in spawned:
                        c.err(f"{w}: kill step names '{m}', which no zone spawns (a dead end)")
                if not s.get("mobs"):
                    c.err(f"{w}: a kill step needs mobs")
            elif t == "collect":
                it = items.get(s.get("item", ""))
                if it is None:
                    c.err(f"{w}: collect step names unknown item '{s.get('item')}'")
                for m in s.get("drops_from", []):
                    if m not in mobs:
                        c.err(f"{w}: drops_from names unknown mob '{m}'")
                if s.get("drops_from") and it is not None and it.get("kind") != "quest":
                    c.err(f"{w}: only quest items drop from 'drops_from'")
                if not s.get("drops_from") and it is not None and it.get("kind") == "quest":
                    c.err(f"{w}: quest item '{s.get('item')}' has no 'drops_from', so nothing ever drops it")
            elif t in ("talk", "deliver"):
                qnpc(w, s.get("npc"), "talk step npc")
                if s.get("item") and s["item"] not in items:
                    c.err(f"{w}: delivery names unknown item '{s['item']}'")
            else:
                c.err(f"{w}: step type must be kill, collect, talk or deliver")
            if not s.get("text"):
                c.err(f"{w}: each step needs a journal text")
        for g in q.get("give", []):
            if items.get(g.get("item", ""), {}).get("kind") != "quest":
                c.err(f"{w}: only quest items are handed out on accepting ('{g.get('item')}')")
        r = q.get("rewards", {}) or {}
        for it in r.get("items", []):
            if it.get("item") not in items:
                c.err(f"{w}: rewards unknown item '{it.get('item')}'")
        for f in (r.get("faction") or {}):
            if f not in factions:
                c.err(f"{w}: rewards standing with unknown faction '{f}'")
        if not 0 <= float(r.get("xp", 0)) <= 3:
            c.err(f"{w}: xp must be 0..3 levels")
    # Prerequisites never loop.
    state: dict = {}

    def visit(qid: str) -> bool:
        if state.get(qid) == 1:
            return False
        if state.get(qid) == 2:
            return True
        state[qid] = 1
        for pre in by_id.get(qid, {}).get("prereqs", []):
            if pre in by_id and not visit(pre):
                return False
        state[qid] = 2
        return True

    for qid in by_id:
        if not visit(qid):
            c.err(f"quest '{qid}': its prerequisites loop back to it")
            break
    # Every class has an epic chain that ends in a weapon it may use.
    for cl in sorted(classes):
        ends = [q for q in quests if q.get("epic") and q.get("classes") == [cl] and (q.get("rewards") or {}).get("items")]
        if not ends:
            c.err(f"class '{cl}': no epic quest chain ends in a reward")
            continue
        for q in ends:
            for it in q["rewards"]["items"]:
                item = items.get(it.get("item"), {})
                if item.get("rarity") != "epic" or item.get("classes") != [cl]:
                    c.err(f"quest '{q['id']}': the epic reward '{it.get('item')}' must be an epic item for {cl} alone")
                if not 46 <= int(item.get("req_level", 0)) <= 50:
                    c.err(f"quest '{q['id']}': the epic reward should be for level 46-50")
