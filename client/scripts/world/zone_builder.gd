## Builds a zone's visuals from its ZoneData: sky and fog from the zone theme, then by kind
##   hub     - flat ground plus the hand-authored scene (res://zones/<id>.tscn)
##   outdoor - a terrain mesh from the hills, plus the scattered props (res://zones/props/<prop>.tscn)
##   dungeon - floor and wall tiles from the seeded layout (MultiMesh), a brazier in every room
##   test    - the Proving Flats' grid with boundary posts and rings
## and a shimmering gateway at every zone line. Collision is not built here: prediction uses
## ZoneGeom, the same data the server collides with.
extends RefCounted

const ZoneData := preload("res://scripts/world/zone_data.gd")
const DungeonGen := preload("res://scripts/world/dungeon_gen.gd")

const PROP_DIR: String = "res://zones/props"
const WALL_HEIGHT: float = 6.0
## Ground colours come out much lighter on screen than their swatches (sky light on flat ground); this
## keeps them close to the theme's intent until the Phase 9 art pass sets real materials.
const GROUND_DARKEN: float = 0.4

static var _prop_cache: Dictionary = {}


static func build(parent: Node3D, z: ZoneData) -> void:
	_environment(parent, z)
	match z.kind:
		"dungeon":
			_dungeon(parent, z)
		"hub":
			_flat_ground(parent, z, z.color("ground", Color(0.42, 0.39, 0.31)))
			_scene(parent, z)
			_props(parent, z)
		"test":
			_test_ground(parent, z)
		_:
			_terrain(parent, z)
			_props(parent, z)
	_zone_lines(parent, z)


static func _environment(parent: Node3D, z: ZoneData) -> void:
	var dungeon: bool = z.kind == "dungeon"
	var sky_mat: ProceduralSkyMaterial = ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = z.color("sky", Color(0.29, 0.39, 0.6))
	sky_mat.sky_horizon_color = z.color("horizon", Color(0.78, 0.63, 0.52))
	sky_mat.ground_horizon_color = Color(0.45, 0.38, 0.32)
	sky_mat.ground_bottom_color = Color(0.2, 0.17, 0.15)
	var sky: Sky = Sky.new()
	sky.sky_material = sky_mat
	var env: Environment = Environment.new()
	if dungeon:
		env.background_mode = Environment.BG_COLOR
		env.background_color = z.color("fog", Color(0.08, 0.07, 0.06))
		env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
		env.ambient_light_color = Color(0.35, 0.3, 0.28)
		env.ambient_light_energy = 0.6
	else:
		env.background_mode = Environment.BG_SKY
		env.sky = sky
		env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
		env.ambient_light_energy = 0.45
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.fog_enabled = true
	env.fog_light_color = z.color("fog", Color(0.7, 0.6, 0.55))
	env.fog_density = 0.02 if dungeon else 0.002
	var we: WorldEnvironment = WorldEnvironment.new()
	we.name = "Environment"
	we.environment = env
	parent.add_child(we)
	var sun: DirectionalLight3D = DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-52.0, -35.0, 0.0)
	sun.light_energy = 0.15 if dungeon else 0.9
	sun.light_color = Color(1.0, 0.93, 0.82)
	sun.shadow_enabled = not dungeon
	parent.add_child(sun)


static func _material(c: Color) -> StandardMaterial3D:
	var m: StandardMaterial3D = StandardMaterial3D.new()
	m.albedo_color = c
	m.roughness = 1.0
	return m


static func _flat_ground(parent: Node3D, z: ZoneData, c: Color) -> void:
	var plane: PlaneMesh = PlaneMesh.new()
	plane.size = z.bounds.size
	var g: MeshInstance3D = MeshInstance3D.new()
	g.name = "Ground"
	g.mesh = plane
	g.material_override = _material(c.darkened(GROUND_DARKEN))
	g.position = Vector3(z.bounds.get_center().x, z.geom.base, z.bounds.get_center().y)
	parent.add_child(g)


