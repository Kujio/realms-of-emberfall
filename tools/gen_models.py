#!/usr/bin/env python3
"""Generate the Emberfall Basic starter models: original, rigged, animated low-poly GLB files.

These are our own CC0 placeholder assets (no third-party art). They exist so the client's model
pipeline (glTF loading, animation maps, attachment bones, swappable parts, material tints) runs on
real skinned glTF data until real asset packs are dropped in (see docs/MODELS.md and ASSETS.md).

Output (committed): client/assets/models/emberfall_basic/
  humanoid_female.glb, humanoid_male.glb  skinned humanoid, 10 clips, swappable parts, 8 materials
  quadruped.glb                           skinned four-legged beast, 6 clips
  sword.glb shield.glb mace.glb staff.glb dagger.glb bow.glb   static held items (grip at the origin)

Conventions (same as most glTF packs): Y up, the model faces +Z, 1 unit = 1 metre, feet at y = 0.
The client turns +Z-facing models to its own -Z forward with the model's yaw_offset_deg (180).

Usage: python tools/gen_models.py [--check]   (--check fails if the committed files are stale)
Standard library only. Output is deterministic (byte-identical for the same script).
"""
from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "client" / "assets" / "models" / "emberfall_basic"

# ----------------------------------------------------------------------------- math helpers


def quat_axis(axis: tuple[float, float, float], deg: float) -> tuple[float, float, float, float]:
    h = math.radians(deg) / 2.0
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def euler(rx: float = 0.0, ry: float = 0.0, rz: float = 0.0):
    """Degrees, applied X then Y then Z (q = qz * qy * qx)."""
    q = quat_axis((1, 0, 0), rx)
    q = quat_mul(quat_axis((0, 1, 0), ry), q)
    q = quat_mul(quat_axis((0, 0, 1), rz), q)
    return tuple(round(v, 6) for v in q)


# ----------------------------------------------------------------------------- glTF writer


class Gltf:
    """Just enough glTF 2.0 for skinned, animated, flat-shaded box meshes."""

    def __init__(self) -> None:
        self.bin = bytearray()
        self.doc: dict = {"asset": {"version": "2.0", "generator": "Emberfall tools/gen_models.py"},
                          "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [],
                          "accessors": [], "bufferViews": [], "buffers": []}
        self.material_index: dict[str, int] = {}

    def _view(self, data: bytes, target: int | None = None) -> int:
        while len(self.bin) % 4:
            self.bin.append(0)
        view = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.bin.extend(data)
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values: list, ctype: int, atype: str, target: int | None = None, minmax: bool = False) -> int:
        fmt = {5126: "f", 5123: "H", 5121: "B"}[ctype]
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}[atype]
        flat: list = []
        for v in values:
            flat.extend(v if isinstance(v, (list, tuple)) else [v])
        data = struct.pack("<" + fmt * len(flat), *flat)
        acc = {"bufferView": self._view(data, target), "componentType": ctype, "count": len(values), "type": atype}
        if minmax:
            cols = [[(v if isinstance(v, (list, tuple)) else [v])[i] for v in values] for i in range(width)]
            acc["min"] = [min(c) for c in cols]
            acc["max"] = [max(c) for c in cols]
        self.doc["accessors"].append(acc)
        return len(self.doc["accessors"]) - 1

    def material(self, name: str, rgb: tuple[float, float, float], metallic: float = 0.0, rough: float = 0.85) -> int:
        if name not in self.material_index:
            self.doc["materials"].append({"name": name, "pbrMetallicRoughness": {
                "baseColorFactor": [rgb[0], rgb[1], rgb[2], 1.0], "metallicFactor": metallic, "roughnessFactor": rough}})
            self.material_index[name] = len(self.doc["materials"]) - 1
        return self.material_index[name]

    def node(self, node: dict, root: bool = False) -> int:
        self.doc["nodes"].append(node)
        idx = len(self.doc["nodes"]) - 1
        if root:
            self.doc["scenes"][0]["nodes"].append(idx)
        return idx

    def mesh(self, name: str, boxes: list["Box"], joint_index: dict[str, int] | None) -> int:
        """One mesh, one primitive per material. Each box is rigidly bound to one joint."""
        by_mat: dict[str, list[Box]] = {}
        for b in boxes:
            by_mat.setdefault(b.material, []).append(b)
        prims = []
        for mat, group in by_mat.items():
            pos, nrm, jnt, wgt, idx = [], [], [], [], []
            for b in group:
                base = len(pos)
                p, n, i = b.geometry()
                pos += p
                nrm += n
                idx += [base + k for k in i]
                if joint_index is not None:
                    j = joint_index[b.joint]
                    jnt += [(j, 0, 0, 0)] * len(p)
                    wgt += [(1.0, 0.0, 0.0, 0.0)] * len(p)
            attrs = {"POSITION": self.accessor(pos, 5126, "VEC3", 34962, minmax=True),
                     "NORMAL": self.accessor(nrm, 5126, "VEC3", 34962)}
            if joint_index is not None:
                attrs["JOINTS_0"] = self.accessor(jnt, 5123, "VEC4", 34962)
                attrs["WEIGHTS_0"] = self.accessor(wgt, 5126, "VEC4", 34962)
            prims.append({"attributes": attrs, "indices": self.accessor(idx, 5123, "SCALAR", 34963),
                          "material": self.material_index[mat]})
        self.doc["meshes"].append({"name": name, "primitives": prims})
        return len(self.doc["meshes"]) - 1

    def glb(self) -> bytes:
        while len(self.bin) % 4:
            self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]
        js = json.dumps(self.doc, separators=(",", ":"), sort_keys=True).encode()
        js += b" " * ((4 - len(js) % 4) % 4)
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        return (struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(js), 0x4E4F534A) + js
                + struct.pack("<II", len(self.bin), 0x004E4942) + bytes(self.bin))


