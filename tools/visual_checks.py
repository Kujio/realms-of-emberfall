"""Checks for content/models.json and content/appearance.json (used by tools/validate_content.py).

Besides structure and id references, this opens every referenced glTF/GLB and checks that the
animation clips, bones, swappable part nodes and tinted materials named in the JSON really exist
in the file, so a typo in an animation map or attachment bone fails CI instead of silently
degrading in the client.
"""
from __future__ import annotations

import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLIENT = ROOT / "client"
ID_RE = re.compile(r"^[a-z0-9_]{1,32}$")
COLOR_RE = re.compile(r"^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$")
SLOTS = ("main_hand", "off_hand", "back", "head")
GENDERS = ("female", "male")
ARMOR_TYPES = ("cloth", "leather", "chain", "plate")
# Standard animation states the client asks for (client/scripts/world/anim_state.gd).
STATES = ("idle", "combat_idle", "walk", "walk_back", "run", "attack1", "attack2", "cast", "hit", "death", "sit",
          "swim", "fall")
SHAPES = ("upright", "low")


class GltfInfo:
    """Names inside a glTF/GLB file: nodes (bones and mesh parts), animations and materials."""

    def __init__(self, nodes: set[str], joints: set[str], meshes: set[str], animations: set[str], materials: set[str]) -> None:
        self.nodes, self.joints, self.meshes, self.animations, self.materials = nodes, joints, meshes, animations, materials


LOOP_SUFFIXES = ("_loop", "-loop", "_cycle", "-cycle")


def godot_clip_name(name: str) -> str:
    """The name Godot's glTF importer gives a clip: it drops a _Loop/-Loop/_Cycle/-Cycle suffix."""
    low = name.lower()
    for suffix in LOOP_SUFFIXES:
        if low.endswith(suffix) and len(name) > len(suffix):
            return name[:-len(suffix)]
    return name


def _named(items, what: str) -> list[dict]:
    if not isinstance(items, list) or not all(isinstance(x, dict) for x in items):
        raise ValueError(f"'{what}' must be an array of objects")
    return items


def _check_external_buffers(path: Path, buffers) -> None:
    """Buffers stored in separate files (.bin next to a .gltf) must exist. data: URIs are inline."""
    if not isinstance(buffers, list):
        raise ValueError("'buffers' must be an array")
    for b in buffers:
        uri = b.get("uri") if isinstance(b, dict) else None
        if not isinstance(uri, str) or uri.startswith("data:"):
            continue
        target = (path.parent / uri).resolve()
        if ".." in Path(uri).parts or not target.is_file():
            raise ValueError(f"buffer file '{uri}' is missing")


