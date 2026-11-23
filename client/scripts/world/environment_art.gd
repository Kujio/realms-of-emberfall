## The per-biome look of a zone beyond its props (Phase 9): lighting, ground clutter and dungeon
## decorations, read from res://data/environments.json (synced from content/environments.json by
## tools/sync_client_content.py). Everything here is visual: nothing collides, and the server never
## sees it. Placement is seeded from the zone, so every client draws the same world.
extends RefCounted

const ZoneData := preload("res://scripts/world/zone_data.gd")
const PropVariants := preload("res://scripts/world/prop_variants.gd")

const DATA_PATH: String = "res://data/environments.json"
const ENV_DIR: String = "res://assets/env"
## Ground clutter is built in square chunks this wide, each fading out past CLUTTER_DISTANCE.
const CHUNK: float = 64.0
const CLUTTER_DISTANCE: float = 110.0
## Scales clutter density (the graphics quality setting): 0 turns it off.
static var density_scale: float = 1.0

static var _data: Dictionary = {}
static var _meshes: Dictionary = {}  # model -> Array of [Mesh, Transform3D] (or [] when missing)


static func data() -> Dictionary:
	if _data.is_empty():
		var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(DATA_PATH))
		_data = parsed if parsed is Dictionary else {"default": {}, "biomes": {}}
	return _data


static func biome_of(z: ZoneData) -> String:
	return str(z.theme.get("biome", ""))


## The biome's settings over the defaults.
static func settings(z: ZoneData) -> Dictionary:
	var out: Dictionary = (data().get("default", {}) as Dictionary).duplicate()
	var b: Variant = (data().get("biomes", {}) as Dictionary).get(biome_of(z))
	if b is Dictionary:
		out.merge(b as Dictionary, true)
	return out


static func color_of(v: Variant, fallback: Color) -> Color:
	return Color.html(str(v)) if v is String and str(v).is_valid_html_color() else fallback


## The meshes of an art model with their transforms relative to its root (for MultiMesh clutter),
## with materials that use the model's vertex colours.
static func _mesh_parts(model: String) -> Array:
	if _meshes.has(model):
		return _meshes[model] as Array
	var parts: Array = []
	var path: String = ENV_DIR.path_join(model + ".glb")
	var ps: PackedScene = load(path) as PackedScene if ResourceLoader.exists(path) else null
	if ps != null:
		var root: Node3D = ps.instantiate() as Node3D
		for n: Node in root.find_children("*", "MeshInstance3D", true, false):
			var mi: MeshInstance3D = n as MeshInstance3D
			if mi.mesh == null:
				continue
			var xf: Transform3D = Transform3D.IDENTITY
			var p: Node = mi
			while p != null and p != root:
				if p is Node3D:
					xf = (p as Node3D).transform * xf
				p = p.get_parent()
			parts.append([mi.mesh, xf])
		root.free()
	else:
		push_warning("environment: missing model %s" % path)
	_meshes[model] = parts
	return parts


static func _clutter_material(mesh: Mesh, tint: Color) -> Material:
	var src: BaseMaterial3D = mesh.surface_get_material(0) as BaseMaterial3D if mesh.get_surface_count() > 0 else null
	if src == null:
		src = StandardMaterial3D.new()
	return PropVariants._material(src, tint)


