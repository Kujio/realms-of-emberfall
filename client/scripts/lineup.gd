## Characters face -Z (the game's forward), so the camera looks at them from -Z.
## Art check without a server: `EmberfallClient.exe -- --lineup DIR` lines up every race and gender in
## each armour type (cloth, leather, chain, plate, by a class that wears it), the plate tiers with and
## without a helm, and close-ups of held weapons and shields, saves a screenshot of each into DIR and
## quits. Used to review character models and tune attachment points (docs/MODELS.md).
## With --zones (and optionally --zone ID) it instead builds every zone offline and photographs it;
## with --effects it photographs the spell effects (effects.json) on a row of casters and targets.
extends Node3D

const CharacterVisual := preload("res://scripts/world/character_visual.gd")
const ZoneData := preload("res://scripts/world/zone_data.gd")
const ZoneBuilder := preload("res://scripts/world/zone_builder.gd")
const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const TIMEOUT_S: float = 120.0
const ZONES_TIMEOUT_S: float = 900.0
const OUTFITS: Array = [["cloth", "wizard"], ["leather", "druid"], ["chain", "ranger"], ["plate", "warrior"]]
const TIERS: Array = ["plate_t1_chest", "plate_t3_chest", "plate_t5_chest", "plate_t7_chest", "plate_t8_chest"]

var _dir: String = ""
var _camera: Camera3D
var _row: Node3D
var _failed: int = 0


func _ready() -> void:
	var args: PackedStringArray = OS.get_cmdline_user_args()
	var i: int = args.find("--lineup")
	_dir = args[i + 1] if i >= 0 and i + 1 < args.size() else "user://lineup"
	DirAccess.make_dir_recursive_absolute(_dir)
	if not DirAccess.dir_exists_absolute(_dir):
		printerr("lineup: cannot create %s" % _dir)
		get_tree().quit(1)
		return
	# Never hang: quit even if something below stalls.
	get_tree().create_timer(ZONES_TIMEOUT_S if OS.get_cmdline_user_args().has("--zones") else TIMEOUT_S).timeout.connect(func() -> void:
		push_error("lineup: timed out")
		get_tree().quit(1))
	get_window().size = Vector2i(1920, 1080)
	_camera = Camera3D.new()
	_camera.fov = 40.0
	add_child(_camera)
	if OS.get_cmdline_user_args().has("--zones"):
		_run.call_deferred()  # each zone brings its own sky, light and ground
		return
	var env: WorldEnvironment = WorldEnvironment.new()
	env.environment = Environment.new()
	env.environment.background_mode = Environment.BG_COLOR
	env.environment.background_color = Color(0.32, 0.36, 0.42)
	env.environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.environment.ambient_light_color = Color(0.75, 0.75, 0.8)
	env.environment.ambient_light_energy = 0.6
	add_child(env)
	var sun: DirectionalLight3D = DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	sun.shadow_enabled = true
	add_child(sun)
	var ground: MeshInstance3D = MeshInstance3D.new()
	var plane: PlaneMesh = PlaneMesh.new()
	plane.size = Vector2(80.0, 40.0)
	ground.mesh = plane
	var gm: StandardMaterial3D = StandardMaterial3D.new()
	gm.albedo_color = Color(0.36, 0.42, 0.3)
	ground.material_override = gm
	add_child(ground)
	_run.call_deferred()


