## The world's surfaces (Phase 11): textured ground, water, the sky and dungeon walls and floors,
## built from a zone's theme (content/zones/<id>.json) and its biome's "ground", "sky" and "dungeon"
## settings (content/environments.json). Textures are detail maps from tools/gen_textures.py, read as
## a factor on a content colour, so the content keeps its palette. Everything here is visual: the
## server never sees it, and nothing collides with it (the skirt past a zone's edge cannot be reached;
## water does not slow or float anyone).
extends RefCounted

const ZoneData := preload("res://scripts/world/zone_data.gd")
const EnvironmentArt := preload("res://scripts/world/environment_art.gd")

const TEXTURE_DIR: String = "res://assets/textures"
const TERRAIN_SHADER: Shader = preload("res://shaders/terrain.gdshader")
const WATER_SHADER: Shader = preload("res://shaders/water.gdshader")
const SKY_SHADER: Shader = preload("res://shaders/sky.gdshader")
const DUNGEON_SHADER: Shader = preload("res://shaders/dungeon.gdshader")
## Theme and layer colours come out much lighter on screen than their swatches (sky light on flat
## ground); every surface colour is darkened by this much, as the plain ground was before Phase 11.
const DARKEN: float = 0.4
## How far the drawn ground reaches past a zone's edge (metres): out of reach, but it fills the view
## up to the fog, rising into hills, or sinking under the sea on a zone's sea sides.
const SKIRT: float = 300.0
## How far past the edge the ground takes to sink under the sea, and how far under it goes.
const SEA_SHELF: float = 120.0
const SEA_DEPTH: float = 5.0
const SIDES: Array[String] = ["-x", "+x", "-z", "+z"]

static var _textures: Dictionary = {}


static func texture(tex_name: String) -> Texture2D:
	if _textures.has(tex_name):
		return _textures[tex_name] as Texture2D
	var path: String = TEXTURE_DIR.path_join(tex_name + ".png")
	var t: Texture2D = load(path) as Texture2D if ResourceLoader.exists(path) else null
	if t == null:
		push_warning("surfaces: missing texture %s" % path)
	_textures[tex_name] = t
	return t


## A biome's settings block ("ground", "sky" or "dungeon") over the defaults' block of the same name.
static func block(z: ZoneData, key: String) -> Dictionary:
	var d: Dictionary = EnvironmentArt.data()
	var out: Dictionary = ((d.get("default", {}) as Dictionary).get(key, {}) as Dictionary).duplicate()
	var b: Variant = (d.get("biomes", {}) as Dictionary).get(EnvironmentArt.biome_of(z))
	if b is Dictionary and (b as Dictionary).get(key) is Dictionary:
		out.merge((b as Dictionary)[key] as Dictionary, true)
	return out


static func _tint(v: Variant, fallback: Color) -> Color:
	return EnvironmentArt.color_of(v, fallback).darkened(DARKEN)


## The zone's water: {} when it has none, else level, color, opacity and its sea sides.
static func water(z: ZoneData) -> Dictionary:
	var w: Variant = z.theme.get("water")
	if not (w is Dictionary) or not ((w as Dictionary).get("level") is float or (w as Dictionary).get("level") is int):
		return {}
	var wd: Dictionary = w as Dictionary
	var sea: Array[String] = []
	for s: Variant in wd.get("sea", []):
		if SIDES.has(str(s)):
			sea.append(str(s))
	return {"level": float(wd["level"]), "color": EnvironmentArt.color_of(wd.get("color"), Color(0.16, 0.24, 0.26)),
		"opacity": clampf(float(wd.get("opacity", 0.72)), 0.0, 1.0), "sea": sea}


## Ground height anywhere: the zone's own terrain inside its bounds, and past an edge the skirt,
## which rises by up to `rise` metres over SKIRT, or sinks under `sea_level` on a sea side.
static func skirt_height(z: ZoneData, x: float, zz: float, rise: float, sea: Array[String], sea_level: float) -> float:
	var b: Rect2 = z.bounds
	var cx: float = clampf(x, b.position.x, b.end.x)
	var cz: float = clampf(zz, b.position.y, b.end.y)
	var edge: float = z.geom.height(cx, cz)
	var out_x: float = maxf(b.position.x - x, x - b.end.x)
	var out_z: float = maxf(b.position.y - zz, zz - b.end.y)
	var d: float = maxf(maxf(out_x, out_z), 0.0)
	if d <= 0.0:
		return edge
	var side: String
	if out_x >= out_z:
		side = "-x" if x < b.position.x else "+x"
	else:
		side = "-z" if zz < b.position.y else "+z"
	if sea.has(side):
		var t: float = smoothstep(0.0, SEA_SHELF, d)
		return lerpf(edge, sea_level - SEA_DEPTH, t)
	# Two slow waves vary the far hills, so the horizon is not one even wall.
	var n: float = 0.5 + 0.25 * (sin(x * 0.013 + zz * 0.007) + sin(zz * 0.011 - x * 0.005 + 1.3))
	return edge + rise * pow(clampf(d / SKIRT, 0.0, 1.0), 1.3) * (0.45 + 0.75 * n)