## A height-field mesh over the zone, coloured by height (low = the theme's ground, high = rock).
static func _terrain(parent: Node3D, z: ZoneData) -> void:
	var size: Vector2 = z.bounds.size
	var step: float = maxf(4.0, maxf(size.x, size.y) / 160.0)
	var nx: int = int(ceil(size.x / step)) + 1
	var nz: int = int(ceil(size.y / step)) + 1
	var low: Color = z.color("ground", Color(0.36, 0.48, 0.25)).darkened(GROUND_DARKEN)
	var high: Color = z.color("high", low.lerp(Color(0.5, 0.47, 0.42), 0.6)).darkened(GROUND_DARKEN * 0.6)
	var top: float = 0.01
	for h: PackedFloat32Array in z.geom.hills:
		top = maxf(top, absf(h[3]))
	var verts: PackedVector3Array = PackedVector3Array()
	var cols: PackedColorArray = PackedColorArray()
	verts.resize(nx * nz)
	cols.resize(nx * nz)
	for iz: int in range(nz):
		for ix: int in range(nx):
			var x: float = minf(z.bounds.position.x + float(ix) * step, z.bounds.end.x)
			var zz: float = minf(z.bounds.position.y + float(iz) * step, z.bounds.end.y)
			var y: float = z.geom.height(x, zz)
			verts[iz * nx + ix] = Vector3(x, y, zz)
			# A little hash noise breaks up the flat colour.
			var n: float = fposmod(sin(x * 12.9898 + zz * 78.233) * 43758.5453, 1.0) * 0.06
			cols[iz * nx + ix] = low.lerp(high, clampf((y - z.geom.base) / top, 0.0, 1.0)).darkened(n)
	var idx: PackedInt32Array = PackedInt32Array()
	for iz: int in range(nz - 1):
		for ix: int in range(nx - 1):
			var a: int = iz * nx + ix
			idx.append_array([a, a + 1, a + nx, a + 1, a + nx + 1, a + nx])
	var st: SurfaceTool = SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for i: int in range(verts.size()):
		st.set_color(cols[i])
		st.add_vertex(verts[i])
	for i: int in idx:
		st.add_index(i)
	st.generate_normals()
	var mesh: ArrayMesh = st.commit()
	var mi: MeshInstance3D = MeshInstance3D.new()
	mi.name = "Terrain"
	mi.mesh = mesh
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.vertex_color_use_as_albedo = true
	mat.vertex_color_is_srgb = true  # the theme colours are sRGB, like every other colour in the content
	mat.roughness = 1.0
	mi.material_override = mat
	parent.add_child(mi)


static func _prop_scene(prop: String) -> PackedScene:
	if _prop_cache.has(prop):
		return _prop_cache[prop] as PackedScene
	var path: String = PROP_DIR.path_join(prop + ".tscn")
	var ps: PackedScene = load(path) as PackedScene if ResourceLoader.exists(path) else null
	if ps == null:
		push_warning("missing prop scene %s" % path)
	_prop_cache[prop] = ps
	return ps


static func _props(parent: Node3D, z: ZoneData) -> void:
	var list: Array = z.doc.get("props", []) as Array
	if list.is_empty():
		return
	var holder: Node3D = Node3D.new()
	holder.name = "Props"
	parent.add_child(holder)
	for p: Variant in list:
		var pd: Dictionary = p as Dictionary
		var ps: PackedScene = _prop_scene(str(pd.get("prop", "")))
		if ps == null:
			continue
		var n: Node3D = ps.instantiate() as Node3D
		var x: float = float(pd["x"])
		var zz: float = float(pd["z"])
		n.position = Vector3(x, z.geom.height(x, zz), zz)
		n.rotation.y = float(pd.get("yaw", 0.0))
		holder.add_child(n)


