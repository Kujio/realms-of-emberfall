"""Diff-friendly JSON for zone files: one line per spawn, obstacle, wall, line and entry."""
from __future__ import annotations

import json

_LISTS = ("spawns", "obstacles", "walls", "zone_lines", "entries", "npcs", "hills", "props")


def _line(v: object) -> str:
    return json.dumps(v, ensure_ascii=False)


def format_zone(z: dict) -> str:
    """One zone object (content/zones/<id>.json or a scene export)."""
    out = ["{"]
    keys = list(z.keys())
    for ki, k in enumerate(keys):
        comma = "" if ki == len(keys) - 1 else ","
        v = z[k]
        if k in _LISTS and isinstance(v, list) and v:
            out.append(f'  "{k}": [')
            for i, item in enumerate(v):
                out.append("    " + _line(item) + ("," if i < len(v) - 1 else ""))
            out.append("  ]" + comma)
        elif k == "terrain" and isinstance(v, dict) and v.get("hills"):
            out.append('  "terrain": {')
            tk = list(v.keys())
            for ti, t in enumerate(tk):
                tcomma = "" if ti == len(tk) - 1 else ","
                if t == "hills":
                    out.append('    "hills": [')
                    for i, h in enumerate(v[t]):
                        out.append("      " + _line(h) + ("," if i < len(v[t]) - 1 else ""))
                    out.append("    ]" + tcomma)
                else:
                    out.append(f'    "{t}": {_line(v[t])}' + tcomma)
            out.append("  }" + comma)
        else:
            out.append(f'  "{k}": {_line(v)}' + comma)
    out += ["}", ""]
    return "\n".join(out)
