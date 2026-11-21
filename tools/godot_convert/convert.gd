## Converts imported model files (FBX, OBJ, ...) to GLB with Godot's own importer and glTF exporter, so
## third-party packs that ship without glTF can join the glTF pipeline.
## Run inside a scratch Godot project that contains the source files (see tools/convert_to_glb.sh):
##   godot --headless --path <project> --script res://convert.gd -- res://in.fbx /abs/out.glb [...]
## Also prints each model's clips and skeleton bones.
## Many FBX packs reference their textures by an absolute path from the author's machine, so the
## importer leaves materials untextured. For every material without an albedo texture, this binds
## a PNG next to the source file: <material>.png, then the same name without importer-added trailing
## digits, then *<material>.png, then <file stem>_Texture.png.
extends SceneTree


func _initialize() -> void:
	var args: PackedStringArray = OS.get_cmdline_user_args()
	var failed: int = 0
	if args.size() == 0 or args.size() % 2 != 0:
		printerr("usage: -- res://in.fbx /abs/out.glb [res://in2.fbx /abs/out2.glb ...]")
		quit(2)
		return
	for i: int in range(0, args.size(), 2):
		var root: Node = _instantiate(args[i])
		if root == null:
			printerr("cannot load %s as a scene or mesh" % args[i])
			failed += 1
			continue
		var bound: int = _bind_textures(root, args[i])
		var clips: Array[String] = []
		for p: Node in root.find_children("*", "AnimationPlayer", true, false):
			for a: StringName in (p as AnimationPlayer).get_animation_list():
				clips.append(str(a))
		var bones: Array[String] = []
		for s: Node in root.find_children("*", "Skeleton3D", true, false):
			for b: int in range((s as Skeleton3D).get_bone_count()):
				bones.append((s as Skeleton3D).get_bone_name(b))
		var doc: GLTFDocument = GLTFDocument.new()
		var state: GLTFState = GLTFState.new()
		var err: Error = doc.append_from_scene(root, state)
		if err == OK:
			err = doc.write_to_filesystem(state, args[i + 1])
		print("%s -> %s: %s; textures bound: %d; clips (%d): %s; bones (%d): %s" % [args[i], args[i + 1], error_string(err),
			bound, clips.size(), ", ".join(clips), bones.size(), ", ".join(bones)])
		if err != OK:
			failed += 1
		root.free()
	quit(1 if failed > 0 else 0)


## FBX and glTF import as scenes; a plain OBJ imports as a Mesh, which gets wrapped in a scene root.
func _instantiate(path: String) -> Node:
	var res: Resource = load(path)
	if res is PackedScene:
		return (res as PackedScene).instantiate()
	if res is Mesh:
		var root: Node3D = Node3D.new()
		root.name = path.get_file().get_basename()
		var mi: MeshInstance3D = MeshInstance3D.new()
		mi.name = root.name
		mi.mesh = res as Mesh
		root.add_child(mi)
		mi.owner = root
		return root
	return null


func _bind_textures(root: Node, source: String) -> int:
	var dir: String = source.get_base_dir()
	var stem: String = source.get_file().get_basename()
	var pngs: PackedStringArray = DirAccess.get_files_at(dir)
	var bound: int = 0
	var cache: Dictionary = {}
	var meshes: Array[Node] = root.find_children("*", "MeshInstance3D", true, false)
	if root is MeshInstance3D:
		meshes.push_front(root)
	for n: Node in meshes:
		var mesh: Mesh = (n as MeshInstance3D).mesh
		if mesh == null:
			continue
		for s: int in range(mesh.get_surface_count()):
			var m: BaseMaterial3D = mesh.surface_get_material(s) as BaseMaterial3D
			if m == null:
				m = StandardMaterial3D.new()
				mesh.surface_set_material(s, m)
			if m.albedo_texture != null:
				continue
			var pick: String = _pick_png(pngs, m.resource_name, stem)
			if pick == "":
				printerr("%s: no texture found for material '%s'" % [source, m.resource_name])
				continue
			if not cache.has(pick):
				var img: Image = Image.load_from_file(ProjectSettings.globalize_path(dir.path_join(pick)))
				cache[pick] = ImageTexture.create_from_image(img) if img != null else null
			if cache[pick] != null:
				m.albedo_texture = cache[pick]
				m.albedo_color = Color.WHITE
				bound += 1
	return bound


func _pick_png(pngs: PackedStringArray, material: String, stem: String) -> String:
	var names: Array[String] = []
	if material != "":
		names.append(material)
		var trimmed: String = material.rstrip("0123456789")
		if trimmed != "" and trimmed != material:
			names.append(trimmed)
	for want: String in names:
		if pngs.has(want + ".png"):
			return want + ".png"
	for want: String in names:
		for f: String in pngs:
			if f.ends_with(want + ".png"):
				return f
	if pngs.has(stem + "_Texture.png"):
		return stem + "_Texture.png"
	return ""