func _run() -> void:
	if OS.get_cmdline_user_args().has("--zones"):
		await _zones()
		return
	if OS.get_cmdline_user_args().has("--effects"):
		await _effects()
		return
	var races: Array = (VisualLibrary.data().get("races", {}) as Dictionary).keys()
	races.sort()
	for o: Array in OUTFITS:
		for g: int in [0, 1]:
			var looks: Array = []
			for race: Variant in races:
				looks.append(["%s %s" % [str(race), "F" if g == 0 else "M"], VisualLibrary.player_look(str(race), g, str(o[1]))])
			await _shot(looks, "lineup_%s_%s.png" % [str(o[0]), "female" if g == 0 else "male"], 1.3, Vector3(0.0, 1.0, -10.5))
	var tiers: Array = []
	# The same model (the female Highlander knight has a helmet) twice per tier: bare-headed, then helmed.
	for helm: bool in [false, true]:
		for chest: String in TIERS:
			var eq: Dictionary = {"main_hand": "longsword_t3", "off_hand": "buckler_t1", "ranged": "", "chest": chest, "head": "plate_t1_head" if helm else ""}
			tiers.append([chest + ("+helm" if helm else ""), VisualLibrary.player_look("highlander", 0, "warrior", eq)])
	await _shot(tiers, "tiers.png", 1.3, Vector3(0.0, 1.1, -10.5))
	# Held items, close: sword and shield, a two-hander, a bow on the back, a staff, on a tall and a small race.
	var held: Array = []
	for race: String in ["highlander", "deepkin", "tinkerling", "morrok"]:
		held.append([race + " sword+shield", VisualLibrary.player_look(race, 1, "warrior", {"main_hand": "longsword_t3", "off_hand": "buckler_t1", "ranged": "", "chest": "", "head": ""})])
		held.append([race + " greatsword+bow", VisualLibrary.player_look(race, 1, "warrior", {"main_hand": "greatsword_t3", "off_hand": "", "ranged": "shortbow_t1", "chest": "", "head": ""})])
		held.append([race + " staff", VisualLibrary.player_look(race, 1, "wizard", {"main_hand": "staff_t3", "off_hand": "", "ranged": "", "chest": "", "head": ""})])
	# Mob, pet and NPC models (content/models.json ids starting "pf_"), eight to a picture.
	var ids: Array = (VisualLibrary.data().get("models", {}) as Dictionary).keys().filter(func(m: Variant) -> bool: return str(m).begins_with("pf_"))
	ids.sort()
	for start: int in range(0, ids.size(), 8):
		var row: Array = []
		for m: Variant in ids.slice(start, start + 8):
			row.append([str(m).trim_prefix("pf_"), VisualLibrary.mob_look(str(m))])
		await _shot(row, "mobs_%d.png" % (start / 8 + 1), 1.6, Vector3(0.0, 1.2, -12.0))
	# Twelve characters 1.2 m apart span 13.2 m: back far enough that the outer ones and their labels fit.
	await _shot(held, "held_front.png", 1.2, Vector3(0.0, 1.0, -14.0))
	await _shot(held, "held_side.png", 1.2, Vector3(15.0, 1.0, 0.0), true)
	if _failed > 0:
		printerr("lineup: %d picture(s) could not be saved; the set in %s is incomplete" % [_failed, ProjectSettings.globalize_path(_dir)])
		get_tree().quit(1)
		return
	print("lineup: wrote %s" % ProjectSettings.globalize_path(_dir))
	get_tree().quit()