static func _scene(parent: Node3D, z: ZoneData) -> void:
	var path: String = str(z.doc.get("scene", ""))
	if path == "" or not ResourceLoader.exists(path):
		if path != "":
			push_warning("zone scene %s is missing" % path)
		return
	var ps: PackedScene = load(path) as PackedScene
	if ps == null:
		return
	var n: Node = ps.instantiate()
	n.name = "Scene"
	parent.add_child(n)


static func _dungeon(parent: Node3D, z: ZoneData) -> void:
	var d: DungeonGen = z.dungeon
	if d == null or d.rooms.is_empty():
		_flat_ground(parent, z, z.color("ground", Color(0.3, 0.27, 0.24)))
		return
	var floors: Array[Vector3] = []
	var walls: Array[Vector3] = []
	for iz: int in range(d.h):
		for ix: int in range(d.w):
			var c: Vector3 = Vector3(d.origin.x + (float(ix) + 0.5) * d.cell, 0.0, d.origin.y + (float(iz) + 0.5) * d.cell)
			if d.solid[iz * d.w + ix] == 0:
				floors.append(c)
				continue
			# Only walls that face a floor tile are drawn.
			var edge: bool = false
			for dz: int in range(-1, 2):
				for dx: int in range(-1, 2):
					var x2: int = ix + dx
					var z2: int = iz + dz
					if x2 >= 0 and z2 >= 0 and x2 < d.w and z2 < d.h and d.solid[z2 * d.w + x2] == 0:
						edge = true
			if edge:
				walls.append(c)
	var floor_mesh: BoxMesh = BoxMesh.new()
	floor_mesh.size = Vector3(d.cell, 0.2, d.cell)
	_multimesh(parent, "Floor", floor_mesh, _material(z.color("ground", Color(0.29, 0.25, 0.22))), floors, -0.1)
	var wall_mesh: BoxMesh = BoxMesh.new()
	wall_mesh.size = Vector3(d.cell, WALL_HEIGHT, d.cell)
	_multimesh(parent, "Walls", wall_mesh, _material(z.color("wall", Color(0.35, 0.31, 0.29))), walls, WALL_HEIGHT * 0.5)
	# A brazier in every room (bigger fire in the boss room) lights the way.
	var brazier: PackedScene = _prop_scene("brazier")
	for i: int in range(d.rooms.size()):
		if brazier == null:
			break
		var rc: Vector2 = d.room_center(i)
		var r: Dictionary = d.rooms[i]
		var b: Node3D = brazier.instantiate() as Node3D
		# Against the room's north-west corner, out of the way of the middle.
		b.position = Vector3(d.origin.x + (float(r["x"]) + 0.8) * d.cell, 0.0, d.origin.y + (float(r["z"]) + 0.8) * d.cell)
		if i == d.boss:
			b.scale = Vector3(1.8, 1.8, 1.8)
			b.position = Vector3(rc.x, 0.0, rc.y - d.room_radius(i) * 0.8)
		parent.add_child(b)


static func _multimesh(parent: Node3D, label: String, mesh: Mesh, mat: Material, at: Array[Vector3], y: float) -> void:
	var mm: MultiMesh = MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = mesh
	mm.instance_count = at.size()
	for i: int in range(at.size()):
		mm.set_instance_transform(i, Transform3D(Basis(), Vector3(at[i].x, y, at[i].z)))
	var mmi: MultiMeshInstance3D = MultiMeshInstance3D.new()
	mmi.name = label
	mmi.multimesh = mm
	mmi.material_override = mat
	parent.add_child(mmi)


