## Measures models for content/models.json: the bone height span (for nameplate_height) and, per
## locomotion clip, the ground speed at which the feet stay planted (for ground_speed).
##   godot --headless --path <project> --script res://measure.gd -- res://model.glb foot_bone_l foot_bone_r [clip ...]
## Exit code: 0 when everything requested was measured, 1 when the model, a bone or a clip is missing.
##
## Foot positions are taken in a body frame that removes the root's position and rotation but keeps
## its scale, and only the component along the travel axis (the model's Z axis) counts. A sample is
## "planted" while the foot is within 3% of the rig's height of the lowest point either foot reaches.
extends SceneTree


func _initialize() -> void:
	_run.call_deferred()


func _run() -> void:
	var a: PackedStringArray = OS.get_cmdline_user_args()
	if a.size() < 3:
		printerr("usage: -- res://model.glb foot_l foot_r [clip ...]")
		quit(2)
		return
	var ps: PackedScene = load(a[0]) as PackedScene
	if ps == null:
		printerr("cannot load %s as a scene" % a[0])
		quit(1)
		return
	var root: Node3D = ps.instantiate() as Node3D
	if root == null:
		printerr("%s: the scene root is not a 3D node" % a[0])
		quit(1)
		return
	get_root().add_child(root)
	await process_frame
	var skels: Array[Node] = root.find_children("*", "Skeleton3D", true, false)
	if skels.is_empty():
		printerr("%s: no skeleton" % a[0])
		quit(1)
		return
	var skel: Skeleton3D = skels[0] as Skeleton3D
	var lo: float = 1e9
	var hi: float = -1e9
	for b: int in range(skel.get_bone_count()):
		var y: float = _body(root, skel.to_global(skel.get_bone_global_rest(b).origin)).y
		lo = minf(lo, y)
		hi = maxf(hi, y)
	print("%s: bones span y %.2f..%.2f m (top of head bone; add ~0.1 for the skull)" % [a[0], lo, hi])
	if a.size() == 3:
		quit(0)
		return
	var feet: Array[int] = [skel.find_bone(a[1]), skel.find_bone(a[2])]
	if feet.has(-1):
		var names: Array = range(skel.get_bone_count()).map(func(b: int) -> String: return skel.get_bone_name(b))
		printerr("  foot bones not found (%s, %s); bones: %s" % [a[1], a[2], ", ".join(names)])
		quit(1)
		return
	var players: Array[Node] = root.find_children("*", "AnimationPlayer", true, false)
	if players.is_empty():
		printerr("%s: no AnimationPlayer, so no clips to measure" % a[0])
		quit(1)
		return
	var player: AnimationPlayer = players[0] as AnimationPlayer
	var span: float = maxf(0.1, hi - lo)
	var missing: int = 0
	for i: int in range(3, a.size()):
		var clip: String = a[i]
		if not player.has_animation(clip):
			print("  %s: no such clip" % clip)
			missing += 1
			continue
		var anim: Animation = player.get_animation(clip)
		player.play(clip)
		var dt: float = 1.0 / 240.0
		var pts: Array = [[], []]
		var t: float = 0.0
		while t <= anim.length:
			player.seek(t, true)
			for k: int in range(2):
				(pts[k] as Array).append(_body(root, skel.to_global(skel.get_bone_global_pose(feet[k]).origin)))
			t += dt
		var ground: float = 1e9
		for k: int in range(2):
			for v: Vector3 in pts[k]:
				ground = minf(ground, v.y)
		# Contact method: travel-axis speed of a planted foot. Range method: the foot's travel-axis
		# extent over half the cycle (assumes ~50% stance).
		var speeds: Array[float] = []
		var extent: float = 0.0
		for k: int in range(2):
			var p: Array = pts[k]
			var min_z: float = 1e9
			var max_z: float = -1e9
			for v: Vector3 in p:
				min_z = minf(min_z, v.z)
				max_z = maxf(max_z, v.z)
			extent = maxf(extent, max_z - min_z)
			for j: int in range(1, p.size()):
				var v0: Vector3 = p[j - 1]
				var v1: Vector3 = p[j]
				if v0.y < ground + 0.03 * span and v1.y < ground + 0.03 * span:
					speeds.append(absf(v1.z - v0.z) / dt)
		speeds.sort()
		var med: float = speeds[speeds.size() / 2] if speeds.size() > 0 else 0.0
		print("  %s: length %.2f s, contact speed %.2f m/s (%d samples), stride-range speed %.2f m/s" % [
			clip, anim.length, med, speeds.size(), 2.0 * extent / maxf(0.01, anim.length)])
	quit(1 if missing > 0 else 0)


## A world point in the body frame: the root's position and rotation removed, its scale kept.
func _body(root: Node3D, world: Vector3) -> Vector3:
	var g: Transform3D = root.global_transform
	return g.basis.orthonormalized().inverse() * (world - g.origin)
