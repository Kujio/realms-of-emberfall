## A zone as the client knows it: the bundled copy of content/zones/<id>.json (client/data/zones,
## synced by tools/sync_client_content.py, scene exports merged in), its geometry for prediction,
## the dungeon layout built from the server's seed, and the layout hash checked against the server's.
extends RefCounted

const ZoneGeom := preload("res://scripts/world/zone_geom.gd")
const DungeonGen := preload("res://scripts/world/dungeon_gen.gd")

const DATA_DIR: String = "res://data/zones"

var id: String = ""
var name: String = ""
var kind: String = "outdoor"
var doc: Dictionary = {}
var theme: Dictionary = {}
var geom: ZoneGeom = ZoneGeom.new()
var dungeon: DungeonGen = null
var bounds: Rect2 = Rect2()
var loaded: bool = false


## Loads client/data/zones/<zone_id>.json; false (and `loaded` stays false) if it is missing or broken.
func load_file(zone_id: String, server_seed: int = 0) -> bool:
	id = zone_id
	var path: String = DATA_DIR.path_join(zone_id + ".json")
	if not FileAccess.file_exists(path):
		push_warning("no bundled data for zone '%s'" % zone_id)
		return false
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not (parsed is Dictionary):
		push_warning("zone data %s is not an object" % path)
		return false
	from_doc(parsed as Dictionary, server_seed)
	return true


func from_doc(d: Dictionary, server_seed: int = 0) -> void:
	doc = d
	id = str(d.get("id", id))
	name = str(d.get("name", id))
	kind = str(d.get("kind", "outdoor"))
	theme = d.get("theme", {}) as Dictionary
	var terrain: Dictionary = d.get("terrain", {}) as Dictionary
	geom.base = float(terrain.get("base", d.get("ground_y", 0.0)))
	for h: Variant in terrain.get("hills", []):
		var a: Array = h as Array
		geom.hills.append(PackedFloat32Array([float(a[0]), float(a[1]), float(a[2]), float(a[3])]))
	for o: Variant in d.get("obstacles", []):
		var od: Dictionary = o as Dictionary
		geom.circles.append(PackedFloat32Array([float(od["x"]), float(od["z"]), float(od["radius"])]))
	for w: Variant in d.get("walls", []):
		var wd: Dictionary = w as Dictionary
		geom.boxes.append(PackedFloat32Array([float(wd["x"]), float(wd["z"]), float(wd["hx"]), float(wd["hz"]), float(wd.get("yaw", 0.0))]))
	if d.has("dungeon"):
		var dd: Dictionary = d["dungeon"] as Dictionary
		dungeon = DungeonGen.new()
		dungeon.seed_value = server_seed if server_seed != 0 else int(dd.get("seed", 1))
		dungeon.rooms_wanted = int(dd.get("rooms", 8))
		dungeon.cell = float(dd.get("cell", 4.0))
		dungeon.sector = int(dd.get("sector", 12))
		dungeon.min_room = int(dd.get("min_room", 4))
		dungeon.max_room = int(dd.get("max_room", 8))
		dungeon.corridor = int(dd.get("corridor", 2))
		dungeon.generate()
		geom.tiles = dungeon.solid
		geom.tile_w = dungeon.w
		geom.tile_h = dungeon.h
		geom.tile_cell = dungeon.cell
		geom.tile_origin = dungeon.origin
		bounds = Rect2(dungeon.origin, Vector2(float(dungeon.w) * dungeon.cell, float(dungeon.h) * dungeon.cell))
	else:
		var b: Dictionary = d.get("bounds", {}) as Dictionary
		var x0: float = float(b.get("min_x", -100.0))
		var z0: float = float(b.get("min_z", -100.0))
		bounds = Rect2(x0, z0, float(b.get("max_x", 100.0)) - x0, float(b.get("max_z", 100.0)) - z0)
	loaded = true


## Mirrors the server's hash_zone (server/src/game/content_world.cpp).
func layout_hash() -> int:
	var h: ZoneGeom.LayoutHash = ZoneGeom.LayoutHash.new()
	h.add_cm(bounds.position.x)
	h.add_cm(bounds.position.y)
	h.add_cm(bounds.end.x)
	h.add_cm(bounds.end.y)
	h.add_cm(geom.base)
	h.add(geom.hills.size())
	for hl: PackedFloat32Array in geom.hills:
		h.add_cm(hl[0])
		h.add_cm(hl[1])
		h.add_cm(hl[2])
		h.add_cm(hl[3])
	h.add(geom.circles.size())
	for c: PackedFloat32Array in geom.circles:
		h.add_cm(c[0])
		h.add_cm(c[1])
		h.add_cm(c[2])
	h.add(geom.boxes.size())
	for b: PackedFloat32Array in geom.boxes:
		h.add_cm(b[0])
		h.add_cm(b[1])
		h.add_cm(b[2])
		h.add_cm(b[3])
		h.add_q(b[4], 10000.0)
	h.add(dungeon.hash_value() if dungeon != null else 0)
	return h.h


func color(key: String, fallback: Color) -> Color:
	var v: Variant = theme.get(key, "")
	if v is String and (v as String).begins_with("#"):
		return Color.html(v as String)
	return fallback


func zone_lines() -> Array:
	var out: Array = []
	for l: Variant in doc.get("zone_lines", []):
		var ld: Dictionary = (l as Dictionary).duplicate()
		if ld.has("room") and dungeon != null:
			# Same placement as the server (content_world.cpp build_dungeon).
			var idx: int = 0 if str(ld["room"]) == "entrance" else (dungeon.boss if str(ld["room"]) == "boss" else int(ld["room"]))
			var c: Vector2 = dungeon.room_center(idx)
			ld["x"] = c.x
			ld["z"] = c.y - dungeon.room_radius(idx) * 0.6
			ld["hx"] = minf(float(ld.get("hx", 4.0)), dungeon.cell)
			ld["hz"] = minf(float(ld.get("hz", 4.0)), dungeon.cell * 0.5)
		out.append(ld)
	return out