## `--effects`: casters fire at a row of targets; pictures mid-flight, on impact, and of the lasting
## states and ground effects. effects_<n>.png.
func _effects() -> void:
	const Effects := preload("res://scripts/world/effects.gd")
	var fx: Effects = Effects.new()
	add_child(fx)
	var casters: Array[CharacterVisual] = []
	var targets: Array[CharacterVisual] = []
	for k: int in range(6):
		var c: CharacterVisual = CharacterVisual.new()
		c.build(VisualLibrary.player_look("highlander", k % 2, "wizard"), 1.0)
		c.position = Vector3(7.5 - 3.0 * float(k), 0.0, 6.0)
		c.rotation.y = PI
		add_child(c)
		casters.append(c)
		var t: CharacterVisual = CharacterVisual.new()
		t.build(VisualLibrary.mob_look(["pf_skeleton_warrior", "pf_wolf", "pf_orc_brute", "pf_ghoul", "pf_tarantula", "pf_dark_knight"][k]), 1.0)
		t.position = Vector3(7.5 - 3.0 * float(k), 0.0, -2.0)
		add_child(t)
		targets.append(t)
	_camera.position = Vector3(0.0, 6.5, -14.0)
	_camera.look_at(Vector3(0.0, 1.0, 2.0))
	_camera.fov = 55.0
	for f: int in range(10):
		for v: CharacterVisual in casters + targets:
			v.step(0.05, 0.0, false, 0.0)
		await get_tree().process_frame
	# 1: casting motes, then bolts in flight.
	var schools: Array[int] = [0, 1, 2, 3, 4, 5]
	for k: int in range(6):
		fx.cast(casters[k], schools[k], "", true)
	await get_tree().create_timer(0.6).timeout
	await _save("effects_1_casting.png")
	for k: int in range(6):
		fx.cast(casters[k], 0, "", false)
		fx.play_event("spell_hit", casters[k], targets[k], schools[k], "Ember Dart" if k == 2 else "Frost Needle")
	await get_tree().create_timer(0.2).timeout
	await _save("effects_2_bolts.png")
	await get_tree().create_timer(0.35).timeout
	await _save("effects_3_impacts.png")
	# 4: heals, buffs, a life drain, a song, a dispel, a summon.
	fx.play_event("heal", casters[0], casters[0], 0, "")
	fx.play_event("buff", casters[1], casters[1], 0, "")
	fx.play_event("lifetap", casters[2], targets[2], 1, "")
	fx.play_event("song_pulse", casters[3], casters[3], 0, "")
	fx.play_event("dispel", casters[4], targets[4], 0, "")
	fx.play_event("summon", casters[5], casters[5], 0, "")
	await get_tree().create_timer(0.3).timeout
	await _save("effects_4_support.png")
	# 5: lasting states and the light columns.
	var states: Array[String] = ["stunned", "mezzed", "rooted", "fleeing", "charmed", ""]
	for k: int in range(5):
		fx.set_states(targets[k], {states[k]: true})
	fx.spawn("level_up", casters[0], 0)
	fx.spawn("resurrect", casters[2], 0)
	fx.spawn("teleport", casters[4], 0)
	await get_tree().create_timer(0.8).timeout
	await _save("effects_5_states_columns.png")
	if _failed > 0:
		get_tree().quit(1)
		return
	print("lineup: wrote %s" % ProjectSettings.globalize_path(_dir))
	get_tree().quit()


## `--zones`: every zone built as the client builds it (no server), seen from its safe spawn looking
## into the zone, and for dungeons from above the first rooms. One picture per zone: zone_<id>.png.
func _zones() -> void:
	var ids: Array[String] = []
	for f: String in DirAccess.get_files_at("res://data/zones"):
		if f.ends_with(".json"):
			ids.append(f.get_basename())
	ids.sort()
	var only: String = ""
	var args: PackedStringArray = OS.get_cmdline_user_args()
	var i: int = args.find("--zone")
	if i >= 0 and i + 1 < args.size():
		only = args[i + 1]
		if not ids.has(only):
			printerr("lineup: no zone '%s' in res://data/zones" % only)
			get_tree().quit(1)
			return
	for zid: String in ids:
		if only != "" and zid != only:
			continue
		var z: ZoneData = ZoneData.new()
		if not z.load_file(zid):
			printerr("lineup: cannot load zone %s" % zid)
			_failed += 1
			continue
		var root: Node3D = Node3D.new()
		add_child(root)
		ZoneBuilder.build(root, z)
		var sp: Dictionary = z.doc.get("safe_spawn", {}) as Dictionary
		var at: Vector3 = Vector3(float(sp.get("x", 0.0)), 0.0, float(sp.get("z", 0.0)))
		if z.kind == "dungeon" and z.dungeon != null and not z.dungeon.rooms.is_empty():
			# Over the boss room (the entrance room holds the zone line's gateway).
			var rc: Vector2 = z.dungeon.room_center(z.dungeon.boss)
			at = Vector3(rc.x, 0.0, rc.y)
			_camera.position = at + Vector3(0.0, 14.0, 14.0)
			_camera.look_at(at)
		else:
			# Up above the spawn, looking toward the middle of the zone.
			at.y = z.geom.height(at.x, at.z)
			var mid: Vector2 = z.bounds.get_center()
			var fwd: Vector3 = Vector3(mid.x - at.x, 0.0, mid.y - at.z)
			fwd = fwd.normalized() if fwd.length() > 1.0 else Vector3(0.0, 0.0, -1.0)
			_camera.position = at + Vector3(0.0, 25.0 if z.kind != "hub" else 8.0, 0.0) - fwd * 10.0
			_camera.look_at(at + fwd * 120.0)
		_camera.fov = 70.0
		_camera.far = 900.0
		for f: int in range(20):
			await get_tree().process_frame
		await _save("zone_%s.png" % zid)
		root.queue_free()
		await get_tree().process_frame
	if _failed > 0:
		get_tree().quit(1)
		return
	print("lineup: wrote %s" % ProjectSettings.globalize_path(_dir))
	get_tree().quit()


