"""Canonical order of content/npcs.json, shared by every generator that writes part of it
(gen_classes: trainers and lockboxes; gen_items: vendors, bankers, auctioneers; gen_world: world NPCs)."""
from __future__ import annotations

ROLE_ORDER = ["trainer", "lockbox", "vendor", "banker", "auctioneer", "binder", "teleporter", "ferry", "guard", "questgiver", "citizen"]


def ordered(npcs: list[dict]) -> list[dict]:
    """Stable sort by role (unknown roles last), the world's hub clones after the originals of the same
    role, keeping each generator's own order otherwise."""
    rank = {r: i for i, r in enumerate(ROLE_ORDER)}
    return sorted(npcs, key=lambda n: (rank.get(n.get("role", ""), len(ROLE_ORDER)), n.get("origin") == "world"))


COMMENT = ("Non-combat NPCs (generated: trainers and lockboxes by tools/gen_classes.py; vendors, bankers and auctioneers by "
           "tools/gen_items.py; soulbinders, teleporters and ferries by tools/gen_world.py). Each generator keeps the others' rows. "
           "A zone scene's npc markers override x/z/yaw (content/zones/scenes).")


def render(npcs: list[dict]) -> str:
    import json
    lines = ["{", '  "_comment": ' + json.dumps(COMMENT) + ",", '  "npcs": [']
    rows = ordered(npcs)
    for i, r in enumerate(rows):
        lines.append("    " + json.dumps(r) + ("," if i < len(rows) - 1 else ""))
    lines += ["  ]", "}", ""]
    return "\n".join(lines)