static func _zone_lines(parent: Node3D, z: ZoneData) -> void:
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.albedo_color = Color(0.55, 0.75, 1.0, 0.22)
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.emission_enabled = true
	mat.emission = Color(0.4, 0.6, 1.0)
	mat.emission_energy_multiplier = 0.8
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	for l: Variant in z.zone_lines():
		var ld: Dictionary = l as Dictionary
		var box: BoxMesh = BoxMesh.new()
		box.size = Vector3(float(ld.get("hx", 4.0)) * 2.0, 4.0, float(ld.get("hz", 4.0)) * 2.0)
		var mi: MeshInstance3D = MeshInstance3D.new()
		mi.name = "ZoneLine_" + str(ld.get("id", ""))
		mi.mesh = box
		mi.material_override = mat
		var x: float = float(ld["x"])
		var zz: float = float(ld["z"])
		mi.position = Vector3(x, z.geom.height(x, zz) + 2.0, zz)
		mi.rotation.y = float(ld.get("yaw", 0.0))
		mi.set_meta("to", str(ld.get("to", "")))
		parent.add_child(mi)


## The Proving Flats: grid, boundary posts every 40 m, danger rings every 100 m, a central brazier.
static func _test_ground(parent: Node3D, z: ZoneData) -> void:
	var size: Vector2 = z.bounds.size
	var ground_y: float = z.geom.base
	var plane: PlaneMesh = PlaneMesh.new()
	plane.size = size
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.albedo_texture = _grid_texture()
	mat.uv1_scale = Vector3(size.x / 4.0, size.y / 4.0, 1.0)
	mat.roughness = 1.0
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	var ground: MeshInstance3D = MeshInstance3D.new()
	ground.name = "Ground"
	ground.mesh = plane
	ground.material_override = mat
	ground.position = Vector3(z.bounds.get_center().x, ground_y, z.bounds.get_center().y)
	parent.add_child(ground)
	var post_mesh: BoxMesh = BoxMesh.new()
	post_mesh.size = Vector3(0.8, 3.0, 0.8)
	var post_mat: StandardMaterial3D = _material(Color(0.35, 0.3, 0.28))
	var min_x: float = z.bounds.position.x
	var min_z: float = z.bounds.position.y
	var max_x: float = z.bounds.end.x
	var max_z: float = z.bounds.end.y
	var x: float = min_x
	while x <= max_x:
		_post(parent, post_mesh, post_mat, Vector3(x, ground_y + 1.5, min_z))
		_post(parent, post_mesh, post_mat, Vector3(x, ground_y + 1.5, max_z))
		x += 40.0
	var zz: float = min_z + 40.0
	while zz < max_z:
		_post(parent, post_mesh, post_mat, Vector3(min_x, ground_y + 1.5, zz))
		_post(parent, post_mesh, post_mat, Vector3(max_x, ground_y + 1.5, zz))
		zz += 40.0
	var ring_mat: StandardMaterial3D = _material(Color(0.55, 0.32, 0.2))
	var ring_mesh: BoxMesh = BoxMesh.new()
	ring_mesh.size = Vector3(0.4, 1.2, 0.4)
	for ring: int in range(1, 10):
		var r: float = float(ring) * 100.0
		var n: int = 12 + ring * 6
		for i: int in range(n):
			var a: float = TAU * float(i) / float(n)
			_post(parent, ring_mesh, ring_mat, Vector3(cos(a) * r, ground_y + 0.6, sin(a) * r))
	var brazier: PackedScene = _prop_scene("brazier")
	if brazier != null:
		var b: Node3D = brazier.instantiate() as Node3D
		b.position = Vector3(0.0, ground_y, -8.0)
		b.scale = Vector3(1.6, 1.6, 1.6)
		parent.add_child(b)


static func _grid_texture() -> ImageTexture:
	var size: int = 64
	var img: Image = Image.create_empty(size, size, false, Image.FORMAT_RGB8)
	img.fill(Color(0.33, 0.36, 0.27))
	for i: int in range(size):
		for w: int in range(2):
			img.set_pixel(i, w, Color(0.25, 0.27, 0.2))
			img.set_pixel(w, i, Color(0.25, 0.27, 0.2))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


static func _post(parent: Node3D, mesh: Mesh, mat: Material, pos: Vector3) -> void:
	var m: MeshInstance3D = MeshInstance3D.new()
	m.mesh = mesh
	m.material_override = mat
	m.position = pos
	parent.add_child(m)
