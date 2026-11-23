#!/usr/bin/env python3
"""Prop library for zone scenes: buildings, walls, trees and rocks. Props with art in
tools/world_skins.py are drawn by those models (and get a scene per biome variant,
<prop>__<biome>.tscn); the rest are built from primitives.

Writes client/zones/props/<id>.tscn (CC0, generated, committed). Each prop is a Node3D with meshes
and a StaticBody3D whose CollisionShape3D children (BoxShape3D / CylinderShape3D) are what the zone
export tool (tools/zone_export/export_zones.gd) turns into server collision. tools/gen_world.py
scatters the same props over outdoor zones using COLLISION below, so the radii always agree.

  python3 tools/world_props.py          # write the scenes
  python3 tools/world_props.py --check  # exit 1 if any committed scene is stale
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from world_skins import DRAW, SKINS  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "client" / "zones" / "props"

# Colours (linear-ish sRGB triples).
C = {
    "plaster": (0.78, 0.70, 0.58), "timber": (0.42, 0.30, 0.20), "roof": (0.45, 0.22, 0.16), "slate": (0.30, 0.32, 0.36),
    "stone": (0.52, 0.50, 0.47), "darkstone": (0.33, 0.31, 0.30), "bark": (0.36, 0.25, 0.16), "leaf": (0.27, 0.45, 0.20),
    "pine": (0.17, 0.33, 0.20), "rock": (0.46, 0.44, 0.41), "ember": (1.0, 0.45, 0.12), "canvas": (0.72, 0.30, 0.22),
    "gold": (0.80, 0.64, 0.25), "rune": (0.35, 0.65, 1.0), "plank": (0.50, 0.38, 0.24), "marble": (0.85, 0.84, 0.80),
    "deadwood": (0.40, 0.36, 0.30), "frost": (0.80, 0.88, 0.95), "ash": (0.25, 0.23, 0.22), "reed": (0.55, 0.58, 0.30),
    "mushroom": (0.55, 0.30, 0.45),
}

# Meshes: (kind, size/params, position, colour, extra). kinds: box (sx, sy, sz), cyl (r_top, r_bottom, h), sphere (r, h),
# prism (sx, sy, sz). Rotation about Y in degrees as an optional 6th element; "glow" in extra makes it emissive.
# Collision: ("box", sx, sz, x, z, yaw_deg) or ("cyl", r, x, z).
PROPS: dict[str, dict] = {
    "house_small": {"mesh": [("box", (8, 5, 6), (0, 2.5, 0), "plaster"), ("prism", (8.6, 2.6, 6.6), (0, 6.3, 0), "roof"),
                             ("box", (1.4, 2.4, 0.2), (0, 1.2, 3.05), "timber")],
                    "collision": [("box", 8, 6, 0, 0, 0)]},
    "house_large": {"mesh": [("box", (12, 7, 10), (0, 3.5, 0), "plaster"), ("prism", (12.8, 3.4, 10.8), (0, 8.7, 0), "slate"),
                             ("box", (2, 3, 0.2), (0, 1.5, 5.05), "timber"), ("box", (1.2, 3, 1.2), (4, 9, 2), "stone")],
                    "collision": [("box", 12, 10, 0, 0, 0)]},
    "wall_segment": {"mesh": [("box", (20, 6, 2), (0, 3, 0), "stone"), ("box", (20, 0.8, 2.6), (0, 6.4, 0), "darkstone")],
                     "collision": [("box", 20, 2, 0, 0, 0)]},
    "wall_tower": {"mesh": [("cyl", (3.5, 3.8, 12), (0, 6, 0), "stone"), ("cyl", (0.2, 4.2, 4), (0, 14, 0), "roof")],
                   "collision": [("cyl", 3.8, 0, 0)]},
    "gate_arch": {"mesh": [("box", (2.5, 9, 3), (-5.5, 4.5, 0), "stone"), ("box", (2.5, 9, 3), (5.5, 4.5, 0), "stone"),
                           ("box", (13.5, 2, 3), (0, 9.5, 0), "darkstone")],
                  "collision": [("box", 2.5, 3, -5.5, 0, 0), ("box", 2.5, 3, 5.5, 0, 0)]},
    "forge": {"mesh": [("cyl", (4, 4.4, 2), (0, 1, 0), "darkstone"), ("cyl", (2.6, 2.6, 0.6), (0, 2.2, 0), "ember", "glow"),
                       ("box", (1.6, 9, 1.6), (3, 4.5, -3), "stone")],
              "collision": [("cyl", 4.4, 0, 0)], "light": (0, 3.5, 0, "ember", 3.0, 18.0)},
    "market_stall": {"mesh": [("box", (4, 1, 3), (0, 0.5, 0), "plank"), ("prism", (4.6, 1.2, 3.6), (0, 3.4, 0), "canvas"),
                              ("box", (0.2, 3, 0.2), (-1.9, 1.5, 1.4), "timber"), ("box", (0.2, 3, 0.2), (1.9, 1.5, 1.4), "timber")],
                     "collision": [("box", 4, 3, 0, 0, 0)]},
    "tree_oak": {"mesh": [("cyl", (0.4, 0.6, 4.5), (0, 2.25, 0), "bark"), ("sphere", (3.2, 5), (0, 6, 0), "leaf")],
                 "collision": [("cyl", 0.7, 0, 0)]},
    "tree_pine": {"mesh": [("cyl", (0.3, 0.45, 3), (0, 1.5, 0), "bark"), ("cyl", (0.0, 2.6, 7), (0, 6, 0), "pine")],
                  "collision": [("cyl", 0.6, 0, 0)]},
    "tree_dead": {"mesh": [("cyl", (0.2, 0.5, 6), (0, 3, 0), "deadwood"), ("box", (2.4, 0.25, 0.25), (0.8, 4.5, 0), "deadwood", "", 25)],
                  "collision": [("cyl", 0.6, 0, 0)]},
    "rock": {"mesh": [("sphere", (1.7, 2.2), (0, 0.6, 0), "rock")], "collision": [("cyl", 1.6, 0, 0)]},
    "boulder": {"mesh": [("sphere", (3.4, 4.6), (0, 1.4, 0), "rock")], "collision": [("cyl", 3.2, 0, 0)]},
    "crystal": {"mesh": [("cyl", (0.0, 1.0, 4), (0, 2, 0), "frost", "glow")], "collision": [("cyl", 1.0, 0, 0)]},
    "mushroom": {"mesh": [("cyl", (0.35, 0.45, 2.5), (0, 1.25, 0), "plaster"), ("sphere", (2.0, 1.4), (0, 2.6, 0), "mushroom")],
                 "collision": [("cyl", 0.6, 0, 0)]},
    "reeds": {"mesh": [("cyl", (0.6, 0.9, 1.6), (0, 0.8, 0), "reed")], "collision": []},
    "well": {"mesh": [("cyl", (1.3, 1.4, 1.1), (0, 0.55, 0), "stone"), ("box", (0.2, 2.4, 0.2), (-1.1, 1.2, 0), "timber"),
                      ("box", (0.2, 2.4, 0.2), (1.1, 1.2, 0), "timber"), ("prism", (2.8, 0.8, 1.6), (0, 2.6, 0), "roof")],
             "collision": [("cyl", 1.4, 0, 0)]},
    "bank_hall": {"mesh": [("box", (16, 8, 12), (0, 4, 0), "marble"), ("prism", (17, 3, 13), (0, 9.5, 0), "slate"),
                           ("cyl", (0.6, 0.6, 8), (-5, 4, 6.6), "marble"), ("cyl", (0.6, 0.6, 8), (5, 4, 6.6), "marble"),
                           ("box", (3, 4, 0.2), (0, 2, 6.05), "gold")],
                  "collision": [("box", 16, 12, 0, 0, 0)]},
    "temple": {"mesh": [("box", (14, 10, 18), (0, 5, 0), "marble"), ("prism", (15, 4, 19), (0, 12, 0), "roof"),
                        ("cyl", (0.1, 2, 10), (0, 19, -6), "gold")],
               "collision": [("box", 14, 18, 0, 0, 0)]},
    "waystone": {"mesh": [("box", (1.6, 5, 1.6), (0, 2.5, 0), "darkstone"), ("box", (1.7, 0.6, 1.7), (0, 3.6, 0), "rune", "glow")],
                 "collision": [("cyl", 1.2, 0, 0)], "light": (0, 4.0, 0, "rune", 1.6, 10.0)},
    "lamp_post": {"mesh": [("cyl", (0.12, 0.15, 4), (0, 2, 0), "darkstone"), ("sphere", (0.35, 0.6), (0, 4.2, 0), "ember", "glow")],
                  "collision": [("cyl", 0.25, 0, 0)], "light": (0, 4.2, 0, "ember", 1.2, 9.0)},
    "crates": {"mesh": [("box", (1.5, 1.5, 1.5), (0, 0.75, 0), "plank"), ("box", (1.2, 1.2, 1.2), (0.3, 2.1, 0.2), "plank", "", 20)],
               "collision": [("box", 1.6, 1.6, 0, 0, 0)]},
    "fence": {"mesh": [("box", (8, 0.2, 0.15), (0, 1.0, 0), "timber"), ("box", (8, 0.2, 0.15), (0, 0.5, 0), "timber"),
                       ("box", (0.2, 1.3, 0.2), (-3.9, 0.65, 0), "timber"), ("box", (0.2, 1.3, 0.2), (3.9, 0.65, 0), "timber")],
              "collision": [("box", 8, 0.4, 0, 0, 0)]},
    "dock": {"mesh": [("box", (6, 0.5, 22), (0, 0.25, 0), "plank"), ("cyl", (0.3, 0.3, 2.5), (-2.8, 0, 10.5), "timber"),
                      ("cyl", (0.3, 0.3, 2.5), (2.8, 0, 10.5), "timber")],
             "collision": []},
    "boat": {"mesh": [("box", (4, 1.4, 12), (0, 0.4, 0), "timber"), ("cyl", (0.15, 0.2, 9), (0, 5, 0), "timber"),
                      ("box", (0.1, 5, 4), (0, 5.5, 0.5), "canvas")],
             "collision": []},
    "cave_mouth": {"mesh": [("sphere", (5, 8), (-6, 2.5, 0), "darkstone"), ("sphere", (5, 8), (6, 2.5, 0), "darkstone"),
                            ("box", (8, 2.5, 5), (0, 7.5, 0), "darkstone"), ("box", (5, 6, 0.3), (0, 3, -1.5), "ash")],
                   "collision": [("cyl", 4.6, -6, 0), ("cyl", 4.6, 6, 0)]},
    "ruin_pillar": {"mesh": [("cyl", (0.9, 1.0, 5), (0, 2.5, 0), "stone"), ("box", (2.2, 0.6, 2.2), (0, 0.3, 0), "darkstone")],
                    "collision": [("cyl", 1.1, 0, 0)]},
    "ruin_wall": {"mesh": [("box", (10, 3.5, 1.6), (0, 1.75, 0), "stone"), ("box", (4, 1.5, 1.6), (-3, 4.2, 0), "stone")],
                  "collision": [("box", 10, 1.6, 0, 0, 0)]},
    "tent": {"mesh": [("prism", (4, 3, 5), (0, 1.5, 0), "canvas")], "collision": [("box", 4, 5, 0, 0, 0)]},
    "brazier": {"mesh": [("cyl", (0.8, 0.5, 1.2), (0, 0.6, 0), "darkstone"), ("sphere", (0.6, 0.8), (0, 1.4, 0), "ember", "glow")],
                "collision": [("cyl", 0.8, 0, 0)], "light": (0, 2.0, 0, "ember", 2.0, 12.0)},
}


def collision_radius(prop: str) -> float:
    """The single circle a scattered prop collides as (gen_world.py); 0 = walk-through."""
    shapes = PROPS[prop]["collision"]
    if not shapes:
        return 0.0
    s = shapes[0]
    if s[0] != "cyl" or len(shapes) != 1 or s[2] != 0 or s[3] != 0:
        raise ValueError(f"prop {prop} is not a single centred cylinder; place it in a scene instead")
    return float(s[1])


def _v(t: tuple) -> str:
    return "Vector3(" + ", ".join(_num(x) for x in t) + ")"


def _num(x: float) -> str:
    return f"{float(x):.4g}" if float(x) != int(x) else str(int(x))


def _yaw_basis(deg: float) -> str:
    import math
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    rows = [c, 0, s, 0, 1, 0, -s, 0, c]
    return ", ".join(f"{v:.6g}" for v in rows)


def _xform(pos: tuple, yaw_deg: float = 0.0) -> str:
    return f"Transform3D({_yaw_basis(yaw_deg)}, {', '.join(_num(p) for p in pos)})"


def _scaled_xform(pos: tuple, yaw_deg: float, scale) -> str:
    """Rows of rotate-about-Y times scale (Godot writes a basis row by row), then the origin."""
    import math
    sx, sy, sz = (scale, scale, scale) if isinstance(scale, (int, float)) else scale
    a = math.radians(yaw_deg)
    c, s = math.cos(a), math.sin(a)
    rows = [c * sx, 0, s * sz, 0, sy, 0, -s * sx, 0, c * sz]
    return f"Transform3D({', '.join(f'{v:.6g}' for v in rows)}, {', '.join(_num(p) for p in pos)})"


def _variants(key: str) -> list[dict]:
    return [v if isinstance(v, dict) else {"parts": v} for v in SKINS[key]]


ENV_DIR = "res://assets/env"


def scene(prop: str, skin: str | None = None) -> str:
    """The scene for `prop`, drawn by SKINS[skin] when given, else by primitives."""
    spec = PROPS[prop]
    res: list[str] = []
    nodes: list[str] = []
    mats: dict[tuple, str] = {}
    exts: list[str] = []
    n = 0
    name = skin or prop

    def material(colour: str, glow: bool) -> str:
        key = (colour, glow)
        if key not in mats:
            mid = f"mat_{colour}{'_glow' if glow else ''}"
            r, g, b = C[colour]
            body = [f'[sub_resource type="StandardMaterial3D" id="{mid}"]', f"albedo_color = Color({r}, {g}, {b}, 1)", "roughness = 0.9"]
            if glow:
                body += ["emission_enabled = true", f"emission = Color({r}, {g}, {b}, 1)", "emission_energy_multiplier = 2.0"]
            res.append("\n".join(body))
            mats[key] = mid
        return mats[key]

    if skin:
        exts.append('[ext_resource type="Script" path="res://scripts/world/prop_variants.gd" id="script"]')
        models: dict[str, str] = {}
        for v in _variants(skin):
            for part in v["parts"]:
                if part[0] not in models:
                    models[part[0]] = f"m_{part[0]}"
                    exts.append(f'[ext_resource type="PackedScene" path="{ENV_DIR}/{part[0]}.glb" id="m_{part[0]}"]')
        root = f'[node name="{name}" type="Node3D"]\nscript = ExtResource("script")'
        if DRAW.get(prop, 0.0) > 0.0:
            root += f"\ndraw_distance = {DRAW[prop]}"
        nodes.append(root)
        for vi, v in enumerate(_variants(skin)):
            vnode = f'[node name="V{vi}" type="Node3D" parent="."]\nvisible = false'
            if "tint" in v:
                h = v["tint"].lstrip("#")
                r, g, b = (int(h[k:k + 2], 16) / 255 for k in (0, 2, 4))
                vnode += f"\nmetadata/tint = Color({r:.4g}, {g:.4g}, {b:.4g}, 1)"
            nodes.append(vnode)
            for pi, (model, scale, pos, yaw) in enumerate(v["parts"]):
                nodes.append(f'[node name="P{pi}" parent="V{vi}" instance=ExtResource("{models[model]}")]\n'
                             f"transform = {_scaled_xform(pos, yaw, scale)}")
    else:
        nodes.append(f'[node name="{prop}" type="Node3D"]')
    for i, m in enumerate(spec["mesh"] if not skin else []):
        kind, size, pos, colour = m[0], m[1], m[2], m[3]
        glow = len(m) > 4 and m[4] == "glow"
        yaw = m[5] if len(m) > 5 else 0.0
        mid = f"mesh{i}"
        if kind == "box":
            res.append(f'[sub_resource type="BoxMesh" id="{mid}"]\nsize = {_v(size)}')
        elif kind == "prism":
            res.append(f'[sub_resource type="PrismMesh" id="{mid}"]\nsize = {_v(size)}')
        elif kind == "cyl":
            res.append(f'[sub_resource type="CylinderMesh" id="{mid}"]\ntop_radius = {_num(size[0])}\nbottom_radius = {_num(size[1])}\nheight = {_num(size[2])}\nradial_segments = 10')
        elif kind == "sphere":
            res.append(f'[sub_resource type="SphereMesh" id="{mid}"]\nradius = {_num(size[0])}\nheight = {_num(size[1])}\nradial_segments = 10\nrings = 6')
        mat = material(colour, glow)
        nodes.append(f'[node name="Mesh{i}" type="MeshInstance3D" parent="."]\ntransform = {_xform(pos, yaw)}\nmesh = SubResource("{mid}")\n'
                     f'surface_material_override/0 = SubResource("{mat}")')
        n += 1
    if spec["collision"]:
        nodes.append('[node name="Collision" type="StaticBody3D" parent="."]')
        for i, c in enumerate(spec["collision"]):
            sid = f"shape{i}"
            if c[0] == "box":
                _, sx, sz, x, z, yaw = c
                res.append(f'[sub_resource type="BoxShape3D" id="{sid}"]\nsize = {_v((sx, 4, sz))}')
                nodes.append(f'[node name="Shape{i}" type="CollisionShape3D" parent="Collision"]\ntransform = {_xform((x, 2, z), yaw)}\nshape = SubResource("{sid}")')
            else:
                _, r, x, z = c
                res.append(f'[sub_resource type="CylinderShape3D" id="{sid}"]\nradius = {_num(r)}\nheight = 4')
                nodes.append(f'[node name="Shape{i}" type="CollisionShape3D" parent="Collision"]\ntransform = {_xform((x, 2, z))}\nshape = SubResource("{sid}")')
    if "light" in spec:
        x, y, z, colour, energy, rng = spec["light"]
        r, g, b = C[colour]
        nodes.append(f'[node name="Light" type="OmniLight3D" parent="."]\ntransform = {_xform((x, y, z))}\nlight_color = Color({r}, {g}, {b}, 1)\n'
                     f"light_energy = {energy}\nomni_range = {rng}")
    head = f"[gd_scene load_steps={len(res) + len(exts) + 1} format=3]"
    return "\n\n".join([head] + exts + res + nodes) + "\n"


def outputs() -> list[tuple[str, str]]:
    """(file name, scene text) for every prop and every biome variant of one."""
    for key in SKINS:
        if key.split("__")[0] not in PROPS:
            raise ValueError(f"world_skins: {key} names an unknown prop")
    out = [(f"{prop}.tscn", scene(prop, prop if prop in SKINS else None)) for prop in sorted(PROPS)]
    out += [(f"{key}.tscn", scene(key.split("__")[0], key)) for key in sorted(k for k in SKINS if "__" in k)]
    return out


def main() -> int:
    check = "--check" in sys.argv
    OUT.mkdir(parents=True, exist_ok=True)
    stale = []
    written = outputs()
    for fname, text in written:
        path = OUT / fname
        if check:
            if not path.exists() or path.read_text(encoding="utf-8") != text:
                stale.append(path.name)
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
    if check and stale:
        print("stale prop scenes (run python3 tools/world_props.py):", ", ".join(stale))
        return 1
    if not check:
        keep = {f for f, _ in written}
        for old in OUT.glob("*__*.tscn"):  # biome variants dropped from world_skins.py
            if old.name not in keep:
                old.unlink()
        print(f"wrote {len(written)} prop scenes ({len(PROPS)} props and their biome variants) to {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
