## Retargets humanoid animation clips from one or more source models onto a target model and writes the
## target, with only the retargeted clips, as a GLB. Used to give Polyfork characters (22-bone
## Mixamo-named rig) the CC0 Quaternius Universal Animation Library clips (65-bone Unreal-style rig).
## Run inside a scratch Godot project that contains the files (see tools/retarget_to_glb.sh):
##   godot --headless --path <project> --script res://retarget.gd -- <target.glb> <out.glb> <clips.json>
## clips.json: {"bone_map": {source bone: target bone}, "sources": [{"scene": "res://..", "reference": "A_TPose",
##   "clips": {"OutName": "SourceClip", ...}}], "fps": 30}
## Each clip is measured against the source's reference pose (a T-pose clip) and the target's rest pose
## (Polyfork binds in a strict T-pose), in world space, so differences in bone orientation, proportions and
## facing cancel out. Hip motion is scaled by the ratio of hip heights.
extends SceneTree


func _initialize() -> void:
	var args: PackedStringArray = OS.get_cmdline_user_args()
	if args.size() != 3:
		printerr("usage: -- <target.glb> <out.glb> <clips.json>")
		quit(2)
		return
	var spec: Variant = JSON.parse_string(FileAccess.get_file_as_string(args[2]))
	if not (spec is Dictionary):
		printerr("cannot read %s" % args[2])
		quit(2)
		return
	var err: int = _run(args[0], args[1], spec as Dictionary)
	quit(err)


func _instantiate(path: String) -> Node3D:
	var res: Resource = load(path)
	if res is PackedScene:
		return (res as PackedScene).instantiate() as Node3D
	return null


func _skeleton(root: Node) -> Skeleton3D:
	var found: Array[Node] = root.find_children("*", "Skeleton3D", true, false)
	return found[0] as Skeleton3D if not found.is_empty() else null


func _player(root: Node) -> AnimationPlayer:
	var found: Array[Node] = root.find_children("*", "AnimationPlayer", true, false)
	return found[0] as AnimationPlayer if not found.is_empty() else null


## The clip name as the player knows it ("Rig|Idle" or "Idle").
## Godot drops a "_Loop" suffix on import, so either spelling finds the clip.
func _find_clip(p: AnimationPlayer, name: String) -> String:
	var names: Array[String] = [name]
	if name.ends_with("_Loop"):
		names.append(name.trim_suffix("_Loop"))
	for c: StringName in p.get_animation_list():
		var s: String = str(c)
		for n: String in names:
			if s == n or s.ends_with("|" + n) or s.ends_with("/" + n):
				return s
	return ""


## Poses `skel` as `anim` at `t` (bones without tracks stay at rest).
func _apply(skel: Skeleton3D, root: Node, player: AnimationPlayer, anim: Animation, t: float) -> void:
	skel.reset_bone_poses()
	var base: Node = player.get_node(player.root_node)
	for i: int in anim.get_track_count():
		var path: NodePath = anim.track_get_path(i)
		if base.get_node_or_null(NodePath(str(path.get_concatenated_names()))) != skel:
			continue
		var bone: int = skel.find_bone(str(path.get_concatenated_subnames()))
		if bone < 0:
			continue
		match anim.track_get_type(i):
			Animation.TYPE_ROTATION_3D:
				skel.set_bone_pose_rotation(bone, anim.rotation_track_interpolate(i, t))
			Animation.TYPE_POSITION_3D:
				skel.set_bone_pose_position(bone, anim.position_track_interpolate(i, t))
			Animation.TYPE_SCALE_3D:
				skel.set_bone_pose_scale(bone, anim.scale_track_interpolate(i, t))
	skel.force_update_all_bone_transforms()


## The skeleton node's transform relative to its scene root, from the parent chain (the scene need not
## be in the tree, where global_transform is unavailable).
func _skel_xform(skel: Skeleton3D) -> Transform3D:
	var x: Transform3D = Transform3D.IDENTITY
	var n: Node = skel
	while n is Node3D and n.get_parent() is Node3D:
		x = (n as Node3D).transform * x
		n = n.get_parent()
	return x