func _save(file: String) -> void:
	if DisplayServer.get_name() == "headless":
		print("lineup: %s skipped: dry run without a renderer, no picture saved" % file)
		return
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	var path: String = _dir.path_join(file)
	var err: Error = img.save_png(path)
	print("lineup: %s (%s)" % [path, error_string(err)])
	if err != OK:
		_failed += 1


## Builds the row of looks (label, look), lets them settle into their idle pose, and saves a screenshot.
func _shot(looks: Array, file: String, spacing: float, cam: Vector3, side: bool = false) -> void:
	if _row != null:
		_row.queue_free()
	_row = Node3D.new()
	add_child(_row)
	var visuals: Array[CharacterVisual] = []
	var width: float = spacing * float(looks.size() - 1)
	for k: int in range(looks.size()):
		var cv: CharacterVisual = CharacterVisual.new()
		cv.build(looks[k][1] as Dictionary, 1.0)
		# Seen from -Z, +X is the screen's left: lay the row out from +X so it reads left to right.
		cv.position = Vector3(width / 2.0 - spacing * float(k), 0.0, 0.0)
		_row.add_child(cv)
		visuals.append(cv)
		var tag: Label3D = Label3D.new()
		tag.text = str(looks[k][0])
		tag.font_size = 24
		tag.pixel_size = 0.004
		tag.position = cv.position + Vector3(0.0, 0.08 if not side else 2.3, -0.45)
		tag.no_depth_test = true  # readable even where the ground would hide it
		tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		tag.visible = not side  # the side view re-lays the row out below; the front view names them
		_row.add_child(tag)
	if side:
		# Seen from the right: the row runs away from the camera, so stagger it sideways.
		for k: int in range(visuals.size()):
			visuals[k].position = Vector3(0.0, 0.0, -width / 2.0 + spacing * float(k))
		_camera.position = cam
		_camera.look_at(Vector3(0.0, 1.0, 0.0))
		_camera.fov = 60.0
	else:
		_camera.position = cam
		_camera.look_at(Vector3(0.0, cam.y - 0.1, 0.0))
		_camera.fov = 40.0
	for f: int in range(30):
		for cv: CharacterVisual in visuals:
			cv.step(1.0 / 30.0, 0.0, false, 0.0)
		await get_tree().process_frame
	if DisplayServer.get_name() == "headless":
		# A dry run (scripts load, every look resolves); nothing is drawn, so nothing is saved.
		print("lineup: %s skipped: dry run without a renderer, no picture saved" % file)
		return
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	var path: String = _dir.path_join(file)
	var err: Error = img.save_png(path)
	print("lineup: %s (%s)" % [path, error_string(err)])
	if err != OK:
		_failed += 1