def read_gltf(path: Path) -> GltfInfo:
    """Names in a glTF/GLB file. Raises ValueError (or OSError) for anything malformed."""
    data = path.read_bytes()
    try:
        if path.suffix.lower() == ".glb":
            if len(data) < 20:
                raise ValueError("file too short for a GLB header")
            magic, version, total = struct.unpack_from("<III", data, 0)
            if magic != 0x46546C67 or version != 2:
                raise ValueError("not a glTF 2.0 binary")
            if total != len(data):
                raise ValueError(f"header says {total} bytes but the file has {len(data)} (truncated?)")
            # Walk every chunk: each must fit inside the file; the first must be JSON.
            off, chunks = 12, []
            while off < len(data):
                if off + 8 > len(data):
                    raise ValueError("truncated chunk header")
                clen, ctype = struct.unpack_from("<II", data, off)
                if off + 8 + clen > len(data):
                    raise ValueError("a chunk runs past the end of the file")
                chunks.append((ctype, data[off + 8:off + 8 + clen]))
                off += 8 + clen
            if not chunks or chunks[0][0] != 0x4E4F534A:
                raise ValueError("first GLB chunk is not JSON")
            doc = json.loads(chunks[0][1].decode("utf-8"))
            buffers = doc.get("buffers", []) if isinstance(doc, dict) else []
            embedded = [b for b in buffers if isinstance(b, dict) and "uri" not in b] if isinstance(buffers, list) else []
            if embedded:
                bins = [c for t, c in chunks[1:] if t == 0x004E4942]
                if not bins:
                    raise ValueError("the binary (BIN) chunk is missing")
                want = embedded[0].get("byteLength")
                # The BIN chunk holds the first URI-less buffer, padded to 4 bytes.
                if not isinstance(want, int) or not 0 <= len(bins[0]) - want <= 3:
                    raise ValueError(f"the BIN chunk has {len(bins[0])} bytes but the buffer declares {want}")
            _check_external_buffers(path, buffers)
        else:
            doc = json.loads(data.decode("utf-8"))
            _check_external_buffers(path, doc.get("buffers", []) if isinstance(doc, dict) else [])
    except (struct.error, UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ValueError(str(e)) from e
    if not isinstance(doc, dict):
        raise ValueError("the glTF document must be a JSON object")
    nodes = _named(doc.get("nodes", []), "nodes")
    names = {str(n.get("name", "")) for n in nodes}
    joints: set[str] = set()
    for skin in _named(doc.get("skins", []), "skins"):
        for j in skin.get("joints", []) if isinstance(skin.get("joints", []), list) else []:
            if isinstance(j, int) and 0 <= j < len(nodes):
                joints.add(str(nodes[j].get("name", "")))
    meshes = {str(n.get("name", "")) for n in nodes if "mesh" in n}
    anims = {str(a.get("name", "")) for a in _named(doc.get("animations", []), "animations")}
    mats = {str(m.get("name", "")) for m in _named(doc.get("materials", []), "materials")}
    return GltfInfo(names, joints, meshes, anims, mats)


def res_path(scene: str) -> Path | None:
    """The file a res:// path names, or None if it is not a path inside the client project."""
    if not scene.startswith("res://"):
        return None
    rel = scene[len("res://"):]
    if rel == "" or rel.startswith(("/", "\\")) or ":" in rel or ".." in Path(rel).parts:
        return None
    full = (CLIENT / rel).resolve()
    try:
        full.relative_to(CLIENT.resolve())
    except ValueError:
        return None
    return full


def vec3(c, v, where: str, lo: float, hi: float) -> None:
    if not (isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v)):
        c.err(f"{where}: must be [x, y, z] numbers")
    elif not all(lo <= x <= hi for x in v):
        c.err(f"{where}: values must be {lo}..{hi}")


def scale_ok(c, v, where: str) -> None:
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        if not 0.3 <= v <= 3.0:
            c.err(f"{where}: scale must be 0.3..3")
    else:
        vec3(c, v, where, 0.3, 3.0)


def tints_ok(c, tints, where: str, info: GltfInfo | None) -> None:
    if not isinstance(tints, dict):
        c.err(f"{where}: tints must be an object of material -> colour")
        return
    for mat, col in tints.items():
        if not isinstance(col, str) or not COLOR_RE.match(col):
            c.err(f"{where}: tint '{mat}' must be a #rrggbb colour")
        if info is not None and mat != "*" and mat not in info.materials and mat not in info.meshes:
            c.err(f"{where}: tint names '{mat}', which is neither a material nor a mesh node of the model")


def parts_ok(c, parts, where: str, groups: dict[str, list[str]]) -> None:
    if not isinstance(parts, dict):
        c.err(f"{where}: parts must be an object of group -> part")
        return
    for group, part in parts.items():
        if group not in groups:
            c.err(f"{where}: unknown part group '{group}'")
        elif not isinstance(part, str) or (part != "" and part not in groups[group]):
            c.err(f"{where}: part '{part}' is not in group '{group}'")


def load(path: Path, c):
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        c.err(f"missing file {path}")
        return {}
    except json.JSONDecodeError as e:
        c.err(f"{path}: invalid JSON: {e}")
        return {}
    return doc if isinstance(doc, dict) else {}