## A bone's current pose relative to the scene root, built from local poses down the bone chain (a
## skeleton outside the scene tree never computes its global poses, so get_bone_global_pose() would
## return the rest pose).
func _world(skel: Skeleton3D, bone: int) -> Transform3D:
	var x: Transform3D = skel.get_bone_pose(bone)
	var p: int = skel.get_bone_parent(bone)
	while p >= 0:
		x = skel.get_bone_pose(p) * x
		p = skel.get_bone_parent(p)
	return _skel_xform(skel) * x


func _run(target_path: String, out_path: String, spec: Dictionary) -> int:
	var target: Node3D = _instantiate(target_path)
	if target == null:
		printerr("cannot load %s" % target_path)
		return 1
	get_root().add_child(target)
	var tskel: Skeleton3D = _skeleton(target)
	if tskel == null:
		printerr("%s has no skeleton" % target_path)
		return 1
	# glTF node names must be unique on export: Polyfork's skeleton hangs under a node named like its body
	# mesh ("body"), which would push the mesh to "body2". Rename the holder so meshes keep their names.
	var holder: Node = tskel.get_parent()
	if holder != target and not target.find_children(str(holder.name), "MeshInstance3D", true, false).is_empty():
		holder.name = str(holder.name) + "-rig"
	tskel.reset_bone_poses()
	tskel.force_update_all_bone_transforms()
	var fps: float = float(spec.get("fps", 30))
	var bone_map: Dictionary = spec["bone_map"]
	# Target reference (rest = T-pose), world space.
	var tref: Dictionary = {}
	for b: int in tskel.get_bone_count():
		tref[b] = _world(tskel, b)
	var thips: int = tskel.find_bone(str(bone_map.get("pelvis", "Hips")))
	# Target bones in parent-first order.
	var order: Array[int] = []
	var placed: Dictionary = {}
	while order.size() < tskel.get_bone_count():
		for b: int in tskel.get_bone_count():
			if placed.has(b):
				continue
			var parent: int = tskel.get_bone_parent(b)
			if parent < 0 or placed.has(parent):
				order.append(b)
				placed[b] = true

	var library: AnimationLibrary = AnimationLibrary.new()
	var made: Array[String] = []
	for src_spec: Dictionary in spec["sources"]:
		var scene_path: String = str(src_spec["scene"])
		if not scene_path.begins_with("res://"):
			scene_path = "res://src/" + scene_path
		var source: Node3D = _instantiate(scene_path)
		if source == null:
			printerr("cannot load %s" % src_spec["scene"])
			return 1
		get_root().add_child(source)
		var sskel: Skeleton3D = _skeleton(source)
		var splayer: AnimationPlayer = _player(source)
		if sskel == null or splayer == null:
			printerr("%s has no skeleton or clips" % src_spec["scene"])
			return 1
		# Source reference pose.
		var ref_name: String = _find_clip(splayer, str(src_spec.get("reference", "A_TPose")))
		if ref_name == "":
			printerr("%s: no reference clip %s" % [src_spec["scene"], src_spec.get("reference")])
			return 1
		_apply(sskel, source, splayer, splayer.get_animation(ref_name), 0.0)
		var sref: Dictionary = {}
		var pairs: Array = []  # [source bone, target bone]
		for s_name: String in bone_map.keys():
			var sb: int = sskel.find_bone(s_name)
			var tb: int = tskel.find_bone(str(bone_map[s_name]))
			if sb >= 0 and tb >= 0:
				pairs.append([sb, tb])
				sref[sb] = _world(sskel, sb)
		var ships: int = sskel.find_bone(bone_map.keys()[0] if not bone_map.has("pelvis") else "pelvis")
		# Facing: if the two characters' left hands lie on opposite sides, turn the source by 180 degrees.
		var conv: Basis = Basis.IDENTITY
		var s_lhand: int = sskel.find_bone("hand_l")
		var t_lhand: int = tskel.find_bone(str(bone_map.get("hand_l", "LeftHand")))
		if s_lhand >= 0 and t_lhand >= 0:
			var sx: float = (sref.get(s_lhand, _world(sskel, s_lhand)) as Transform3D).origin.x - (sref[ships] as Transform3D).origin.x
			var tx: float = (tref[t_lhand] as Transform3D).origin.x - (tref[thips] as Transform3D).origin.x
			if signf(sx) != signf(tx):
				conv = Basis(Vector3.UP, PI)
		var cq: Quaternion = conv.get_rotation_quaternion()
		var hip_scale: float = (tref[thips] as Transform3D).origin.y / maxf(0.01, (sref[ships] as Transform3D).origin.y)
		var tmap: Dictionary = {}  # target bone -> source bone
		for p: Array in pairs:
			tmap[p[1]] = p[0]

		var clips: Dictionary = src_spec["clips"]
		for out_name: String in clips.keys():
			var src_name: String = _find_clip(splayer, str(clips[out_name]))
			if src_name == "":
				printerr("%s: no clip %s (skipped)" % [src_spec["scene"], clips[out_name]])
				continue
			var src: Animation = splayer.get_animation(src_name)
			var anim: Animation = Animation.new()
			anim.length = src.length
			var rot_tracks: Dictionary = {}
			for b: int in order:
				var tr: int = anim.add_track(Animation.TYPE_ROTATION_3D)
				anim.track_set_path(tr, NodePath("%s:%s" % [target.get_path_to(tskel), tskel.get_bone_name(b)]))
				rot_tracks[b] = tr
			var pos_track: int = anim.add_track(Animation.TYPE_POSITION_3D)
			anim.track_set_path(pos_track, NodePath("%s:%s" % [target.get_path_to(tskel), tskel.get_bone_name(thips)]))
			var frames: int = maxi(1, int(ceil(src.length * fps)))
			for f: int in frames + 1:
				var t: float = minf(src.length, float(f) / fps)
				_apply(sskel, source, splayer, src, t)
				var world: Dictionary = {}  # target bone -> world basis this frame
				for b: int in order:
					var parent: int = tskel.get_bone_parent(b)
					var parent_world: Quaternion = (_skel_xform(tskel).basis.get_rotation_quaternion() if parent < 0 else world[parent])
					var w: Quaternion
					if tmap.has(b):
						var sb: int = tmap[b]
						var s_now: Quaternion = _world(sskel, sb).basis.get_rotation_quaternion()
						var s_ref: Quaternion = (sref[sb] as Transform3D).basis.get_rotation_quaternion()
						w = (cq * (s_now * s_ref.inverse()) * cq.inverse()) * (tref[b] as Transform3D).basis.get_rotation_quaternion()
					else:
						# Unmapped: keep its rest orientation relative to its parent.
						w = parent_world * tskel.get_bone_rest(b).basis.get_rotation_quaternion()
					world[b] = w
					var local: Quaternion = (parent_world.inverse() * w).normalized()
					anim.rotation_track_insert_key(rot_tracks[b], t, local)
				# Hips: the source's displacement from its reference, scaled and turned.
				var s_hip_now: Vector3 = _world(sskel, ships).origin
				var delta: Vector3 = conv * ((s_hip_now - (sref[ships] as Transform3D).origin) * hip_scale)
				var t_world: Vector3 = (tref[thips] as Transform3D).origin + delta
				var parent_t: Transform3D = _skel_xform(tskel)
				var hp: int = tskel.get_bone_parent(thips)
				if hp >= 0:
					parent_t = _world(tskel, hp)
				anim.position_track_insert_key(pos_track, t, parent_t.affine_inverse() * t_world)
			library.add_animation(StringName(out_name), anim)
			made.append(out_name)
		source.queue_free()

	# Replace the target's own clips with the retargeted ones.
	var tplayer: AnimationPlayer = _player(target)
	if tplayer == null:
		tplayer = AnimationPlayer.new()
		tplayer.name = "AnimationPlayer"
		target.add_child(tplayer)
		tplayer.owner = target
	for lib: StringName in tplayer.get_animation_library_list():
		tplayer.remove_animation_library(lib)
	tplayer.root_node = tplayer.get_path_to(target)
	tplayer.add_animation_library(&"", library)
	tskel.reset_bone_poses()
	var doc: GLTFDocument = GLTFDocument.new()
	var state: GLTFState = GLTFState.new()
	var err: Error = doc.append_from_scene(target, state)
	if err == OK:
		err = doc.write_to_filesystem(state, out_path)
	print("%s -> %s: %s; clips (%d): %s" % [target_path, out_path, error_string(err), made.size(), ", ".join(made)])
	return 0 if err == OK else 1