## Walk-through ground clutter over an outdoor zone or a hub, kept off anything solid.
static func scatter(parent: Node3D, z: ZoneData) -> void:
	var list: Array = settings(z).get("scatter", []) as Array
	if list.is_empty() or density_scale <= 0.0:
		return
	var holder: Node3D = Node3D.new()
	holder.name = "Clutter"
	parent.add_child(holder)
	var rng: RandomNumberGenerator = RandomNumberGenerator.new()
	var b: Rect2 = z.bounds
	var nx: int = int(ceil(b.size.x / CHUNK))
	var nz: int = int(ceil(b.size.y / CHUNK))
	for k: int in range(list.size()):
		var spec: Dictionary = list[k] as Dictionary
		var parts: Array = _mesh_parts(str(spec.get("model", "")))
		if parts.is_empty():
			continue
		var per_chunk: int = int(round(float(spec.get("density", 0.0)) * density_scale * CHUNK * CHUNK / 100.0))
		var sc: Array = spec.get("scale", [1.0, 1.0]) as Array
		var tint: Color = color_of(spec.get("tint"), Color.WHITE)
		for cz: int in range(nz):
			for cx: int in range(nx):
				rng.seed = hash([z.id, k, cx, cz])
				var xforms: Array[Transform3D] = []
				for i: int in range(per_chunk):
					var x: float = b.position.x + (float(cx) + rng.randf()) * CHUNK
					var zz: float = b.position.y + (float(cz) + rng.randf()) * CHUNK
					if x > b.end.x or zz > b.end.y or z.geom.overlaps(x, zz, 0.6):
						continue
					var s: float = rng.randf_range(float(sc[0]), float(sc[1]))
					var basis: Basis = Basis(Vector3.UP, rng.randf() * TAU).scaled(Vector3.ONE * s)
					xforms.append(Transform3D(basis, Vector3(x, z.geom.height(x, zz), zz)))
				if xforms.is_empty():
					continue
				for part: Array in parts:
					var mm: MultiMesh = MultiMesh.new()
					mm.transform_format = MultiMesh.TRANSFORM_3D
					mm.mesh = part[0] as Mesh
					mm.instance_count = xforms.size()
					for i: int in range(xforms.size()):
						mm.set_instance_transform(i, xforms[i] * (part[1] as Transform3D))
					var mmi: MultiMeshInstance3D = MultiMeshInstance3D.new()
					mmi.multimesh = mm
					mmi.material_override = _clutter_material(part[0] as Mesh, tint)
					mmi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
					mmi.visibility_range_end = CLUTTER_DISTANCE * PropVariants.distance_scale
					mmi.visibility_range_end_margin = 15.0
					mmi.visibility_range_fade_mode = GeometryInstance3D.VISIBILITY_RANGE_FADE_SELF
					holder.add_child(mmi)


## Decorations along the walls of every dungeon room (the biome's "decor" list), seeded by the layout.
static func dungeon_decor(parent: Node3D, z: ZoneData) -> void:
	var d: RefCounted = z.dungeon
	var list: Array = settings(z).get("decor", []) as Array
	if d == null or list.is_empty():
		return
	var holder: Node3D = Node3D.new()
	holder.name = "Decor"
	parent.add_child(holder)
	var rng: RandomNumberGenerator = RandomNumberGenerator.new()
	var rooms: Array = d.get("rooms") as Array
	var cell: float = float(d.get("cell"))
	var origin: Vector2 = d.get("origin") as Vector2
	for i: int in range(rooms.size()):
		var r: Dictionary = rooms[i] as Dictionary
		rng.seed = hash([z.id, i])
		var rw: int = int(r["w"])
		var rh: int = int(r["h"])
		var count: int = clampi(rw + rh, 3, 9)
		for k: int in range(count):
			var spec: Dictionary = list[rng.randi() % list.size()] as Dictionary
			# On the room's rim (one of its four sides), half a cell in from the wall.
			var side: int = rng.randi() % 4
			var u: float = rng.randf_range(0.6, maxf(0.6, float(rw if side < 2 else rh) - 0.6))
			var cx: float
			var cz: float
			match side:
				0:
					cx = float(r["x"]) + u
					cz = float(r["z"]) + 0.6
				1:
					cx = float(r["x"]) + u
					cz = float(r["z"]) + float(rh) - 0.6
				2:
					cx = float(r["x"]) + 0.6
					cz = float(r["z"]) + u
				_:
					cx = float(r["x"]) + float(rw) - 0.6
					cz = float(r["z"]) + u
			var path: String = ENV_DIR.path_join(str(spec.get("model", "")) + ".glb")
			var ps: PackedScene = load(path) as PackedScene if ResourceLoader.exists(path) else null
			if ps == null:
				continue
			var n: Node3D = ps.instantiate() as Node3D
			n.position = Vector3(origin.x + cx * cell, 0.0, origin.y + cz * cell)
			n.rotation.y = rng.randf() * TAU
			n.scale = Vector3.ONE * float(spec.get("scale", 1.0))
			holder.add_child(n)
			PropVariants.dress(n, Color.WHITE, 0.0)