def validate_visuals(c, content_dir: Path, races: list[dict], classes: list[dict]) -> None:
    mdoc = load(content_dir / "models.json", c)
    adoc = load(content_dir / "appearance.json", c)
    models = c.list_of(mdoc, "models", "models.json")
    maps = c.list_of(mdoc, "animation_maps", "models.json")
    visuals = c.list_of(mdoc, "item_visuals", "models.json")

    def ids(entries: list[dict], kind: str) -> dict[str, dict]:
        out: dict[str, dict] = {}
        for i, e in enumerate(entries):
            eid = c.string(e, "id", f"{kind}[{i}]")
            if not ID_RE.match(eid or "-"):
                c.err(f"{kind} id '{eid}' must be 1-32 chars of [a-z0-9_]")
            if eid in out:
                c.err(f"duplicate {kind} id '{eid}'")
            out[eid] = e
        return out

    model_by = ids(models, "model")
    map_by = ids(maps, "animation map")
    visual_by = ids(visuals, "item visual")
    cache: dict[str, GltfInfo | None] = {}

    def scene_info(scene: str, where: str) -> GltfInfo | None:
        if scene == "":
            return None
        if scene in cache:
            return cache[scene]
        p = res_path(scene)
        info = None
        if p is None or p.suffix.lower() not in (".glb", ".gltf"):
            c.err(f"{where}: scene must be \"\" or a res:// path to a .glb/.gltf file")
        elif not p.is_file():
            c.err(f"{where}: scene file {scene} does not exist")
        else:
            try:
                info = read_gltf(p)
            except (ValueError, OSError) as e:
                c.err(f"{where}: cannot read {scene}: {e}")
        cache[scene] = info
        return info

    for mid, a in map_by.items():
        w = f"animation map '{mid}'"
        clips = a.get("clips")
        if not isinstance(clips, dict) or not clips:
            c.err(f"{w}: 'clips' must be a non-empty object")
            continue
        for state, spec in clips.items():
            if state not in STATES:
                c.err(f"{w}: unknown state '{state}' (standard states: {', '.join(STATES)})")
            if not isinstance(spec, dict) or not isinstance(spec.get("clip"), str) or not spec["clip"]:
                c.err(f"{w}.{state}: must be an object with a 'clip' name")
                continue
            gs = spec.get("ground_speed")
            if gs is not None and (not isinstance(gs, (int, float)) or isinstance(gs, bool) or not 0.01 <= gs <= 30):
                c.err(f"{w}.{state}: ground_speed must be 0.01..30 m/s (at the model's own size, so tiny animals are slow)")
            if "loop" in spec and not isinstance(spec["loop"], bool):
                c.err(f"{w}.{state}: loop must be true or false")
        if "idle" not in clips:
            c.err(f"{w}: an 'idle' clip is required")
        for key, lo, hi in (("walk_run_threshold", 0.1, 30), ("blend", 0.0, 2.0)):
            v = a.get(key)
            if v is not None and (not isinstance(v, (int, float)) or isinstance(v, bool) or not lo <= v <= hi):
                c.err(f"{w}: {key} must be {lo}..{hi}")

    model_groups: dict[str, dict[str, list[str]]] = {}
    model_attachments: dict[str, dict] = {}
    model_info: dict[str, GltfInfo | None] = {}
    for mid, m in model_by.items():
        w = f"model '{mid}'"
        scene = c.string(m, "scene", w)
        info = scene_info(scene, w)
        model_info[mid] = info
        for key, lo, hi in (("scale", 0.05, 20), ("nameplate_height", 0.2, 30), ("collision_radius", 0.05, 15)):
            if key == "scale" and key not in m:
                continue
            v = c.number(m, key, w)
            if not lo <= v <= hi:
                c.err(f"{w}: {key} must be {lo}..{hi}")
        if "yaw_offset_deg" in m and not -360 <= c.number(m, "yaw_offset_deg", w) <= 360:
            c.err(f"{w}: yaw_offset_deg must be -360..360")
        ph = c.obj(m.get("placeholder"), f"{w}.placeholder")
        if ph.get("shape") not in SHAPES:
            c.err(f"{w}.placeholder: shape must be one of {', '.join(SHAPES)}")
        if not isinstance(ph.get("color"), str) or not COLOR_RE.match(ph["color"]):
            c.err(f"{w}.placeholder: color must be #rrggbb")
        anim = m.get("animations", "")
        if not isinstance(anim, str):
            c.err(f"{w}: 'animations' must be an animation map id")
            anim = ""
        clips_of = map_by.get(anim, {}).get("clips") if anim else None
        if anim and anim not in map_by:
            c.err(f"{w}: unknown animation map '{anim}'")
        elif anim and info is not None and isinstance(clips_of, dict):
            runtime = {godot_clip_name(a) for a in info.animations}
            for state, spec in clips_of.items():
                clip = spec.get("clip") if isinstance(spec, dict) else None
                if isinstance(clip, str) and clip not in info.animations and godot_clip_name(clip) not in runtime:
                    c.err(f"{w}: animation map '{anim}' state '{state}' uses clip '{spec.get('clip')}', not in {scene}")
        if scene and not anim:
            c.err(f"{w}: a model with a scene needs an animation map ('animations')")
        groups = m.get("parts", {})
        if not isinstance(groups, dict) or not all(isinstance(v, list) and all(isinstance(x, str) for x in v) for v in groups.values()):
            c.err(f"{w}: parts must be an object of group -> [node names]")
            groups = {}
        model_groups[mid] = groups
        if info is not None:
            for g, names in groups.items():
                for n in names:
                    if n not in info.meshes:
                        c.err(f"{w}: part '{n}' (group '{g}') is not a mesh node in {scene}")
        parts_ok(c, m.get("default_parts", {}), f"{w}.default_parts", groups)
        if "tints" in m:
            tints_ok(c, m["tints"], w, info)
        att = m.get("attachments", {})
        if not isinstance(att, dict):
            c.err(f"{w}: attachments must be an object of point -> {{bone, position, rotation_deg}}")
            att = {}
        model_attachments[mid] = att
        for point, spec in att.items():
            wa = f"{w}.attachments.{point}"
            if not ID_RE.match(point):
                c.err(f"{wa}: attachment point names are [a-z0-9_]")
            if isinstance(spec, str):
                spec = {"bone": spec}
            if not isinstance(spec, dict) or not isinstance(spec.get("bone"), str):
                c.err(f"{wa}: must name a 'bone'")
                continue
            if info is not None and spec["bone"] not in info.joints:
                c.err(f"{wa}: bone '{spec['bone']}' is not a joint in {scene}")
            if "position" in spec:
                vec3(c, spec["position"], f"{wa}.position", -10, 10)
            if "rotation_deg" in spec:
                vec3(c, spec["rotation_deg"], f"{wa}.rotation_deg", -360, 360)

    for vid, v in visual_by.items():
        w = f"item visual '{vid}'"
        info = scene_info(c.string(v, "scene", w), w)
        if not v.get("scene"):
            c.err(f"{w}: scene is required")
        if v.get("slot") not in SLOTS:
            c.err(f"{w}: slot must be one of {', '.join(SLOTS)}")
        if "attach_point" in v and (not isinstance(v["attach_point"], str) or not ID_RE.match(v["attach_point"])):
            c.err(f"{w}: attach_point must be an attachment point name")
        if "position" in v:
            vec3(c, v["position"], f"{w}.position", -10, 10)
        if "rotation_deg" in v:
            vec3(c, v["rotation_deg"], f"{w}.rotation_deg", -360, 360)
        if "scale" in v:
            scale_ok(c, v["scale"], f"{w}.scale")
        _ = info

    items = load(content_dir / "items.json", c).get("items", [])
    item_visual = {}
    for it in items if isinstance(items, list) else []:
        if isinstance(it, dict) and "visual" in it:
            vis = it["visual"]
            if not isinstance(vis, str):
                c.err(f"item '{it.get('id')}': 'visual' must be an item visual id")
            elif vis not in visual_by:
                c.err(f"item '{it.get('id')}': unknown visual '{vis}'")
            elif isinstance(it.get("id"), str):
                item_visual[it["id"]] = vis
    for f, key in (("mobs.json", "mobs"), ("npcs.json", "npcs")):
        entries = load(content_dir / f, c).get(key, [])
        for e in entries if isinstance(entries, list) else []:
            if not isinstance(e, dict):
                continue
            mdl = e.get("model", "humanoid")
            if not isinstance(mdl, str):
                c.err(f"{key[:-1]} '{e.get('id')}': 'model' must be a model id")
            elif mdl not in model_by:
                c.err(f"{key[:-1]} '{e.get('id')}': unknown model '{mdl}'")
    if "humanoid" not in model_by:
        c.err("models.json: a 'humanoid' model is required (restored player corpses use it)")

    # Appearance: every race has both genders; class looks must fit every player model.
    race_ids = {r.get("id") for r in races}
    class_ids = {cl.get("id") for cl in classes}
    player_models: set[str] = set()
    seen: set[str] = set()
    for i, ra in enumerate(c.list_of(adoc, "races", "appearance.json")):
        rid = c.string(ra, "race", f"appearance.races[{i}]")
        w = f"appearance race '{rid}'"
        if not rid:
            continue
        if rid not in race_ids:
            c.err(f"{w}: unknown race")
        if rid in seen:
            c.err(f"{w}: listed twice")
        seen.add(rid)
        for g in GENDERS:
            spec = c.obj(ra.get(g), f"{w}.{g}")
            mid = c.string(spec, "model", f"{w}.{g}")
            if not mid:
                continue
            outfits = spec.get("outfits", {})
            if not isinstance(outfits, dict):
                c.err(f"{w}.{g}: outfits must be an object of armour type -> look")
                outfits = {}
            # The gender's own look, then each outfit: its fields replace the gender's (tints and parts too).
            looks = [(f"{w}.{g}", spec)]
            for armor, o in outfits.items():
                if armor not in ARMOR_TYPES:
                    c.err(f"{w}.{g}.outfits: '{armor}' is not an armour type ({', '.join(ARMOR_TYPES)})")
                    continue
                if not isinstance(o, dict):
                    c.err(f"{w}.{g}.outfits.{armor}: must be an object")
                    continue
                looks.append((f"{w}.{g}.outfits.{armor}", {**{k: v for k, v in spec.items() if k != "outfits"}, **o}))
            for where, lk in looks:
                lmid = lk.get("model")
                if not isinstance(lmid, str) or lmid not in model_by:
                    c.err(f"{where}: unknown model '{lmid}'")
                    continue
                player_models.add(lmid)
                if "scale" in lk:
                    scale_ok(c, lk["scale"], f"{where}.scale")
                if "tints" in lk:
                    tints_ok(c, lk["tints"], where, model_info.get(lmid))
                parts_ok(c, lk.get("parts", {}), where, model_groups.get(lmid, {}))
    for rid in sorted(race_ids - seen):
        c.err(f"race '{rid}': no entry in appearance.json")
    tiers = adoc.get("armor_tiers")
    if tiers is not None:
        w = "appearance.armor_tiers"
        if not isinstance(tiers, dict):
            c.err(f"{w}: must be an object")
        else:
            if not isinstance(tiers.get("prefix"), str) or not tiers["prefix"]:
                c.err(f"{w}: 'prefix' must be a mesh node name prefix")
            if not isinstance(tiers.get("keep", []), list) or not all(isinstance(k, str) for k in tiers.get("keep", [])):
                c.err(f"{w}: 'keep' must be a list of mesh node names")
            steps = tiers.get("tiers")
            if not isinstance(steps, list) or not steps:
                c.err(f"{w}: 'tiers' must be a non-empty list")
                steps = []
            last = 0
            for i, t in enumerate(steps):
                ok = isinstance(t, dict) and isinstance(t.get("min_level"), int) and not isinstance(t.get("min_level"), bool)
                if not ok or not isinstance(t.get("tint"), str) or not COLOR_RE.match(t["tint"]):
                    c.err(f"{w}.tiers[{i}]: needs an integer min_level and a #rrggbb tint")
                    continue
                if t["min_level"] <= last and i > 0:
                    c.err(f"{w}.tiers[{i}]: min_level must increase")
                last = t["min_level"]
            if "epic" in tiers and (not isinstance(tiers["epic"], str) or not COLOR_RE.match(tiers["epic"])):
                c.err(f"{w}: 'epic' must be a #rrggbb tint")
    seen.clear()
    for i, ca in enumerate(c.list_of(adoc, "classes", "appearance.json")):
        cid = c.string(ca, "class", f"appearance.classes[{i}]")
        w = f"appearance class '{cid}'"
        if not cid:
            continue
        if cid not in class_ids:
            c.err(f"{w}: unknown class")
        if cid in seen:
            c.err(f"{w}: listed twice")
        seen.add(cid)
        for mid in sorted(player_models):
            if not model_by.get(mid, {}).get("class_style", True):
                continue  # dressed by its own outfit: class tints and parts do not apply
            if "tints" in ca:
                tints_ok(c, ca["tints"], f"{w} (on model '{mid}')", model_info.get(mid))
            parts_ok(c, ca.get("parts", {}), f"{w} (on model '{mid}')", model_groups.get(mid, {}))
        held = ca.get("held", {})
        if not isinstance(held, dict):
            c.err(f"{w}: held must be an object of slot -> item id")
            held = {}
        for slot, item in held.items():
            if slot not in SLOTS:
                c.err(f"{w}: held slot '{slot}' must be one of {', '.join(SLOTS)}")
            if not isinstance(item, str):
                c.err(f"{w}: held '{slot}' must be an item id")
                continue
            vid = item_visual.get(item)
            if vid is None:
                c.err(f"{w}: holds '{item}', which is not an item with a visual")
                continue
            v = visual_by.get(vid, {})
            if v.get("slot") != slot:
                c.err(f"{w}: item '{item}' belongs in {v.get('slot')}, not {slot}")
            point = v.get("attach_point", slot)
            if not isinstance(point, str):
                continue  # already reported on the item visual
            for mid in sorted(player_models):
                att = model_attachments.get(mid, {})
                if model_info.get(mid) is not None and point not in att:
                    c.err(f"{w}: model '{mid}' has no '{point}' attachment point for '{item}'")


