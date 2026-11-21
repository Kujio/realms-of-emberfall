## In-world scene: environment, ground, players, camera, HUD, chat, and the in-game menu.
extends Node3D

const Proto := preload("res://scripts/net/protocol.gd")
const LocalPlayer := preload("res://scripts/world/local_player.gd")
const RemotePlayer := preload("res://scripts/world/remote_player.gd")
const CameraRig := preload("res://scripts/world/camera_rig.gd")
const ChatWindow := preload("res://scripts/ui/chat_window.gd")
const UI := preload("res://scripts/ui/ui.gd")

## Remote players are drawn this far behind the newest snapshot so there are always two to blend.
const INTERP_DELAY: float = 0.1

var info: Dictionary = {}
var local_player: LocalPlayer
var remotes: Dictionary = {}  # entity_id -> RemotePlayer
var chat: ChatWindow
var snapshots_received: int = 0
var corrections_received: int = 0
var seen_names: Dictionary = {}  # name -> true, everyone ever spawned

var _tick_rate: int = 20
var _server_time: float = 0.0
var _has_time: bool = false
var _last_usec: int = 0
var _hud: Label
var _menu: PanelContainer


func _ready() -> void:
	info = Session.world_info
	_tick_rate = maxi(1, int(info["tick_rate"]))
	_server_time = float(info["server_tick"]) / float(_tick_rate)
	_build_environment()
	_build_ground()

	var me: Dictionary = _my_character()
	local_player = LocalPlayer.new()
	local_player.name = "LocalPlayer"
	local_player.setup(info, str(me.get("name", Session.username)), str(me.get("class_id", "")))
	add_child(local_player)

	var rig: CameraRig = CameraRig.new()
	rig.target = local_player
	add_child(rig)

	_build_hud()
	chat.my_name = local_player.display_name
	Session.world_message.connect(_on_world_message)


func _my_character() -> Dictionary:
	# EnterWorldResult does not repeat the name/class, so look it up from the character list.
	for c: Dictionary in Session.characters:
		if int(c["id"]) == Session.entering_char_id:
			return c
	return Session.characters[0] if Session.characters.size() > 0 else {}


func _process(_delta: float) -> void:
	var now: int = Time.get_ticks_usec()
	if _last_usec != 0:
		_server_time += float(now - _last_usec) / 1000000.0
	_last_usec = now
	var render_time: float = _server_time - INTERP_DELAY
	for r: RemotePlayer in remotes.values():
		r.update_render(render_time)
	local_player.input_enabled = not chat.is_typing() and not _menu.visible
	_hud.text = "%s   pos %.1f, %.1f   players nearby: %d   ping: %d ms   fps: %d" % [
		str(info["zone_name"]), local_player.position.x, local_player.position.z, remotes.size(),
		Net.round_trip_ms(), Engine.get_frames_per_second()]


func _unhandled_input(event: InputEvent) -> void:
	var k: InputEventKey = event as InputEventKey
	if k == null or not k.pressed or k.echo:
		return
	if (k.keycode == KEY_ENTER or k.keycode == KEY_KP_ENTER) and not chat.is_typing() and not _menu.visible:
		chat.focus_input()
		get_viewport().set_input_as_handled()
	elif k.keycode == KEY_ESCAPE:
		_menu.visible = not _menu.visible
		get_viewport().set_input_as_handled()


