#!/usr/bin/env python3
"""UI icons (Phase 9): one SVG per spell type and item category, drawn from simple shapes.

Writes client/assets/icons/<name>.svg: a white glyph with a dark outline on a transparent 64x64
square. The client tints the glyph (spells by resist school, items by category) and frames it by
rarity (client/scripts/ui/icons.gd), so a few dozen templates cover every spell, ability and item.
Original work, CC0, generated: no third-party input.

  python3 tools/gen_icons.py          # write the icons
  python3 tools/gen_icons.py --check  # exit 1 if a committed icon is stale or missing
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "client" / "assets" / "icons"


def poly(*pts: tuple[float, float]) -> str:
    return '<path d="M' + " L".join(f"{x:g} {y:g}" for x, y in pts) + ' Z"/>'


def circle(x: float, y: float, r: float) -> str:
    return f'<circle cx="{x:g}" cy="{y:g}" r="{r:g}"/>'


def rect(x: float, y: float, w: float, h: float, r: float = 0) -> str:
    return f'<rect x="{x:g}" y="{y:g}" width="{w:g}" height="{h:g}" rx="{r:g}"/>'


def ring(x: float, y: float, r: float, w: float) -> str:
    return f'<circle cx="{x:g}" cy="{y:g}" r="{r:g}" fill="none" stroke-width="{w:g}"/>'


def star(cx: float, cy: float, r1: float, r2: float, n: int = 5, rot: float = -90) -> str:
    pts = []
    for i in range(n * 2):
        r = r1 if i % 2 == 0 else r2
        a = math.radians(rot + i * 180 / n)
        pts.append((round(cx + r * math.cos(a), 2), round(cy + r * math.sin(a), 2)))
    return poly(*pts)


def path(d: str) -> str:
    return f'<path d="{d}"/>'


# Each icon: a list of shapes (filled white, outlined dark), drawn in a 64x64 box.
ICONS: dict[str, list[str]] = {
    # ---------------------------------------------------------------- spell and ability types
    "nuke": [poly((36, 4), (14, 36), (30, 36), (24, 60), (50, 24), (33, 24), (42, 4))],
    "heal": [rect(25, 8, 14, 48, 3), rect(8, 25, 48, 14, 3)],
    "hot": [ring(32, 32, 26, 5), rect(27, 15, 10, 34, 2), rect(15, 27, 34, 10, 2)],
    "dot": [path("M32 6 C32 6 12 32 12 42 A20 20 0 0 0 52 42 C52 32 32 6 32 6 Z")],
    "buff": [path("M32 5 L54 13 L54 30 C54 45 44 54 32 60 C20 54 10 45 10 30 L10 13 Z"),
             poly((32, 18), (43, 32), (36, 32), (36, 44), (28, 44), (28, 32), (21, 32))],
    "debuff": [poly((24, 6), (40, 6), (40, 32), (54, 32), (32, 58), (10, 32), (24, 32))],
    "cc": [ring(32, 32, 24, 5), ring(32, 32, 14, 5), circle(32, 32, 5)],
    "pet": [path("M32 58 C18 58 16 44 22 38 C27 33 37 33 42 38 C48 44 46 58 32 58 Z"),
            circle(14, 30, 7), circle(24, 16, 7), circle(40, 16, 7), circle(50, 30, 7)],
    "utility": [star(32, 33, 28, 12)],
    "passive": [ring(32, 32, 24, 8), circle(32, 32, 6)],
    "discipline": [rect(12, 24, 40, 30, 8), rect(12, 12, 10, 16, 4), rect(23, 9, 10, 18, 4), rect(34, 9, 10, 18, 4),
                   rect(45, 14, 9, 16, 4)],
    "song": [rect(38, 8, 6, 40, 2), poly((44, 8), (58, 14), (58, 22), (44, 16)),
             path("M28 44 A10 8 0 1 0 44 46 A10 8 0 1 0 28 44 Z")],
    # ---------------------------------------------------------------- weapons
    "sword": [poly((49, 6), (58, 6), (58, 15), (26, 47), (17, 38)), rect(11, 38, 6, 20, 2),
              poly((8, 40), (24, 56), (20, 60), (4, 44)), circle(9, 55, 5)],
    "dagger": [poly((46, 8), (56, 8), (56, 18), (32, 42), (22, 32)), poly((16, 32), (32, 48), (28, 52), (12, 36)),
               rect(10, 44, 8, 14, 3)],
    "spear": [poly((44, 4), (60, 4), (60, 20), (52, 18), (14, 56), (8, 50), (46, 12))],
    "mace": [rect(28, 30, 8, 30, 3), circle(32, 20, 14), poly((32, 0), (36, 8), (28, 8)), poly((50, 20), (44, 24), (44, 16)),
             poly((14, 20), (20, 16), (20, 24))],
    "hammer": [rect(28, 22, 8, 40, 3), rect(10, 6, 44, 18, 3)],
    "axe": [rect(28, 8, 8, 52, 3), path("M34 8 C56 8 60 26 56 38 C48 32 40 32 34 34 Z")],
    "staff": [rect(29, 16, 6, 46, 3), circle(32, 11, 9), ring(32, 11, 13, 3)],
    "bow": [path("M18 6 C46 12 46 52 18 58 L22 52 C38 46 38 18 22 12 Z"), rect(18, 8, 3, 48)],
    "fist": [rect(12, 24, 40, 30, 8), rect(12, 12, 10, 16, 4), rect(23, 9, 10, 18, 4), rect(34, 9, 10, 18, 4),
             rect(45, 14, 9, 16, 4)],
    "throwing": [star(32, 32, 28, 9, 4, -45), circle(32, 32, 6)],
    "arrow": [rect(30, 14, 4, 44, 1), poly((32, 2), (42, 18), (22, 18)), poly((30, 48), (22, 60), (30, 56)),
              poly((34, 48), (42, 60), (34, 56))],
    # ---------------------------------------------------------------- armour and jewellery
    "shield": [path("M32 4 L56 12 L56 30 C56 46 44 56 32 62 C20 56 8 46 8 30 L8 12 Z")],
    "head": [path("M10 40 C10 16 22 8 32 8 C42 8 54 16 54 40 L54 48 L42 48 L42 34 L22 34 L22 48 L10 48 Z")],
    "face": [path("M10 22 C10 12 54 12 54 22 C54 40 44 54 32 54 C20 54 10 40 10 22 Z"), rect(18, 26, 10, 6, 3), rect(36, 26, 10, 6, 3)],
    "chest": [poly((20, 6), (26, 12), (38, 12), (44, 6), (58, 16), (52, 28), (46, 24), (46, 58), (18, 58), (18, 24), (12, 28), (6, 16))],
    "shoulders": [path("M4 34 C4 18 18 12 28 16 L28 40 Z"), path("M60 34 C60 18 46 12 36 16 L36 40 Z")],
    "back": [poly((20, 6), (44, 6), (54, 58), (32, 52), (10, 58))],
    "arms": [rect(14, 14, 36, 14, 5), rect(18, 34, 28, 14, 5)],
    "hands": [path("M16 60 L16 30 C16 26 22 26 22 30 L22 14 C22 10 28 10 28 14 L28 10 C28 6 34 6 34 10 L34 12 C34 8 40 8 40 12 L40 18 C40 14 46 14 46 18 L46 40 C46 52 40 60 34 60 Z")],
    "waist": [rect(4, 26, 56, 12, 3), rect(24, 22, 16, 20, 3)],
    "legs": [poly((16, 6), (48, 6), (50, 58), (36, 58), (32, 24), (28, 58), (14, 58))],
    "feet": [path("M18 6 L34 6 L34 38 L54 44 C58 46 58 58 52 58 L14 58 C12 58 12 54 12 50 Z")],
    "ring": [ring(32, 38, 18, 7), poly((24, 12), (40, 12), (36, 22), (28, 22))],
    "neck": [path("M10 8 C10 30 22 40 32 40 C42 40 54 30 54 8"), poly((32, 36), (42, 46), (32, 60), (22, 46))],
    # ---------------------------------------------------------------- everything else
    "potion": [rect(26, 4, 12, 12, 2), path("M26 16 L26 24 C14 30 10 38 10 44 A22 18 0 0 0 54 44 C54 38 50 30 38 24 L38 16 Z")],
    "food": [path("M12 40 C12 24 22 16 34 16 C46 16 54 26 54 36 C54 48 44 56 30 56 C18 56 12 50 12 40 Z"),
             rect(44, 6, 6, 18, 3)],
    "bandage": [rect(6, 22, 52, 20, 6), rect(26, 22, 12, 20)],
    "scroll": [rect(14, 10, 36, 44, 3), rect(8, 6, 48, 10, 5), rect(8, 48, 48, 10, 5)],
    "bag": [path("M20 14 L44 14 L40 22 C54 28 58 40 56 50 C54 58 10 58 8 50 C6 40 10 28 24 22 Z"), rect(22, 6, 20, 10, 3)],
    "gem": [poly((18, 8), (46, 8), (58, 24), (32, 58), (6, 24))],
    "key": [ring(18, 20, 12, 7), rect(26, 18, 32, 8, 2), rect(46, 24, 6, 12), rect(54, 24, 5, 9)],
    "coin": [circle(32, 32, 26), ring(32, 32, 18, 3)],
}


def svg(shapes: list[str]) -> str:
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">\n'
            '<g fill="#ffffff" stroke="#1a1418" stroke-width="2.5" stroke-linejoin="round">\n'
            + "\n".join(shapes) + "\n</g>\n</svg>\n")


def main() -> int:
    check = "--check" in sys.argv
    OUT.mkdir(parents=True, exist_ok=True)
    stale = []
    for name, shapes in sorted(ICONS.items()):
        path_ = OUT / f"{name}.svg"
        text = svg(shapes)
        # A ring is a stroked outline: its stroke is the white part, so redraw it white over a dark edge.
        text = text.replace('fill="none" stroke-width=', 'fill="none" stroke="#ffffff" stroke-width=')
        if check:
            if not path_.exists() or path_.read_text(encoding="utf-8") != text:
                stale.append(path_.name)
        else:
            path_.write_text(text, encoding="utf-8", newline="\n")
    extra = [p.name for p in OUT.glob("*.svg") if p.stem not in ICONS]
    if check and (stale or extra):
        print("stale or extra icons (run python3 tools/gen_icons.py):", ", ".join(stale + extra))
        return 1
    if not check:
        for p in OUT.glob("*.svg"):
            if p.stem not in ICONS:
                p.unlink()
        print(f"wrote {len(ICONS)} icons to {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