ENV_DIR = ROOT / "client" / "assets" / "env"


def validate_environments(c, content_dir: Path, zones: list[dict]) -> None:
    """content/environments.json: every zone's biome has an entry, and every model it names exists."""
    path = content_dir / "environments.json"
    if not path.is_file():
        c.err("environments.json is missing")
        return
    doc = load(path, c)
    biomes = doc.get("biomes", {})
    if not isinstance(biomes, dict):
        c.err("environments.json: 'biomes' must be an object")
        return
    for z in zones:
        b = (z.get("theme") or {}).get("biome")
        if b and b not in biomes:
            c.err(f"zone '{z.get('id')}': biome '{b}' has no entry in environments.json")
    default = doc.get("default", {})
    if not isinstance(default, dict):
        c.err("environments.json: 'default' must be an object")
        default = {}
    for bid, b in [("default", default)] + list(biomes.items()):
        w = "environments.json default" if bid == "default" else f"environments.json biome '{bid}'"
        if not isinstance(b, dict):
            c.err(f"{w}: must be an object")
            continue
        for key in ("sun", "horizon", "ambient_color"):
            if key in b and (not isinstance(b[key], str) or not COLOR_RE.match(b[key])):
                c.err(f"{w}: {key} must be #rrggbb")
        for key, lo, hi in (("sun_energy", 0.0, 4.0), ("ambient", 0.0, 4.0), ("fog_density", 0.0, 0.2), ("sun_pitch", 5.0, 90.0)):
            v = b.get(key)
            if v is not None and (not isinstance(v, (int, float)) or isinstance(v, bool) or not lo <= v <= hi):
                c.err(f"{w}: {key} must be {lo}..{hi}")
        for kind in ("scatter", "decor"):
            entries = b.get(kind, [])
            if not isinstance(entries, list):
                c.err(f"{w}: '{kind}' must be a list")
                continue
            for i, e in enumerate(entries):
                m = e.get("model") if isinstance(e, dict) else None
                if not isinstance(m, str) or not (ENV_DIR / f"{m}.glb").is_file():
                    c.err(f"{w}.{kind}[{i}]: model '{m}' is not a file in client/assets/env")
                if kind == "decor" and isinstance(e, dict):
                    sc = e.get("scale", 1.0)
                    if not isinstance(sc, (int, float)) or isinstance(sc, bool) or not 0 < sc <= 20:
                        c.err(f"{w}.decor[{i}]: scale must be a number above 0 and at most 20")
                if kind == "scatter" and isinstance(e, dict):
                    d = e.get("density")
                    if not isinstance(d, (int, float)) or not 0 < d <= 10:
                        c.err(f"{w}.scatter[{i}]: density must be above 0 and at most 10 per 100 m2")
                    sc = e.get("scale")
                    if not (isinstance(sc, list) and len(sc) == 2 and all(isinstance(x, (int, float)) for x in sc) and 0 < sc[0] <= sc[1]):
                        c.err(f"{w}.scatter[{i}]: scale must be [min, max]")
                    if "tint" in e and (not isinstance(e["tint"], str) or not COLOR_RE.match(e["tint"])):
                        c.err(f"{w}.scatter[{i}]: tint must be #rrggbb")