func _on_world_message(msg: Dictionary) -> void:
	var id: int = msg["_id"]
	match id:
		Proto.ENTITY_SPAWN:
			var eid: int = int(msg["entity_id"])
			if remotes.has(eid):
				return
			var r: RemotePlayer = RemotePlayer.new()
			r.setup(msg, _server_time)
			r.name = "Remote_%d" % eid
			add_child(r)
			remotes[eid] = r
			seen_names[str(msg["name"])] = true
		Proto.ENTITY_DESPAWN:
			var eid: int = int(msg["entity_id"])
			if remotes.has(eid):
				var r: RemotePlayer = remotes[eid]
				r.queue_free()
				remotes.erase(eid)
		Proto.SNAPSHOT:
			snapshots_received += 1
			var t: float = float(msg["server_tick"]) / float(_tick_rate)
			if not _has_time or absf(_server_time - t) > 0.25:
				_server_time = t
				_has_time = true
			else:
				_server_time = lerpf(_server_time, t, 0.05)
			local_player.reconcile(int(msg["ack_seq"]), Vector3(float(msg["self_x"]), float(msg["self_y"]), float(msg["self_z"])), false)
			for e: Dictionary in msg["entities"]:
				var eid: int = int(e["entity_id"])
				if remotes.has(eid):
					var r: RemotePlayer = remotes[eid]
					r.push_state(t, Vector3(float(e["x"]), float(e["y"]), float(e["z"])), float(e["yaw"]))
		Proto.MOVE_CORRECTION:
			corrections_received += 1
			local_player.reconcile(int(msg["ack_seq"]), Vector3(float(msg["x"]), float(msg["y"]), float(msg["z"])), true)
		Proto.CHAT_MESSAGE:
			chat.add_message(int(msg["channel"]), str(msg["from_name"]), str(msg["text"]))


# ------------------------------------------------------------------------------ construction

func _build_environment() -> void:
	var sky_mat: ProceduralSkyMaterial = ProceduralSkyMaterial.new()
	sky_mat.sky_top_color = Color(0.29, 0.39, 0.6)
	sky_mat.sky_horizon_color = Color(0.78, 0.63, 0.52)
	sky_mat.ground_horizon_color = Color(0.45, 0.38, 0.32)
	sky_mat.ground_bottom_color = Color(0.2, 0.17, 0.15)
	var sky: Sky = Sky.new()
	sky.sky_material = sky_mat
	var env: Environment = Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_energy = 0.7
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.fog_enabled = true
	env.fog_light_color = Color(0.7, 0.6, 0.55)
	env.fog_density = 0.0035
	var we: WorldEnvironment = WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var sun: DirectionalLight3D = DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-52.0, -35.0, 0.0)
	sun.light_energy = 1.1
	sun.light_color = Color(1.0, 0.93, 0.82)
	sun.shadow_enabled = true
	add_child(sun)


func _grid_texture() -> ImageTexture:
	var size: int = 64
	var img: Image = Image.create_empty(size, size, false, Image.FORMAT_RGB8)
	img.fill(Color(0.33, 0.36, 0.27))
	for i: int in range(size):
		for w: int in range(2):
			img.set_pixel(i, w, Color(0.25, 0.27, 0.2))
			img.set_pixel(w, i, Color(0.25, 0.27, 0.2))
	img.generate_mipmaps()
	return ImageTexture.create_from_image(img)