## The textured ground for a zone: its theme's ground (and high) colours with the biome's layers.
## base_y and top place the high layer (top = 0 leaves it out, as on flat ground).
static func ground_material(z: ZoneData, base_y: float, top: float) -> ShaderMaterial:
	var g: Dictionary = block(z, "ground")
	var m: ShaderMaterial = ShaderMaterial.new()
	m.shader = TERRAIN_SHADER
	var ground: Color = z.color("ground", Color(0.36, 0.48, 0.25))
	var base_tint: Color = _tint(g.get("base_tint"), ground)
	var high: Color = z.color("high", ground.lerp(Color(0.5, 0.47, 0.42), 0.6))
	m.set_shader_parameter("base_tex", texture(str(g.get("base", "grass"))))
	m.set_shader_parameter("high_tex", texture(str(g.get("high", "rock"))))
	m.set_shader_parameter("slope_tex", texture(str(g.get("slope", "rock"))))
	m.set_shader_parameter("shore_tex", texture(str(g.get("shore", "mud"))))
	m.set_shader_parameter("macro_tex", texture("macro"))
	m.set_shader_parameter("base_tint", base_tint)
	m.set_shader_parameter("high_tint", _tint(g.get("high_tint"), high))
	m.set_shader_parameter("slope_tint", _tint(g.get("slope_tint"), Color(0.45, 0.43, 0.4)))
	m.set_shader_parameter("shore_tint", _tint(g.get("shore_tint"), Color(0.3, 0.27, 0.2)))
	m.set_shader_parameter("scale", maxf(0.5, float(g.get("scale", 5.0))))
	m.set_shader_parameter("base_y", base_y)
	m.set_shader_parameter("top", top if top > 0.0 else 100000.0)
	var w: Dictionary = water(z)
	m.set_shader_parameter("water_level", float(w.get("level", -10000.0)))
	m.set_shader_parameter("shore_band", maxf(0.05, float(g.get("shore_band", 0.8))))
	return m


static func water_material(w: Dictionary) -> ShaderMaterial:
	var m: ShaderMaterial = ShaderMaterial.new()
	m.shader = WATER_SHADER
	m.set_shader_parameter("color", w.get("color", Color(0.16, 0.24, 0.26)))
	m.set_shader_parameter("opacity", float(w.get("opacity", 0.72)))
	m.set_shader_parameter("normal_tex", texture("water_normal"))
	return m


## One sheet of water at the zone's level over the zone and its skirt: the terrain hides it except
## in hollows below the level, and past a sea side, where the skirt sinks under it.
static func add_water(parent: Node3D, z: ZoneData) -> void:
	var w: Dictionary = water(z)
	if w.is_empty():
		return
	var plane: PlaneMesh = PlaneMesh.new()
	plane.size = z.bounds.size + Vector2(SKIRT, SKIRT) * 2.0
	var mi: MeshInstance3D = MeshInstance3D.new()
	mi.name = "Water"
	mi.mesh = plane
	mi.material_override = water_material(w)
	mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mi.position = Vector3(z.bounds.get_center().x, float(w["level"]), z.bounds.get_center().y)
	parent.add_child(mi)


## The sky: the theme's sky colour overhead, its horizon below, clouds by the biome.
static func sky_material(z: ZoneData, horizon: Color) -> ShaderMaterial:
	var s: Dictionary = block(z, "sky")
	var m: ShaderMaterial = ShaderMaterial.new()
	m.shader = SKY_SHADER
	m.set_shader_parameter("top_color", z.color("sky", Color(0.29, 0.39, 0.6)))
	m.set_shader_parameter("horizon_color", horizon)
	m.set_shader_parameter("ground_color", horizon.darkened(0.55))
	m.set_shader_parameter("clouds_tex", texture("clouds"))
	m.set_shader_parameter("cover", clampf(float(s.get("cover", 0.35)), 0.0, 1.0))
	m.set_shader_parameter("cloud_color", EnvironmentArt.color_of(s.get("cloud"), Color(1.0, 0.98, 0.95)))
	m.set_shader_parameter("cloud_shade", EnvironmentArt.color_of(s.get("shade"), Color(0.62, 0.64, 0.7)))
	m.set_shader_parameter("sun_size", float(s.get("sun_size", 1.0)))
	return m


## Floor and wall materials for a dungeon's blocks: [floor, walls].
static func dungeon_materials(z: ZoneData) -> Array[ShaderMaterial]:
	var d: Dictionary = block(z, "dungeon")
	var floor_tint: Color = _tint(null, z.color("ground", Color(0.29, 0.25, 0.22)))
	var wall_tint: Color = _tint(null, z.color("wall", Color(0.35, 0.31, 0.29)))
	var floor_tex: Texture2D = texture(str(d.get("floor", "flagstone")))
	var wall_tex: Texture2D = texture(str(d.get("wall", "brick")))
	var floor_m: ShaderMaterial = _dungeon_material(floor_tex, floor_tex, floor_tint, floor_tint, d)
	# A wall's top face is seen from the camera above: the wall's stone, darker.
	var wall_m: ShaderMaterial = _dungeon_material(texture(str(d.get("wall_top", "cave_rock"))), wall_tex,
		wall_tint.darkened(0.35), wall_tint, d)
	return [floor_m, wall_m]


static func _dungeon_material(top: Texture2D, side: Texture2D, top_tint: Color, side_tint: Color, d: Dictionary) -> ShaderMaterial:
	var m: ShaderMaterial = ShaderMaterial.new()
	m.shader = DUNGEON_SHADER
	m.set_shader_parameter("top_tex", top)
	m.set_shader_parameter("side_tex", side)
	m.set_shader_parameter("top_tint", top_tint)
	m.set_shader_parameter("side_tint", side_tint)
	m.set_shader_parameter("top_scale", maxf(0.5, float(d.get("floor_scale", 4.0))))
	m.set_shader_parameter("side_scale", maxf(0.5, float(d.get("wall_scale", 3.0))))
	m.set_shader_parameter("roughness", clampf(float(d.get("roughness", 0.9)), 0.0, 1.0))
	return m