FX_KINDS = ("cast", "projectile", "burst", "rise", "nova", "column", "beam", "marker", "aura")
FX_EVENTS = ("spell_hit", "ability_hit", "dot_tick", "debuff", "resist", "heal", "hot_tick", "buff", "mana_restore", "lifetap",
             "drain_tick", "mez", "root", "snare", "slow", "fear", "charm", "stun", "resurrect", "teleport", "summon", "illusion",
             "song_pulse", "dispel", "level_up")
FX_STATES = ("stunned", "mezzed", "fleeing", "rooted", "charmed")
FX_SCREEN = ("low_health", "stunned", "mezzed", "rooted", "fleeing", "level_up")
SCHOOLS = ("none", "magic", "fire", "cold", "poison", "disease")


def validate_effects(c, content_dir: Path, spells: list[dict], abilities: list[dict]) -> None:
    """content/effects.json: kinds, colours and that every reference names an effect, event, state or spell."""
    path = content_dir / "effects.json"
    if not path.is_file():
        c.err("effects.json is missing")
        return
    doc = load(path, c)
    effects = doc.get("effects", {})
    if not isinstance(effects, dict) or not effects:
        c.err("effects.json: 'effects' must be a non-empty object")
        return

    def obj(key: str) -> dict:
        v = doc.get(key)
        if v is None:
            return {}
        if not isinstance(v, dict):
            c.err(f"effects.json {key}: must be an object")
            return {}
        return v

    def color_ok(v, w: str, school_ok: bool) -> None:
        if not (isinstance(v, str) and (COLOR_RE.match(v) or (school_ok and v == "school"))):
            c.err(f"{w}: colour must be #rrggbb" + (" or 'school'" if school_ok else ""))

    def ref(v, w: str) -> None:
        if not isinstance(v, str) or v not in effects:
            c.err(f"{w}: '{v}' is not an effect in effects.json")

    sc = obj("school_colors")
    for k in SCHOOLS:
        if k not in sc:
            c.err(f"effects.json school_colors: missing '{k}'")
        else:
            color_ok(sc[k], f"effects.json school_colors.{k}", False)
    for eid, e in effects.items():
        w = f"effects.json effect '{eid}'"
        if not isinstance(e, dict):
            c.err(f"{w}: must be an object")
            continue
        if e.get("kind") not in FX_KINDS:
            c.err(f"{w}: kind must be one of {', '.join(FX_KINDS)}")
        color_ok(e.get("color"), w, True)
        for key, lo, hi in (("size", 0.01, 20.0), ("duration", 0.05, 30.0), ("speed", 1.0, 200.0), ("count", 1, 400)):
            v = e.get(key)
            if v is not None and (not isinstance(v, (int, float)) or isinstance(v, bool) or not lo <= v <= hi):
                c.err(f"{w}: {key} must be {lo}..{hi}")
        if "impact" in e:
            ref(e["impact"], f"{w}.impact")
    for ev, eid in obj("events").items():
        if ev not in FX_EVENTS:
            c.err(f"effects.json events: unknown event '{ev}' (known: {', '.join(FX_EVENTS)})")
        ref(eid, f"effects.json events.{ev}")
    for st, eid in obj("states").items():
        if st not in FX_STATES:
            c.err(f"effects.json states: unknown state '{st}' (known: {', '.join(FX_STATES)})")
        ref(eid, f"effects.json states.{st}")
    ref(doc.get("projectile"), "effects.json projectile")
    types = {a.get("type") for a in spells + abilities if isinstance(a, dict)}
    ptypes = doc.get("projectile_types", [])
    if not isinstance(ptypes, list):
        c.err("effects.json projectile_types: must be a list")
        ptypes = []
    for t in ptypes:
        if t not in types:
            c.err(f"effects.json projectile_types: no spell or ability has type '{t}'")
    names = {a.get("name") for a in spells + abilities if isinstance(a, dict)}
    for name, o in obj("spells").items():
        if name not in names:
            c.err(f"effects.json spells: '{name}' is not a spell or ability name")
        if not isinstance(o, dict) or not o:
            c.err(f"effects.json spells.{name}: must name at least one of cast, projectile, impact")
            continue
        for k, v in o.items():
            if k not in ("cast", "projectile", "impact"):
                c.err(f"effects.json spells.{name}: unknown key '{k}'")
            ref(v, f"effects.json spells.{name}.{k}")
    for k, v in obj("screen").items():
        if k not in FX_SCREEN:
            c.err(f"effects.json screen: unknown cue '{k}'")
        if not isinstance(v, dict):
            c.err(f"effects.json screen.{k}: must be an object")
            continue
        color_ok(v.get("color"), f"effects.json screen.{k}", False)