func _build_ground() -> void:
	var min_x: float = float(info["min_x"])
	var min_z: float = float(info["min_z"])
	var max_x: float = float(info["max_x"])
	var max_z: float = float(info["max_z"])
	var ground_y: float = float(info["y"])
	var size: Vector2 = Vector2(max_x - min_x, max_z - min_z)

	var plane: PlaneMesh = PlaneMesh.new()
	plane.size = size
	var mat: StandardMaterial3D = StandardMaterial3D.new()
	mat.albedo_texture = _grid_texture()
	mat.uv1_scale = Vector3(size.x / 4.0, size.y / 4.0, 1.0)
	mat.roughness = 1.0
	mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	var ground: MeshInstance3D = MeshInstance3D.new()
	ground.mesh = plane
	ground.material_override = mat
	ground.position = Vector3((min_x + max_x) * 0.5, ground_y, (min_z + max_z) * 0.5)
	add_child(ground)

	# Boundary posts every 20 m so the zone edge is visible.
	var post_mesh: BoxMesh = BoxMesh.new()
	post_mesh.size = Vector3(0.8, 3.0, 0.8)
	var post_mat: StandardMaterial3D = StandardMaterial3D.new()
	post_mat.albedo_color = Color(0.35, 0.3, 0.28)
	var x: float = min_x
	while x <= max_x:
		_post(post_mesh, post_mat, Vector3(x, ground_y + 1.5, min_z))
		_post(post_mesh, post_mat, Vector3(x, ground_y + 1.5, max_z))
		x += 20.0
	var z: float = min_z + 20.0
	while z < max_z:
		_post(post_mesh, post_mat, Vector3(min_x, ground_y + 1.5, z))
		_post(post_mesh, post_mat, Vector3(max_x, ground_y + 1.5, z))
		z += 20.0

	# A central ember brazier as a landmark.
	var base: MeshInstance3D = MeshInstance3D.new()
	var cyl: CylinderMesh = CylinderMesh.new()
	cyl.top_radius = 1.2
	cyl.bottom_radius = 1.5
	cyl.height = 1.0
	base.mesh = cyl
	base.position = Vector3(0.0, ground_y + 0.5, -8.0)
	base.material_override = post_mat
	add_child(base)
	var ember: MeshInstance3D = MeshInstance3D.new()
	var sphere: SphereMesh = SphereMesh.new()
	sphere.radius = 0.8
	sphere.height = 1.2
	ember.mesh = sphere
	var ember_mat: StandardMaterial3D = StandardMaterial3D.new()
	ember_mat.albedo_color = Color(1.0, 0.45, 0.15)
	ember_mat.emission_enabled = true
	ember_mat.emission = Color(1.0, 0.4, 0.1)
	ember_mat.emission_energy_multiplier = 2.0
	ember.material_override = ember_mat
	ember.position = Vector3(0.0, ground_y + 1.3, -8.0)
	add_child(ember)
	var light: OmniLight3D = OmniLight3D.new()
	light.light_color = Color(1.0, 0.55, 0.25)
	light.light_energy = 2.0
	light.omni_range = 12.0
	light.position = Vector3(0.0, ground_y + 2.5, -8.0)
	add_child(light)


func _post(mesh: Mesh, mat: Material, pos: Vector3) -> void:
	var m: MeshInstance3D = MeshInstance3D.new()
	m.mesh = mesh
	m.material_override = mat
	m.position = pos
	add_child(m)


func _build_hud() -> void:
	var layer: CanvasLayer = CanvasLayer.new()
	add_child(layer)
	var root: Control = Control.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(root)

	_hud = Label.new()
	_hud.position = Vector2(12, 8)
	_hud.add_theme_color_override("font_outline_color", Color.BLACK)
	_hud.add_theme_constant_override("outline_size", 6)
	root.add_child(_hud)

	var help: Label = Label.new()
	help.text = "W/S move   A/D turn   Q/E strafe   Right-drag mouselook   Wheel zoom   Enter chat   Esc menu"
	help.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	help.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	help.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	help.grow_vertical = Control.GROW_DIRECTION_BEGIN
	help.offset_right = -12
	help.offset_bottom = -12
	help.add_theme_color_override("font_outline_color", Color.BLACK)
	help.add_theme_constant_override("outline_size", 6)
	root.add_child(help)

	chat = ChatWindow.new()
	chat.anchor_left = 0.0
	chat.anchor_right = 0.0
	chat.anchor_top = 1.0
	chat.anchor_bottom = 1.0
	chat.offset_left = 12
	chat.offset_right = 472
	chat.offset_top = -252
	chat.offset_bottom = -12
	root.add_child(chat)

	_menu = PanelContainer.new()
	_menu.visible = false
	_menu.set_anchors_preset(Control.PRESET_CENTER)
	_menu.grow_horizontal = Control.GROW_DIRECTION_BOTH
	_menu.grow_vertical = Control.GROW_DIRECTION_BOTH
	root.add_child(_menu)
	var box: VBoxContainer = VBoxContainer.new()
	box.custom_minimum_size = Vector2(280, 0)
	box.add_theme_constant_override("separation", 8)
	_menu.add_child(box)
	UI.title(box, "Menu", 22)
	UI.button(box, "Resume", func() -> void: _menu.visible = false)
	UI.button(box, "Camp (character select)", func() -> void: Session.logout())
	UI.button(box, "Log out", func() -> void: Session.quit_session("Logged out."))
	UI.button(box, "Exit game", _exit_game)


func _exit_game() -> void:
	Session.quit_session("")
	get_tree().quit()
