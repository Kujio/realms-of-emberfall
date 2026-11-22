## Zone export tool: turns hand-authored zone scenes (res://zones/*.tscn) into the server's collision,
## markers and zone lines (content/zones/scenes/<zone>.json). The scene is the one source: the client
## draws it, and the server collides with what this exports (see docs/ZONES.md).
##
##   godot --headless --path client --script res://tools/export_zones.gd            # write
##   godot --headless --path client --script res://tools/export_zones.gd -- --check # exit 1 if stale
##
## What it reads:
##   StaticBody3D > CollisionShape3D with BoxShape3D      -> walls (centre, half extents, yaw)
##   StaticBody3D > CollisionShape3D with CylinderShape3D -> obstacles (centre, radius)
##   Area3D (metadata kind = "zone_line", id, to, entry) > CollisionShape3D with BoxShape3D -> zone_lines
##   Marker3D (metadata kind = "entry", id)                -> entries (position, yaw)
##   Marker3D (metadata kind = "npc", id)                  -> npcs (placement of an npcs.json NPC)
##   Marker3D (metadata kind = "spawn", mob, count, radius) -> spawns
## The scene root's metadata "zone" names the zone (default: the file name).
extends SceneTree

const SCENE_DIR: String = "res://zones"


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var check: bool = OS.get_cmdline_user_args().has("--check")
	var out_dir: String = ProjectSettings.globalize_path("res://").path_join("../content/zones/scenes").simplify_path()
	DirAccess.make_dir_recursive_absolute(out_dir)
	var stale: PackedStringArray = PackedStringArray()
	var count: int = 0
	var files: PackedStringArray = DirAccess.get_files_at(SCENE_DIR)
	files.sort()
	for f: String in files:
		if not f.ends_with(".tscn"):
			continue
		var ps: PackedScene = load(SCENE_DIR.path_join(f)) as PackedScene
		if ps == null:
			printerr("cannot load %s" % f)
			quit(2)
			return
		var root: Node3D = ps.instantiate() as Node3D
		if root == null:
			printerr("%s: the root is not a Node3D" % f)
			quit(2)
			return
		get_root().add_child(root)
		var zone_id: String = str(root.get_meta("zone", f.get_basename()))
		var text: String = _export(root, zone_id, SCENE_DIR.path_join(f))
		root.queue_free()
		var path: String = out_dir.path_join(zone_id + ".json")
		count += 1
		if check:
			var old: String = FileAccess.get_file_as_string(path)
			if old != text:
				stale.append(zone_id)
		else:
			var fa: FileAccess = FileAccess.open(path, FileAccess.WRITE)
			if fa == null:
				printerr("cannot write %s" % path)
				quit(2)
				return
			fa.store_string(text)
			fa.close()
	if check and not stale.is_empty():
		printerr("stale zone scene exports (run the zone export tool): %s" % ", ".join(stale))
		quit(1)
		return
	print("%s %d zone scene(s)" % ["checked" if check else "exported", count])
	quit(0)


static func _n(v: float) -> String:
	var s: String = "%.2f" % v
	if s == "-0.00":
		s = "0.00"
	return s


static func _yaw(b: Basis) -> String:
	var s: String = "%.4f" % atan2(-b.x.z, b.x.x)
	if s == "-0.0000":
		s = "0.0000"
	return s


func _export(root: Node3D, zone_id: String, source: String) -> String:
	var walls: PackedStringArray = PackedStringArray()
	var obstacles: PackedStringArray = PackedStringArray()
	var lines: PackedStringArray = PackedStringArray()
	var entries: PackedStringArray = PackedStringArray()
	var npcs: PackedStringArray = PackedStringArray()
	var spawns: PackedStringArray = PackedStringArray()
	var stack: Array[Node] = [root]
	var ordered: Array[Node] = []
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		ordered.append(n)
		var kids: Array[Node] = n.get_children()
		kids.reverse()
		for k: Node in kids:
			stack.append(k)
	for n: Node in ordered:
		if n is CollisionShape3D and n.get_parent() is StaticBody3D:
			var cs: CollisionShape3D = n
			var t: Transform3D = cs.global_transform
			if cs.shape is BoxShape3D:
				var size: Vector3 = (cs.shape as BoxShape3D).size
				walls.append('{"x": %s, "z": %s, "hx": %s, "hz": %s, "yaw": %s}' % [_n(t.origin.x), _n(t.origin.z),
					_n(size.x * 0.5 * t.basis.x.length()), _n(size.z * 0.5 * t.basis.z.length()), _yaw(t.basis)])
			elif cs.shape is CylinderShape3D:
				var r: float = (cs.shape as CylinderShape3D).radius * maxf(t.basis.x.length(), t.basis.z.length())
				obstacles.append('{"x": %s, "z": %s, "radius": %s}' % [_n(t.origin.x), _n(t.origin.z), _n(r)])
		elif n is Area3D and str(n.get_meta("kind", "")) == "zone_line":
			for c: Node in n.get_children():
				if c is CollisionShape3D and (c as CollisionShape3D).shape is BoxShape3D:
					var cs2: CollisionShape3D = c
					var t2: Transform3D = cs2.global_transform
					var sz: Vector3 = (cs2.shape as BoxShape3D).size
					lines.append('{"id": "%s", "x": %s, "z": %s, "hx": %s, "hz": %s, "yaw": %s, "to": "%s", "entry": "%s"}' % [
						str(n.get_meta("id", n.name)), _n(t2.origin.x), _n(t2.origin.z), _n(sz.x * 0.5 * t2.basis.x.length()),
						_n(sz.z * 0.5 * t2.basis.z.length()), _yaw(t2.basis), str(n.get_meta("to", "")), str(n.get_meta("entry", ""))])
		elif n is Marker3D:
			var m: Marker3D = n
			var g: Transform3D = m.global_transform
			match str(m.get_meta("kind", "")):
				"entry":
					entries.append('{"id": "%s", "x": %s, "z": %s, "yaw": %s}' % [str(m.get_meta("id", m.name)), _n(g.origin.x), _n(g.origin.z), _yaw(g.basis)])
				"npc":
					npcs.append('{"id": "%s", "x": %s, "z": %s, "yaw": %s}' % [str(m.get_meta("id", "")), _n(g.origin.x), _n(g.origin.z), _yaw(g.basis)])
				"spawn":
					spawns.append('{"mob": "%s", "x": %s, "z": %s, "count": %d, "radius": %s}' % [str(m.get_meta("mob", "")), _n(g.origin.x),
						_n(g.origin.z), int(m.get_meta("count", 1)), _n(float(m.get_meta("radius", 0.0)))])
	var out: PackedStringArray = PackedStringArray()
	out.append("{")
	out.append('  "_comment": "Exported from %s by client/tools/export_zones.gd. Do not edit; edit the scene and re-export.",' % source)
	out.append('  "zone": "%s",' % zone_id)
	out.append('  "source": "%s",' % source)
	var sections: Array = [["walls", walls], ["obstacles", obstacles], ["zone_lines", lines], ["entries", entries], ["npcs", npcs], ["spawns", spawns]]
	for i: int in range(sections.size()):
		var key: String = sections[i][0]
		var rows: PackedStringArray = sections[i][1]
		var tail: String = "," if i < sections.size() - 1 else ""
		if rows.is_empty():
			out.append('  "%s": []%s' % [key, tail])
			continue
		out.append('  "%s": [' % key)
		for j: int in range(rows.size()):
			out.append("    " + rows[j] + ("," if j < rows.size() - 1 else ""))
		out.append("  ]" + tail)
	out.append("}")
	return "\n".join(out) + "\n"
