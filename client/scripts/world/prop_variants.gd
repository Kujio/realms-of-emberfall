## Root of a generated prop scene (tools/world_props.py) that draws itself with art models. Its
## children named "V0", "V1", ... are alternative looks; on entering the tree the prop keeps one,
## picked from its position (so a forest of the same prop varies, and every client agrees), and frees
## the rest. A variant's "tint" metadata multiplies its colours. Models coloured per vertex (the
## Polyfork kits) get materials that use those colours, shared across all props. Meshes fade out
## past `draw_distance` (0 = always drawn); the graphics settings scale it.
extends Node3D

## Metres; set per prop by the generator (small clutter disappears sooner than buildings).
@export var draw_distance: float = 0.0

## Scales every prop's draw distance (the graphics quality setting).
static var distance_scale: float = 1.0
static var _materials: Dictionary = {}  # "<material id>:<tint>" -> Material


func _ready() -> void:
	var variants: Array[Node3D] = []
	for c: Node in get_children():
		if c is Node3D and str(c.name).begins_with("V") and str(c.name).substr(1).is_valid_int():
			variants.append(c as Node3D)
	if variants.is_empty():
		return
	var p: Vector3 = global_position if is_inside_tree() else position
	var h: int = absi(hash(Vector3i(roundi(p.x * 2.0), 0, roundi(p.z * 2.0))))
	var keep: Node3D = variants[h % variants.size()]
	for v: Node3D in variants:
		if v != keep:
			v.queue_free()
	keep.visible = true
	var tint: Color = keep.get_meta("tint") as Color if keep.has_meta("tint") else Color.WHITE
	dress(keep, tint, draw_distance)


## Gives every mesh under `root` vertex-coloured materials (times `tint`) and, when `draw_distance`
## is above 0, a fade-out range (scaled by the graphics setting). Also used for dungeon decorations.
static func dress(root: Node, tint: Color, draw_distance: float) -> void:
	var dist: float = draw_distance * distance_scale
	for node: Node in root.find_children("*", "MeshInstance3D", true, false):
		var mi: MeshInstance3D = node as MeshInstance3D
		if dist > 0.0:
			mi.visibility_range_end = dist
			mi.visibility_range_end_margin = dist * 0.1
			mi.visibility_range_fade_mode = GeometryInstance3D.VISIBILITY_RANGE_FADE_SELF
		if mi.mesh == null:
			continue
		for i: int in range(mi.mesh.get_surface_count()):
			var mat: BaseMaterial3D = mi.mesh.surface_get_material(i) as BaseMaterial3D
			if mat == null:
				continue
			mi.set_surface_override_material(i, _material(mat, tint))


static func _material(mat: BaseMaterial3D, tint: Color) -> Material:
	var key: String = "%d:%s" % [mat.get_instance_id(), tint.to_html()]
	if not _materials.has(key):
		var dup: BaseMaterial3D = mat.duplicate() as BaseMaterial3D
		dup.vertex_color_use_as_albedo = true
		dup.albedo_color = Color(mat.albedo_color.r * tint.r, mat.albedo_color.g * tint.g, mat.albedo_color.b * tint.b, mat.albedo_color.a)
		_materials[key] = dup
	return _materials[key] as Material