AUDIO_BUSES = ("Music", "Ambient", "SFX", "UI")
CLIENT = ROOT / "client"


def validate_audio(c, content_dir: Path, zones: list[dict]) -> None:
    """content/audio.json: every event's files exist, buses are known, and every reference names an event."""
    path = content_dir / "audio.json"
    if not path.is_file():
        c.err("audio.json is missing")
        return
    doc = load(path, c)
    events = doc.get("events", {})
    if not isinstance(events, dict) or not events:
        c.err("audio.json: 'events' must be a non-empty object")
        return
    for name, e in events.items():
        w = f"audio.json event '{name}'"
        if not isinstance(e, dict):
            c.err(f"{w}: must be an object")
            continue
        files = e.get("files", [e.get("file")] if "file" in e else [])
        if not isinstance(files, list) or not files:
            c.err(f"{w}: needs 'file' or a non-empty 'files' list")
            files = []
        for f in files:
            if not isinstance(f, str) or not f.startswith("res://") or not (CLIENT / f[len("res://"):]).is_file():
                c.err(f"{w}: file '{f}' is not a file in the client")
        if e.get("bus") not in AUDIO_BUSES:
            c.err(f"{w}: bus must be one of {', '.join(AUDIO_BUSES)}")
        v = e.get("volume_db", 0.0)
        if not isinstance(v, (int, float)) or not -60 <= v <= 12:
            c.err(f"{w}: volume_db must be -60..12")
        p = e.get("pitch", 1.0)
        if not isinstance(p, (int, float)) or not 0.25 <= p <= 4:
            c.err(f"{w}: pitch must be 0.25..4")

    def ref(v, w: str, looping: bool) -> None:
        for one in (v if isinstance(v, list) else [v]):
            if not isinstance(one, str) or one not in events:
                c.err(f"{w}: '{one}' is not an event in audio.json")
            elif looping and isinstance(events[one], dict) and not events[one].get("loop"):
                c.err(f"{w}: '{one}' must loop")  # (a malformed event was reported above)

    def obj(v, w: str) -> dict:
        if v is None:
            return {}
        if not isinstance(v, dict):
            c.err(f"audio.json {w}: must be an object")
            return {}
        return v

    zone_ids = {z.get("id") for z in zones}
    biomes = {(z.get("theme") or {}).get("biome") for z in zones} - {None}
    for key in ("music", "ambience"):
        sec = obj(doc.get(key), key)
        ref(sec.get("default"), f"audio.json {key}.default", True)
        bsec = obj(sec.get("biomes"), f"{key}.biomes")
        for b, v in bsec.items():
            ref(v, f"audio.json {key}.biomes.{b}", True)
        for zid, v in obj(sec.get("zones"), f"{key}.zones").items():
            if zid not in zone_ids:
                c.err(f"audio.json {key}.zones: unknown zone '{zid}'")
            ref(v, f"audio.json {key}.zones.{zid}", True)
        missing = biomes - set(bsec.keys())
        if missing:
            c.err(f"audio.json {key}.biomes: no entry for {', '.join(sorted(missing))}")
    for key in ("boss_music", "raid_boss_music"):
        ref(doc.get(key), f"audio.json {key}", True)
    cm = obj(doc.get("combat_music"), "combat_music")
    ref(cm.get("event"), "audio.json combat_music.event", True)
    fs = obj(doc.get("footsteps"), "footsteps")
    surfaces = obj(fs.get("surfaces"), "footsteps.surfaces")
    for sname, v in surfaces.items():
        ref(v, f"audio.json footsteps.surfaces.{sname}", False)
    for b, sname in obj(fs.get("biomes"), "footsteps.biomes").items():
        if sname not in surfaces:
            c.err(f"audio.json footsteps.biomes.{b}: unknown surface '{sname}'")
    if fs.get("default") not in surfaces:
        c.err("audio.json footsteps.default: unknown surface")
    schools = obj(doc.get("schools"), "schools")
    for school in SCHOOLS:
        sc = schools.get(school)
        if not isinstance(sc, dict):
            c.err(f"audio.json schools: missing '{school}'")
            continue
        ref(sc.get("cast"), f"audio.json schools.{school}.cast", False)
        ref(sc.get("impact"), f"audio.json schools.{school}.impact", False)


def validate_assets_listed(c) -> None:
    """Every asset folder the client ships is named in ASSETS.md's build table (the Credits screen is
    built from it), and LICENSES/ holds the licences the table names."""
    text = (ROOT / "ASSETS.md").read_text(encoding="utf-8")
    start = text.find("## In the build")
    table = text[start:text.find("\n## ", start + 5)] if start >= 0 else ""
    assets = ROOT / "client" / "assets"
    folders = [p for p in assets.iterdir() if p.is_dir()]
    for top in folders:
        subs = [p for p in top.iterdir() if p.is_dir()] or [top]
        for d in subs:
            rel = "client/" + d.relative_to(ROOT / "client").as_posix() + "/"
            if rel not in table and ("client/" + top.relative_to(ROOT / "client").as_posix() + "/") not in table:
                c.err(f"ASSETS.md: {rel} ships in the client but is not listed in the build table")
    for lic, f in (("CC BY 4.0", "CC-BY-4.0.txt"), ("CC0 1.0", "CC0-1.0.txt")):
        if lic in table and not (ROOT / "LICENSES" / f).is_file():
            c.err(f"LICENSES/{f} is missing (ASSETS.md lists assets under {lic})")