class Box:
    """An axis-aligned box in rest-pose world space, optionally tapered, bound to one joint."""

    def __init__(self, joint: str, center, size, material: str, taper: float = 1.0) -> None:
        self.joint, self.center, self.size, self.material, self.taper = joint, center, size, material, taper

    def geometry(self):
        cx, cy, cz = self.center
        hx, hy, hz = (s / 2.0 for s in self.size)
        t = self.taper  # scales the top (+Y) face in X and Z

        def corner(sx, sy, sz):
            k = t if sy > 0 else 1.0
            return (round(cx + sx * hx * k, 5), round(cy + sy * hy, 5), round(cz + sz * hz * k, 5))

        faces = [((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
                 ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
                 ((0, 1, 0), [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
                 ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
                 ((0, 0, 1), [(1, -1, 1), (1, 1, 1), (-1, 1, 1), (-1, -1, 1)]),
                 ((0, 0, -1), [(-1, -1, -1), (-1, 1, -1), (1, 1, -1), (1, -1, -1)])]
        pos, nrm, idx = [], [], []
        for n, quad in faces:
            base = len(pos)
            pts = [corner(*q) for q in quad]
            # Use the true face normal (tapered sides lean in).
            ax, ay, az = (pts[1][i] - pts[0][i] for i in range(3))
            bx, by, bz = (pts[3][i] - pts[0][i] for i in range(3))
            fx, fy, fz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
            ln = math.sqrt(fx * fx + fy * fy + fz * fz) or 1.0
            fn = (fx / ln, fy / ln, fz / ln)
            if fn[0] * n[0] + fn[1] * n[1] + fn[2] * n[2] < 0:
                fn = (-fn[0], -fn[1], -fn[2])
            pos += pts
            nrm += [tuple(round(v, 5) for v in fn)] * 4
            # Counter-clockwise seen from outside.
            idx += [base, base + 2, base + 1, base, base + 3, base + 2] if _cw(pts, fn) else \
                   [base, base + 1, base + 2, base, base + 2, base + 3]
        return pos, nrm, idx


def _cw(pts, n) -> bool:
    ax, ay, az = (pts[1][i] - pts[0][i] for i in range(3))
    bx, by, bz = (pts[2][i] - pts[0][i] for i in range(3))
    cx, cy, cz = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
    return cx * n[0] + cy * n[1] + cz * n[2] < 0


# ----------------------------------------------------------------------------- rigged models


class Rig:
    def __init__(self, joints: list[tuple[str, str | None, tuple[float, float, float]]]) -> None:
        self.joints = joints  # (name, parent, rest head position in model space)
        self.head = {n: p for n, _, p in joints}
        self.parent = {n: par for n, par, _ in joints}

    def local(self, name: str) -> tuple[float, float, float]:
        par = self.parent[name]
        h = self.head[name]
        if par is None:
            return h
        ph = self.head[par]
        return tuple(round(h[i] - ph[i], 5) for i in range(3))


def build_rigged(rig: Rig, parts: dict[str, list[Box]], materials: dict[str, tuple], clips: dict, fps: int = 12) -> bytes:
    g = Gltf()
    for name, spec in materials.items():
        g.material(name, *spec)
    arm = g.node({"name": "Armature", "children": []}, root=True)
    jnode: dict[str, int] = {}
    for name, parent, _ in rig.joints:
        jnode[name] = g.node({"name": name, "translation": list(rig.local(name))})
    for name, parent, _ in rig.joints:
        if parent is None:
            g.doc["nodes"][arm]["children"].append(jnode[name])
        else:
            g.doc["nodes"][jnode[parent]].setdefault("children", []).append(jnode[name])
    joint_order = [n for n, _, _ in rig.joints]
    joint_index = {n: i for i, n in enumerate(joint_order)}
    ibm = []
    for n in joint_order:
        x, y, z = rig.head[n]
        ibm.append([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1])  # column-major inverse translation
    g.doc["skins"] = [{"name": "Skin", "joints": [jnode[n] for n in joint_order],
                       "inverseBindMatrices": g.accessor(ibm, 5126, "MAT4"), "skeleton": jnode[joint_order[0]]}]
    for part, boxes in parts.items():
        m = g.mesh(part, boxes, joint_index)
        g.doc["nodes"][arm]["children"].append(g.node({"name": part, "mesh": m, "skin": 0}))

    # Joints any clip moves get a translation channel in every clip, so blending from (say) a lunge
    # into a death never leaves a stale offset behind.
    moved = {n for n in joint_order if rig.parent[n] is None or n == "hips"}
    for duration, pose_fn in clips.values():
        for i in range(9):
            moved |= {n for n, v in pose_fn(i / 8).items() if "t" in v}
    anims = []
    for clip, (duration, pose_fn) in clips.items():
        frames = max(2, int(round(duration * fps)) + 1)
        times = [round(duration * i / (frames - 1), 5) for i in range(frames)]
        poses = [pose_fn(i / (frames - 1)) for i in range(frames)]
        t_acc = g.accessor(times, 5126, "SCALAR", minmax=True)
        samplers, channels = [], []
        for n in joint_order:
            rots = [p.get(n, {}).get("r", (0.0, 0.0, 0.0)) for p in poses]
            samplers.append({"input": t_acc, "output": g.accessor([euler(*r) for r in rots], 5126, "VEC4"),
                             "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": jnode[n], "path": "rotation"}})
            if n in moved:
                base = rig.local(n)
                tr = []
                for p in poses:
                    off = p.get(n, {}).get("t", (0.0, 0.0, 0.0))
                    tr.append(tuple(round(base[i] + off[i], 5) for i in range(3)))
                samplers.append({"input": t_acc, "output": g.accessor(tr, 5126, "VEC3"), "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": jnode[n], "path": "translation"}})
        anims.append({"name": clip, "samplers": samplers, "channels": channels})
    g.doc["animations"] = anims
    return g.glb()


def build_static(name: str, boxes: list[Box], materials: dict[str, tuple]) -> bytes:
    g = Gltf()
    for mname, spec in materials.items():
        g.material(mname, *spec)
    m = g.mesh(name, boxes, None)
    g.node({"name": name, "mesh": m}, root=True)
    for k in ("skins", "animations"):
        g.doc.pop(k, None)
    return g.glb()


# ----------------------------------------------------------------------------- humanoid

HUMAN_MATERIALS = {
    # name: (rgb, metallic, roughness). Neutral colours: appearance.json tints them per race and class.
    "Skin": ((0.85, 0.66, 0.52), 0.0, 0.8),
    "Hair": ((0.35, 0.22, 0.12), 0.0, 0.9),
    "Cloth": ((0.62, 0.6, 0.55), 0.0, 0.9),
    "Pants": ((0.36, 0.33, 0.3), 0.0, 0.9),
    "Trim": ((0.75, 0.62, 0.3), 0.3, 0.6),
    "Leather": ((0.33, 0.22, 0.14), 0.0, 0.85),
    "Metal": ((0.62, 0.63, 0.66), 0.8, 0.35),
    "Eyes": ((0.06, 0.05, 0.05), 0.0, 0.5),
}


def humanoid_rig(f: dict) -> Rig:
    s = f["shoulder"]
    hp = f["hip"]
    k = f["height"]

    def y(v: float) -> float:
        return round(v * k, 4)
    return Rig([
        ("root", None, (0.0, 0.0, 0.0)),
        ("hips", "root", (0.0, y(0.95), 0.0)),
        ("spine", "hips", (0.0, y(1.08), 0.0)),
        ("chest", "spine", (0.0, y(1.26), 0.0)),
        ("neck", "chest", (0.0, y(1.5), 0.0)),
        ("head", "neck", (0.0, y(1.58), 0.0)),
        ("upper_arm_L", "chest", (s, y(1.45), 0.0)),
        ("forearm_L", "upper_arm_L", (s, y(1.17), 0.0)),
        ("hand_L", "forearm_L", (s, y(0.93), 0.0)),
        ("upper_arm_R", "chest", (-s, y(1.45), 0.0)),
        ("forearm_R", "upper_arm_R", (-s, y(1.17), 0.0)),
        ("hand_R", "forearm_R", (-s, y(0.93), 0.0)),
        ("thigh_L", "hips", (hp, y(0.92), 0.0)),
        ("shin_L", "thigh_L", (hp, y(0.5), 0.0)),
        ("foot_L", "shin_L", (hp, y(0.08), 0.0)),
        ("thigh_R", "hips", (-hp, y(0.92), 0.0)),
        ("shin_R", "thigh_R", (-hp, y(0.5), 0.0)),
        ("foot_R", "shin_R", (-hp, y(0.08), 0.0)),
    ])


def humanoid_parts(f: dict) -> dict[str, list[Box]]:
    s, hp, k = f["shoulder"], f["hip"], f["height"]

    def Y(v: float) -> float:
        return round(v * k, 4)
    cw = f["chest_w"]
    body = [
        Box("hips", (0, Y(0.97), 0), (hp * 2 + 0.16, Y(0.18), 0.21), "Pants"),
        Box("hips", (0, Y(1.04), 0), (hp * 2 + 0.17, 0.05, 0.22), "Leather"),  # belt
        Box("hips", (0, Y(1.04), 0.112), (0.07, 0.05, 0.02), "Trim"),        # buckle
        Box("spine", (0, Y(1.17), 0), (cw - 0.08, Y(0.18), 0.19), "Cloth"),
        Box("chest", (0, Y(1.38), 0), (cw, Y(0.26), 0.22), "Cloth", taper=1.06),
        Box("chest", (0, Y(1.49), 0), (0.16, 0.03, 0.2), "Trim"),            # collar
        Box("neck", (0, Y(1.54), 0), (0.09, Y(0.08), 0.09), "Skin"),
        Box("head", (0, Y(1.7), 0), (0.21, Y(0.24), 0.22), "Skin"),
        Box("head", (0, Y(1.69), 0.118), (0.04, 0.05, 0.03), "Skin"),        # nose
        Box("head", (0.05, Y(1.73), 0.111), (0.035, 0.025, 0.01), "Eyes"),
        Box("head", (-0.05, Y(1.73), 0.111), (0.035, 0.025, 0.01), "Eyes"),
        Box("head", (0, Y(1.63), 0.111), (0.07, 0.015, 0.01), "Eyes"),       # mouth
    ]
    for side, x in (("L", s), ("R", -s)):
        body += [
            Box(f"upper_arm_{side}", (x, Y(1.32), 0), (0.1, Y(0.28), 0.1), "Cloth"),
            Box(f"forearm_{side}", (x, Y(1.06), 0), (0.085, Y(0.24), 0.085), "Skin"),
            Box(f"forearm_{side}", (x, Y(0.97), 0), (0.095, 0.05, 0.095), "Leather"),  # cuff
            Box(f"hand_{side}", (x, Y(0.87), 0.005), (0.075, Y(0.11), 0.06), "Skin"),
        ]
    for side, x in (("L", hp), ("R", -hp)):
        body += [
            Box(f"thigh_{side}", (x, Y(0.71), 0), (0.14, Y(0.42), 0.14), "Pants"),
            Box(f"shin_{side}", (x, Y(0.31), 0), (0.12, Y(0.4), 0.12), "Pants"),
            Box(f"shin_{side}", (x, Y(0.2), 0), (0.13, Y(0.18), 0.13), "Leather"),     # boot top
            Box(f"foot_{side}", (x, 0.045, 0.04), (0.12, 0.09, 0.23), "Leather"),
        ]
    hy = Y(1.7)  # head centre
    parts = {
        "Body": body,
        # Swappable parts. Group membership (which of these are alternatives) is declared in models.json.
        "HairShort": [Box("head", (0, hy + 0.115, -0.01), (0.23, 0.05, 0.24), "Hair"),
                      Box("head", (0, hy + 0.02, -0.105), (0.23, 0.2, 0.04), "Hair")],
        "HairLong": [Box("head", (0, hy + 0.115, -0.01), (0.23, 0.05, 0.24), "Hair"),
                     Box("head", (0, hy - 0.06, -0.11), (0.235, 0.36, 0.05), "Hair"),
                     Box("head", (0.112, hy + 0.0, 0.0), (0.02, 0.18, 0.2), "Hair"),
                     Box("head", (-0.112, hy + 0.0, 0.0), (0.02, 0.18, 0.2), "Hair")],
        "HairTopknot": [Box("head", (0, hy + 0.115, -0.01), (0.22, 0.04, 0.23), "Hair"),
                        Box("head", (0, hy + 0.17, -0.04), (0.08, 0.08, 0.08), "Hair")],
        "Beard": [Box("head", (0, hy - 0.1, 0.09), (0.19, 0.11, 0.06), "Hair"),
                  Box("head", (0, hy - 0.17, 0.1), (0.12, 0.06, 0.05), "Hair")],
        "EarsPointed": [Box("head", (0.12, hy + 0.04, -0.01), (0.03, 0.05, 0.05), "Skin", taper=0.4),
                        Box("head", (-0.12, hy + 0.04, -0.01), (0.03, 0.05, 0.05), "Skin", taper=0.4),
                        Box("head", (0.13, hy + 0.09, -0.02), (0.02, 0.06, 0.03), "Skin", taper=0.3),
                        Box("head", (-0.13, hy + 0.09, -0.02), (0.02, 0.06, 0.03), "Skin", taper=0.3)],
        "Helm": [Box("head", (0, hy + 0.04, -0.005), (0.25, 0.22, 0.25), "Metal", taper=0.92),
                 Box("head", (0, hy - 0.01, 0.125), (0.03, 0.12, 0.02), "Metal"),
                 Box("head", (0, hy + 0.16, 0), (0.04, 0.03, 0.2), "Trim")],
        "Hood": [Box("head", (0, hy + 0.03, -0.015), (0.26, 0.29, 0.26), "Cloth", taper=0.85),
                 Box("chest", (0, Y(1.47), -0.06), (0.3, 0.08, 0.16), "Cloth")],
        "WizardHat": [Box("head", (0, hy + 0.13, 0), (0.36, 0.02, 0.36), "Cloth"),
                      Box("head", (0, hy + 0.2, -0.01), (0.2, 0.13, 0.2), "Cloth", taper=0.6),
                      Box("head", (0, hy + 0.31, -0.03), (0.12, 0.11, 0.12), "Cloth", taper=0.35),
                      Box("head", (0, hy + 0.145, 0), (0.205, 0.025, 0.205), "Trim")],
        "Circlet": [Box("head", (0, hy + 0.07, 0), (0.225, 0.025, 0.225), "Trim")],
        "Robe": [Box("hips", (0, Y(0.66), 0), (hp * 2 + 0.24, Y(0.66), 0.3), "Cloth", taper=0.7),
                 Box("hips", (0, Y(0.34), 0), (hp * 2 + 0.26, 0.03, 0.31), "Trim")],
        "Cape": [Box("chest", (0, Y(1.0), -0.13), (cw - 0.02, Y(0.92), 0.025), "Cloth", taper=0.75),
                 Box("chest", (0, Y(1.46), -0.12), (cw - 0.04, 0.05, 0.04), "Trim")],
        "Pauldrons": [Box("upper_arm_L", (s + 0.02, Y(1.47), 0), (0.15, 0.08, 0.16), "Metal"),
                      Box("upper_arm_R", (-s - 0.02, Y(1.47), 0), (0.15, 0.08, 0.16), "Metal")],
        "Tabard": [Box("chest", (0, Y(1.2), 0.115), (cw * 0.55, Y(0.5), 0.015), "Trim")],
    }
    return parts


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def smooth(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return t * t * (3 - 2 * t)


def keys(t: float, points: list[tuple[float, float]]) -> float:
    """Piecewise smooth interpolation through (time, value) points."""
    if t <= points[0][0]:
        return points[0][1]
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        if t <= t1:
            return lerp(v0, v1, smooth((t - t0) / max(1e-6, t1 - t0)))
    return points[-1][1]


def humanoid_clips(k: float) -> dict:
    """Clip name -> (seconds, pose(t in 0..1) -> {joint: {"r": (x,y,z) deg, "t": offset}})."""
    tau = 2.0 * math.pi
    # In this rig the model faces +Z and +X is its left. A negative X rotation swings a limb forward.

    def idle(t):
        b = math.sin(t * tau)
        return {"chest": {"r": (b * 1.5, 0, 0)}, "head": {"r": (-b * 1.0, b * 2.0, 0)},
                "upper_arm_L": {"r": (b * 2, 0, 4)}, "upper_arm_R": {"r": (-b * 2, 0, -4)},
                "forearm_L": {"r": (-6, 0, 0)}, "forearm_R": {"r": (-6, 0, 0)},
                "hips": {"t": (0, b * 0.006, 0)}}

    def combat_idle(t):
        b = math.sin(t * tau)
        return {"hips": {"r": (0, 12, 0), "t": (0, -0.04 + b * 0.01, 0)}, "chest": {"r": (6, -10, 0)},
                "head": {"r": (-4, -4, 0)},
                "upper_arm_R": {"r": (-35 + b * 3, 0, -12)}, "forearm_R": {"r": (-55, 0, 0)},
                "upper_arm_L": {"r": (-25 - b * 3, 0, 14)}, "forearm_L": {"r": (-60, 0, 0)},
                "thigh_L": {"r": (-14, 0, 4)}, "shin_L": {"r": (18, 0, 0)}, "foot_L": {"r": (-4, 0, 0)},
                "thigh_R": {"r": (10, 0, -4)}, "shin_R": {"r": (14, 0, 0)}, "foot_R": {"r": (-10, 0, 0)}}

    def gait(swing: float, knee: float, arm: float, elbow: float, lean: float, bob: float):
        def pose(t):
            p = math.sin(t * tau)
            q = math.sin(t * tau + math.pi)
            # A leg lifts while it swings forward (recovery) and stays planted while it moves back.
            lift_l = max(0.0, math.cos(t * tau))
            lift_r = max(0.0, -math.cos(t * tau))
            return {
                "hips": {"r": (0, p * 4, 0), "t": (0, -abs(math.cos(t * tau)) * bob + bob * 0.5, 0)},
                "spine": {"r": (lean * 0.5, -p * 3, 0)}, "chest": {"r": (lean * 0.5, -p * 5, 0)},
                "head": {"r": (-lean * 0.8, p * 3, 0)},
                "thigh_L": {"r": (-p * swing, 0, 0)}, "shin_L": {"r": (lift_l * knee, 0, 0)},
                "foot_L": {"r": (-lift_l * knee * 0.3, 0, 0)},
                "thigh_R": {"r": (-q * swing, 0, 0)}, "shin_R": {"r": (lift_r * knee, 0, 0)},
                "foot_R": {"r": (-lift_r * knee * 0.3, 0, 0)},
                "upper_arm_L": {"r": (q * arm, 0, 5)}, "forearm_L": {"r": (-elbow - max(0, -q) * elbow * 0.5, 0, 0)},
                "upper_arm_R": {"r": (p * arm, 0, -5)}, "forearm_R": {"r": (-elbow - max(0, -p) * elbow * 0.5, 0, 0)},
            }
        return pose

    def attack1(t):  # overhand swing with the right hand
        a = keys(t, [(0, -20), (0.3, -150), (0.55, -40), (1.0, -20)])
        tw = keys(t, [(0, 0), (0.3, 20), (0.55, -25), (1.0, 0)])
        return {"chest": {"r": (keys(t, [(0, 0), (0.3, -6), (0.55, 10), (1, 0)]), tw, 0)},
                "spine": {"r": (0, tw * 0.4, 0)},
                "upper_arm_R": {"r": (a, 0, -10)}, "forearm_R": {"r": (keys(t, [(0, -20), (0.3, -60), (0.55, -5), (1, -20)]), 0, 0)},
                "upper_arm_L": {"r": (-20, 0, 15)}, "forearm_L": {"r": (-45, 0, 0)},
                "thigh_L": {"r": (-18, 0, 0)}, "shin_L": {"r": (14, 0, 0)},
                "thigh_R": {"r": (12, 0, 0)}, "shin_R": {"r": (10, 0, 0)},
                "hips": {"t": (0, -0.03, keys(t, [(0, 0), (0.55, 0.06), (1, 0)]))}}

    def attack2(t):  # forward thrust / jab
        a = keys(t, [(0, -30), (0.25, -40), (0.5, -95), (1.0, -30)])
        return {"chest": {"r": (keys(t, [(0, 0), (0.5, 12), (1, 0)]), keys(t, [(0, 0), (0.25, 25), (0.5, -20), (1, 0)]), 0)},
                "upper_arm_R": {"r": (a, 0, -8)}, "forearm_R": {"r": (keys(t, [(0, -60), (0.25, -90), (0.5, -5), (1, -60)]), 0, 0)},
                "upper_arm_L": {"r": (-30, 0, 20)}, "forearm_L": {"r": (-70, 0, 0)},
                "thigh_L": {"r": (keys(t, [(0, -10), (0.5, -30), (1, -10)]), 0, 0)}, "shin_L": {"r": (20, 0, 0)},
                "thigh_R": {"r": (15, 0, 0)}, "shin_R": {"r": (12, 0, 0)},
                "hips": {"t": (0, -0.04, keys(t, [(0, 0), (0.5, 0.1), (1, 0)]))}}

    def cast(t):  # both hands raised and weaving
        b = math.sin(t * tau)
        c = math.cos(t * tau)
        return {"chest": {"r": (-6, 0, 0)}, "head": {"r": (-8, 0, 0)},
                "upper_arm_L": {"r": (-105 + b * 8, 0, 25 + c * 6)}, "forearm_L": {"r": (-25 - c * 10, 0, 0)},
                "upper_arm_R": {"r": (-105 - b * 8, 0, -25 - c * 6)}, "forearm_R": {"r": (-25 + c * 10, 0, 0)},
                "hand_L": {"r": (0, b * 20, 0)}, "hand_R": {"r": (0, -b * 20, 0)},
                "hips": {"t": (0, b * 0.01, 0)}}

    def hit(t):
        k2 = keys(t, [(0, 0), (0.2, 1), (1, 0)])
        return {"chest": {"r": (-18 * k2, 8 * k2, 0)}, "spine": {"r": (-8 * k2, 0, 0)}, "head": {"r": (-14 * k2, 0, 0)},
                "upper_arm_L": {"r": (-20 * k2, 0, 25 * k2)}, "upper_arm_R": {"r": (-20 * k2, 0, -25 * k2)},
                "hips": {"t": (0, 0, -0.06 * k2)}}

    def death(t):  # buckle, then fall onto the back
        knee = keys(t, [(0, 0), (0.3, 1), (0.6, 0.4), (1, 0.1)])
        fall = keys(t, [(0, 0), (0.25, 0), (0.75, 1), (1, 1)])
        return {"root": {"r": (-90 * fall, 0, 0), "t": (0, 0.11 * fall, -0.15 * fall)},
                "hips": {"t": (0, -0.25 * knee, 0)},
                "thigh_L": {"r": (-45 * knee, 0, 6), }, "shin_L": {"r": (70 * knee, 0, 0)},
                "thigh_R": {"r": (-35 * knee, 0, -6)}, "shin_R": {"r": (60 * knee, 0, 0)},
                "chest": {"r": (20 * knee - 10 * fall, 0, 0)}, "head": {"r": (-20 * fall, 15 * fall, 0)},
                "upper_arm_L": {"r": (-30 * knee - 60 * fall, 0, 30 * fall)}, "forearm_L": {"r": (-20, 0, 0)},
                "upper_arm_R": {"r": (-30 * knee - 70 * fall, 0, -40 * fall)}, "forearm_R": {"r": (-30, 0, 0)}}

    def sit(t):  # on the ground, legs crossed forward, a slow breath
        b = math.sin(t * tau)
        drop = -0.78 * k
        return {"hips": {"t": (0, drop, 0)}, "spine": {"r": (b * 1.5 + 4, 0, 0)}, "chest": {"r": (6, 0, 0)},
                "head": {"r": (4, 0, 0)},
                "thigh_L": {"r": (-82, -28, 10)}, "shin_L": {"r": (125, 0, 0)}, "foot_L": {"r": (-30, 0, 0)},
                "thigh_R": {"r": (-82, 28, -10)}, "shin_R": {"r": (125, 0, 0)}, "foot_R": {"r": (-30, 0, 0)},
                "upper_arm_L": {"r": (-30, 0, 12)}, "forearm_L": {"r": (-50, 0, 0)},
                "upper_arm_R": {"r": (-30, 0, -12)}, "forearm_R": {"r": (-50, 0, 0)}}

    return {
        "Idle": (2.4, idle),
        "CombatIdle": (1.0, combat_idle),
        "Walk": (1.0, gait(swing=24, knee=40, arm=20, elbow=10, lean=3, bob=0.025)),
        "Run": (0.66, gait(swing=42, knee=85, arm=40, elbow=70, lean=12, bob=0.05)),
        "Attack1": (0.7, attack1),
        "Attack2": (0.6, attack2),
        "Cast": (1.2, cast),
        "Hit": (0.4, hit),
        "Death": (1.3, death),
        "Sit": (3.0, sit),
    }


HUMAN_SHAPES = {
    "humanoid_female": {"shoulder": 0.2, "hip": 0.1, "height": 0.96, "chest_w": 0.34},
    "humanoid_male": {"shoulder": 0.235, "hip": 0.1, "height": 1.0, "chest_w": 0.42},
}

# ----------------------------------------------------------------------------- quadruped

BEAST_MATERIALS = {
    "Fur": ((0.55, 0.5, 0.45), 0.0, 0.95),
    "FurDark": ((0.3, 0.26, 0.22), 0.0, 0.95),
    "Horn": ((0.88, 0.84, 0.74), 0.0, 0.6),
    "Eyes": ((0.95, 0.7, 0.15), 0.0, 0.3),
}

BEAST_RIG = Rig([
    ("root", None, (0.0, 0.0, 0.0)),
    ("body", "root", (0.0, 0.5, 0.0)),
    ("neck", "body", (0.0, 0.58, 0.36)),
    ("head", "neck", (0.0, 0.66, 0.5)),
    ("tail", "body", (0.0, 0.56, -0.4)),
    ("leg_FL", "body", (0.13, 0.42, 0.3)), ("paw_FL", "leg_FL", (0.13, 0.2, 0.3)),
    ("leg_FR", "body", (-0.13, 0.42, 0.3)), ("paw_FR", "leg_FR", (-0.13, 0.2, 0.3)),
    ("leg_BL", "body", (0.13, 0.42, -0.3)), ("paw_BL", "leg_BL", (0.13, 0.2, -0.3)),
    ("leg_BR", "body", (-0.13, 0.42, -0.3)), ("paw_BR", "leg_BR", (-0.13, 0.2, -0.3)),
])


def beast_parts() -> dict[str, list[Box]]:
    body = [
        Box("body", (0, 0.52, 0), (0.36, 0.3, 0.82), "Fur"),
        Box("body", (0, 0.66, 0.05), (0.3, 0.06, 0.6), "FurDark"),
        Box("neck", (0, 0.62, 0.44), (0.2, 0.2, 0.18), "Fur"),
        Box("head", (0, 0.68, 0.62), (0.24, 0.22, 0.28), "Fur"),
        Box("head", (0, 0.63, 0.81), (0.14, 0.12, 0.14), "FurDark"),
        Box("head", (0.075, 0.73, 0.765), (0.04, 0.03, 0.01), "Eyes"),
        Box("head", (-0.075, 0.73, 0.765), (0.04, 0.03, 0.01), "Eyes"),
        Box("head", (0.08, 0.82, 0.58), (0.06, 0.08, 0.04), "FurDark", taper=0.4),
        Box("head", (-0.08, 0.82, 0.58), (0.06, 0.08, 0.04), "FurDark", taper=0.4),
        Box("tail", (0, 0.56, -0.62), (0.07, 0.07, 0.42), "FurDark", taper=0.6),
    ]
    for leg in ("FL", "FR", "BL", "BR"):
        x = 0.13 if leg.endswith("L") else -0.13
        z = 0.3 if leg.startswith("F") else -0.3
        body += [Box(f"leg_{leg}", (x, 0.32, z), (0.1, 0.24, 0.11), "Fur"),
                 Box(f"paw_{leg}", (x, 0.1, z + 0.01), (0.085, 0.2, 0.09), "FurDark")]
    return {"Body": body,
            "Tusks": [Box("head", (0.07, 0.6, 0.86), (0.025, 0.1, 0.025), "Horn", taper=0.4),
                      Box("head", (-0.07, 0.6, 0.86), (0.025, 0.1, 0.025), "Horn", taper=0.4)],
            "Horns": [Box("head", (0.07, 0.86, 0.6), (0.035, 0.16, 0.035), "Horn", taper=0.3),
                      Box("head", (-0.07, 0.86, 0.6), (0.035, 0.16, 0.035), "Horn", taper=0.3)],
            "Spines": [Box("body", (0, 0.72, z), (0.03, 0.12, 0.05), "Horn", taper=0.2) for z in (-0.25, -0.1, 0.05, 0.2)]}


def beast_clips() -> dict:
    tau = 2.0 * math.pi

    def idle(t):
        b = math.sin(t * tau)
        return {"body": {"t": (0, b * 0.006, 0)}, "head": {"r": (b * 3, b * 6, 0)}, "tail": {"r": (0, b * 15, 0)}}

    def gait(swing: float, bob: float, gallop: bool):
        def pose(t):
            p = math.sin(t * tau)
            q = math.sin(t * tau + math.pi)
            out = {"body": {"r": (p * (5 if gallop else 1.5), 0, 0), "t": (0, abs(p) * bob, 0)},
                   "head": {"r": (-p * 4, 0, 0)}, "tail": {"r": (10 + p * 10, q * 10, 0)}}
            # (phase offset, amplitude) per leg. Walk: diagonal pairs. Gallop: front pair, then back pair.
            legs = ({"FL": (0.0, 1.0), "BR": (0.0, 1.0), "FR": (math.pi, 1.0), "BL": (math.pi, 1.0)} if not gallop else
                    {"FL": (0.0, 1.0), "FR": (0.0, 0.9), "BR": (math.pi * 0.6, 1.0), "BL": (math.pi * 0.6, 0.9)})
            for leg, (offset, amp) in legs.items():
                angle = t * tau + offset
                out[f"leg_{leg}"] = {"r": (-math.sin(angle) * amp * swing, 0, 0)}
                # The leg swings forward while cos(angle) > 0: flex the paw then, keep it planted otherwise.
                out[f"paw_{leg}"] = {"r": (max(0.0, math.cos(angle)) * amp * swing * 0.9, 0, 0)}
            return out
        return pose

    def attack(t):
        k = keys(t, [(0, 0), (0.35, -1), (0.55, 1), (1, 0)])
        return {"body": {"r": (-k * 8, 0, 0), "t": (0, 0, k * 0.12)}, "neck": {"r": (k * 25, 0, 0)},
                "head": {"r": (-k * 20, 0, 0)}, "leg_FL": {"r": (-k * 25, 0, 0)}, "leg_FR": {"r": (-k * 25, 0, 0)}}

    def hit(t):
        k = keys(t, [(0, 0), (0.2, 1), (1, 0)])
        return {"body": {"r": (k * 6, 0, k * 6), "t": (0, 0, -0.08 * k)}, "head": {"r": (-k * 20, k * 15, 0)}}

    def death(t):
        k = keys(t, [(0, 0), (0.6, 1), (1, 1)])
        return {"root": {"r": (0, 0, 88 * k), "t": (-0.05 * k, 0.17 * k, 0)},
                "leg_FL": {"r": (-20 * k, 0, 0)}, "leg_BL": {"r": (20 * k, 0, 0)},
                "head": {"r": (15 * k, 0, 0)}, "tail": {"r": (-10 * k, 0, 0)}}

    return {"Idle": (2.0, idle), "Walk": (1.0, gait(22, 0.01, False)), "Run": (0.5, gait(38, 0.05, True)),
            "Attack": (0.7, attack), "Hit": (0.4, hit), "Death": (1.0, death)}


# ----------------------------------------------------------------------------- held items
# Grip at the origin. Blades point along +Z (forward from a hanging hand), shields face +X (outward
# from the left forearm), staffs and bows run along Y. models.json attachment offsets can adjust.

ITEM_MATERIALS = {
    "Metal": ((0.72, 0.73, 0.76), 0.85, 0.3),
    "Wood": ((0.45, 0.3, 0.17), 0.0, 0.85),
    "Leather": ((0.3, 0.2, 0.12), 0.0, 0.85),
    "Trim": ((0.8, 0.65, 0.28), 0.5, 0.45),
    "Gem": ((0.3, 0.55, 0.95), 0.1, 0.2),
}

ITEMS: dict[str, list[Box]] = {
    "sword": [Box("", (0, 0, 0), (0.035, 0.035, 0.14), "Leather"), Box("", (0, 0, 0.08), (0.2, 0.03, 0.03), "Trim"),
              Box("", (0, 0, 0.4), (0.05, 0.012, 0.6), "Metal", ), Box("", (0, 0, -0.085), (0.05, 0.05, 0.04), "Trim")],
    "dagger": [Box("", (0, 0, 0), (0.03, 0.03, 0.1), "Leather"), Box("", (0, 0, 0.06), (0.12, 0.025, 0.025), "Trim"),
               Box("", (0, 0, 0.2), (0.04, 0.01, 0.26), "Metal")],
    "mace": [Box("", (0, 0, 0.1), (0.035, 0.035, 0.5), "Wood"), Box("", (0, 0, 0.38), (0.12, 0.12, 0.14), "Metal"),
             Box("", (0, 0, 0.38), (0.16, 0.04, 0.04), "Metal"), Box("", (0, 0, 0.38), (0.04, 0.16, 0.04), "Metal")],
    "staff": [Box("", (0, 0.05, 0), (0.04, 1.7, 0.04), "Wood"), Box("", (0, 0.92, 0), (0.09, 0.1, 0.09), "Trim", taper=0.6),
              Box("", (0, 1.0, 0), (0.07, 0.07, 0.07), "Gem")],
    "bow": [Box("", (0, 0, 0), (0.035, 0.16, 0.04), "Leather"), Box("", (0, 0.36, 0.03), (0.03, 0.6, 0.03), "Wood"),
            Box("", (0, -0.36, 0.03), (0.03, 0.6, 0.03), "Wood"), Box("", (0, 0, -0.07), (0.006, 1.3, 0.006), "Trim"),
            # Limb tips curve back to meet the string.
            Box("", (0, 0.65, -0.015), (0.025, 0.03, 0.13), "Wood"), Box("", (0, -0.65, -0.015), (0.025, 0.03, 0.13), "Wood")],
    "shield": [Box("", (0.03, 0, 0), (0.04, 0.5, 0.42), "Wood"), Box("", (0.055, 0, 0), (0.015, 0.54, 0.46), "Metal"),
               Box("", (0.07, 0, 0), (0.03, 0.12, 0.12), "Trim")],
}


def generate() -> dict[str, bytes]:
    out: dict[str, bytes] = {}
    for name, shape in HUMAN_SHAPES.items():
        out[f"{name}.glb"] = build_rigged(humanoid_rig(shape), humanoid_parts(shape), HUMAN_MATERIALS,
                                          humanoid_clips(shape["height"]))
    out["quadruped.glb"] = build_rigged(BEAST_RIG, beast_parts(), BEAST_MATERIALS, beast_clips())
    for name, boxes in ITEMS.items():
        used = {b.material for b in boxes}
        out[f"{name}.glb"] = build_static(name.capitalize(), boxes, {k: v for k, v in ITEM_MATERIALS.items() if k in used})
    return out


def main() -> int:
    check = "--check" in sys.argv[1:]
    files = generate()
    stale = []
    OUT.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        path = OUT / name
        if check:
            if not path.exists() or path.read_bytes() != data:
                stale.append(name)
        else:
            path.write_bytes(data)
            print(f"wrote {path.relative_to(ROOT)} ({len(data)} bytes)")
    if stale:
        print("stale generated models (run python tools/gen_models.py): " + ", ".join(stale), file=sys.stderr)
        return 1
    if check:
        print(f"models up to date ({len(files)} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
