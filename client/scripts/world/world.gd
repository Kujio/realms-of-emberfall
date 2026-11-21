## In-world scene: environment, entities (players, mobs, corpses), camera, combat HUD, chat with
## the combat log, targeting, and the in-game menu. Everything combat-related is an intent sent
## to the server; this script only presents what the server decides.
extends Node3D

const Proto := preload("res://scripts/net/protocol.gd")
const LocalPlayer := preload("res://scripts/world/local_player.gd")
const RemoteEntity := preload("res://scripts/world/remote_entity.gd")
const CameraRig := preload("res://scripts/world/camera_rig.gd")
const ChatWindow := preload("res://scripts/ui/chat_window.gd")
const Hud := preload("res://scripts/ui/hud.gd")
const UI := preload("res://scripts/ui/ui.gd")

## Remote entities are drawn this far behind the newest snapshot so there are always two to blend.
const INTERP_DELAY: float = 0.1
const TAB_RANGE: float = 60.0
const PICK_RADIUS_PX: float = 48.0
const FLAG_DEAD: int = 1
const FLAG_STUNNED: int = 8
const FLAG_SITTING: int = 4

# CombatEvent kinds (server/src/game/zone.hpp: CombatKind)
enum Ev { HIT, MISS, DODGE, PARRY, RIPOSTE, CRIT, ABILITY_HIT, ABILITY_MISS, TAUNT, TAUNT_FAIL, STUN, STUN_FAIL, HEAL, BUFF, BUFF_FADE, DEATH, EVADE }

var info: Dictionary = {}
var local_player: LocalPlayer
var remotes: Dictionary = {}  # entity_id -> RemoteEntity
var chat: ChatWindow
var hud: Hud
var snapshots_received: int = 0
var corrections_received: int = 0
var seen_names: Dictionary = {}  # player name -> true, everyone ever spawned
var my_id: int = 0
var my_level: int = 1
var stats: Dictionary = {}
var target_id: int = 0
var target_con: int = 3
var abilities: Array = []
# Counters the autotest reports.
var kills: int = 0
var deaths: int = 0
var corpses_looted: int = 0
var level_ups: int = 0
var combat_events: int = 0
var abilities_used: int = 0
var last_death: Dictionary = {}
var death_position: Vector3 = Vector3.ZERO
var last_loot_closed: String = ""

var _tick_rate: int = 20
var _server_time: float = 0.0
var _has_time: bool = false
var _last_usec: int = 0
var _hud_label: Label
var _menu: PanelContainer
var _camera_rig: CameraRig
var _tab_index: int = -1


func _ready() -> void:
	info = Session.world_info
	my_id = int(info["entity_id"])
	_tick_rate = maxi(1, int(info["tick_rate"]))
	_server_time = float(info["server_tick"]) / float(_tick_rate)
	_build_environment()
	_build_ground()

	var me: Dictionary = _my_character()
	my_level = int(me.get("level", 1))
	local_player = LocalPlayer.new()
	local_player.name = "LocalPlayer"
	local_player.setup(info, str(me.get("name", Session.username)), str(me.get("class_id", "")))
	add_child(local_player)

	_camera_rig = CameraRig.new()
	_camera_rig.target = local_player
	add_child(_camera_rig)

	_build_hud()
	chat.my_name = local_player.display_name
	hud.my_name = local_player.display_name
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
	for r: RemoteEntity in remotes.values():
		r.update_render(render_time)
	var flags: int = int(stats.get("flags", 0))
	local_player.frozen = (flags & (FLAG_DEAD | FLAG_STUNNED)) != 0
	local_player.input_enabled = not chat.is_typing() and not _menu.visible
	if target_id != 0 and remotes.has(target_id):
		hud.set_target_hp((remotes[target_id] as RemoteEntity).hp_pct)
	_hud_label.text = "%s   pos %.1f, %.1f   nearby: %d   ping: %d ms   fps: %d" % [
		str(info["zone_name"]), local_player.position.x, local_player.position.z, remotes.size(),
		Net.round_trip_ms(), Engine.get_frames_per_second()]


# ------------------------------------------------------------------------------ input

func _unhandled_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb != null and mb.pressed and mb.button_index == MOUSE_BUTTON_LEFT and not _menu.visible:
		var picked: int = _pick_at(mb.position)
		if picked != 0:
			set_target(picked)
			get_viewport().set_input_as_handled()
		return
	var k: InputEventKey = event as InputEventKey
	if k == null or not k.pressed or k.echo or chat.is_typing():
		return
	var handled: bool = true
	match k.keycode:
		KEY_ENTER, KEY_KP_ENTER:
			if not _menu.visible:
				chat.focus_input()
		KEY_SLASH:
			if not _menu.visible:
				chat.focus_input()
				handled = false  # let the "/" reach the input
		KEY_ESCAPE:
			if hud.any_window_open():
				hud.close_windows()
			elif target_id != 0:
				set_target(0)
			else:
				_menu.visible = not _menu.visible
		KEY_TAB:
			cycle_target(k.shift_pressed)
		KEY_F1:
			set_target(my_id)
		KEY_R:
			toggle_auto_attack()
		KEY_K:
			hud.toggle_abilities()
		KEY_I:
			hud.toggle_inventory()
		KEY_L:
			loot_target()
		KEY_C:
			consider()
		KEY_X:
			set_sitting(int(stats.get("flags", 0)) & FLAG_SITTING == 0)
		_:
			if k.keycode >= KEY_0 and k.keycode <= KEY_9 and not _menu.visible:
				var n: int = int(k.keycode - KEY_0)
				use_slot(9 if n == 0 else n - 1)
			else:
				handled = false
	if handled:
		get_viewport().set_input_as_handled()


func _pick_at(screen_pos: Vector2) -> int:
	var cam: Camera3D = get_viewport().get_camera_3d()
	if cam == null:
		return 0
	var best: int = 0
	var best_d: float = PICK_RADIUS_PX
	for r: RemoteEntity in remotes.values():
		var anchor: Vector3 = r.global_position + Vector3(0.0, 0.9 * r.scale_factor, 0.0)
		if cam.is_position_behind(anchor):
			continue
		var d: float = cam.unproject_position(anchor).distance_to(screen_pos)
		if d < best_d:
			best_d = d
			best = r.entity_id
	return best


# ------------------------------------------------------------------------------ intents (also used by the autotest)

func set_target(eid: int) -> void:
	if target_id != 0 and remotes.has(target_id):
		(remotes[target_id] as RemoteEntity).set_selected(false)
	target_id = eid
	if eid == 0:
		hud.clear_target()
	elif remotes.has(eid):
		(remotes[eid] as RemoteEntity).set_selected(true)
	Net.send(Proto.encode_set_target({"entity_id": eid}))


## Tab cycles living mobs by distance; Shift+Tab goes the other way.
func cycle_target(reverse: bool) -> void:
	var list: Array = []
	for r: RemoteEntity in remotes.values():
		if r.kind == RemoteEntity.KIND_MOB and r.is_alive():
			var d: float = r.position.distance_to(local_player.position)
			if d <= TAB_RANGE:
				list.append([d, r.entity_id])
	if list.is_empty():
		chat.add_system("No targets nearby.")
		return
	list.sort_custom(func(a: Array, b: Array) -> bool: return float(a[0]) < float(b[0]))
	var ids: Array = list.map(func(e: Array) -> int: return int(e[1]))
	var cur: int = ids.find(target_id)
	var next: int = 0 if cur < 0 else (cur + (-1 if reverse else 1) + ids.size()) % ids.size()
	set_target(int(ids[next]))


func toggle_auto_attack() -> void:
	Net.send(Proto.encode_auto_attack({"enabled": not bool(stats.get("auto_attack", false))}))


func use_slot(index: int) -> void:
	if index < 0 or index >= hud.hotbar_ids.size():
		return
	var id: String = hud.hotbar_ids[index]
	if id == Hud.ATTACK_ID:
		toggle_auto_attack()
	elif id != "":
		use_ability(id)


func use_ability(id: String) -> void:
	Net.send(Proto.encode_use_ability({"ability_id": id, "target_id": target_id}))


func loot_target() -> void:
	if target_id != 0 and remotes.has(target_id) and (remotes[target_id] as RemoteEntity).kind == RemoteEntity.KIND_CORPSE:
		Net.send(Proto.encode_loot_open({"corpse_id": target_id}))
	else:
		chat.add_system("Target a corpse to loot it.")


func loot_take(slot: int) -> void:
	if hud.loot_corpse != 0:
		Net.send(Proto.encode_loot_take({"corpse_id": hud.loot_corpse, "slot": slot}))


func loot_close() -> void:
	if hud.loot_corpse != 0:
		Net.send(Proto.encode_loot_close({"corpse_id": hud.loot_corpse}))
	hud.hide_loot()


func respawn() -> void:
	Net.send(Proto.encode_respawn_request())


func set_sitting(sitting: bool) -> void:
	Net.send(Proto.encode_set_sitting({"sitting": sitting}))


func consider() -> void:
	if target_id == 0 or not remotes.has(target_id):
		chat.add_system("You must have a target to consider.")
		return
	var r: RemoteEntity = remotes[target_id]
	var verdicts: Array[String] = [
		"looks like it would not be worth your time.", "looks like an easy fight.", "looks like a fair fight, in your favor.",
		"looks like an even fight.", "looks like quite a gamble.", "looks like it would wipe the floor with you!",
	]
	chat.add_system("%s (level %d) %s" % [_cap(r.display_name), r.level, verdicts[clampi(target_con, 0, 5)]])


func _on_command(cmd: String, args: String) -> void:
	match cmd:
		"sit":
			set_sitting(true)
		"stand":
			set_sitting(false)
		"loc":
			chat.add_system("Your location is %.1f, %.1f." % [local_player.position.x, local_player.position.z])
		"con", "consider":
			consider()
		"attack":
			toggle_auto_attack()
		"target":
			_target_by_name(args)
		_:
			chat.add_system("Unknown command /%s. Try /sit, /stand, /loc, /con, /attack, /target NAME." % cmd)


func _target_by_name(text: String) -> void:
	var needle: String = text.strip_edges().to_lower()
	if needle == "":
		return
	var best: int = 0
	var best_d: float = INF
	for r: RemoteEntity in remotes.values():
		if r.display_name.to_lower().contains(needle):
			var d: float = r.position.distance_to(local_player.position)
			if d < best_d:
				best_d = d
				best = r.entity_id
	if best != 0:
		set_target(best)
	else:
		chat.add_system("No '%s' nearby." % text)


# ------------------------------------------------------------------------------ server messages

func _on_world_message(msg: Dictionary) -> void:
	var id: int = msg["_id"]
	match id:
		Proto.ENTITY_SPAWN:
			var eid: int = int(msg["entity_id"])
			if remotes.has(eid):
				(remotes[eid] as RemoteEntity).refresh(msg)
				return
			var r: RemoteEntity = RemoteEntity.new()
			r.setup(msg, _server_time)
			r.name = "Entity_%d" % eid
			add_child(r)
			remotes[eid] = r
			if r.kind == RemoteEntity.KIND_PLAYER:
				seen_names[str(msg["name"])] = true
			if eid == target_id:
				r.set_selected(true)
		Proto.ENTITY_DESPAWN:
			var eid: int = int(msg["entity_id"])
			if remotes.has(eid):
				var r: RemoteEntity = remotes[eid]
				r.queue_free()
				remotes.erase(eid)
			if eid == target_id:
				target_id = 0
				hud.clear_target()
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
					var r: RemoteEntity = remotes[eid]
					r.push_state(t, Vector3(float(e["x"]), float(e["y"]), float(e["z"])), float(e["yaw"]), int(e["hp_pct"]), int(e["flags"]))
		Proto.MOVE_CORRECTION:
			corrections_received += 1
			local_player.reconcile(int(msg["ack_seq"]), Vector3(float(msg["x"]), float(msg["y"]), float(msg["z"])), true)
		Proto.CHAT_MESSAGE:
			chat.add_message(int(msg["channel"]), str(msg["from_name"]), str(msg["text"]))
		Proto.PLAYER_STATS:
			var old_level: int = my_level
			stats = msg
			my_level = int(msg["level"])
			if my_level > old_level:
				level_ups += 1
			hud.set_stats(msg)
		Proto.TARGET_INFO:
			_on_target_info(msg)
		Proto.COMBAT_EVENT:
			_on_combat_event(msg)
		Proto.ABILITY_LIST:
			abilities = msg["abilities"]
			hud.set_abilities(abilities, local_player.display_name)
		Proto.ABILITY_RESULT:
			hud.note_ability_result(msg)
			if bool(msg["ok"]):
				abilities_used += 1
			elif str(msg["message"]) != "":
				chat.add_combat(str(msg["message"]), Color(1.0, 0.55, 0.45))
		Proto.PLAYER_DIED:
			deaths += 1
			last_death = msg
			death_position = local_player.position
			hud.show_death(str(msg["killer"]), int(msg["xp_lost"]))
			set_target(0)
		Proto.LOOT_WINDOW:
			hud.show_loot(msg)
		Proto.LOOT_CLOSED:
			hud.hide_loot()
			last_loot_closed = str(msg["message"])
			if last_loot_closed != "":
				chat.add_system(last_loot_closed)
			else:
				corpses_looted += 1  # closed normally: everything we wanted is in the pack
		Proto.INVENTORY:
			hud.set_inventory(msg)
		Proto.SKILL_VALUES:
			if bool(msg["announce"]):
				for s: Dictionary in msg["skills"]:
					chat.add_system("You have become better at %s! (%d)" % [str(s["name"]), int(s["value"])])


func _on_target_info(msg: Dictionary) -> void:
	var eid: int = int(msg["entity_id"])
	if eid == 0:
		if target_id != 0 and remotes.has(target_id):
			(remotes[target_id] as RemoteEntity).set_selected(false)
		target_id = 0
		hud.clear_target()
		return
	target_id = eid
	target_con = int(msg["con"])
	var kind: int = 0
	if remotes.has(eid):
		kind = (remotes[eid] as RemoteEntity).kind
	hud.set_target(_cap(str(msg["name"])), int(msg["level"]), target_con, int(msg["hp_pct"]), kind)


func _name_of(eid: int, subject: bool) -> String:
	if eid == my_id:
		return "You" if subject else "YOU"
	if remotes.has(eid):
		return (remotes[eid] as RemoteEntity).display_name
	return "something"


static func _cap(s: String) -> String:
	return s if s.is_empty() else s.substr(0, 1).to_upper() + s.substr(1)


func _on_combat_event(e: Dictionary) -> void:
	combat_events += 1
	var src: int = int(e["source_id"])
	var tgt: int = int(e["target_id"])
	var kind: int = int(e["kind"])
	var amount: int = int(e["amount"])
	var ability: String = str(e["ability"])
	var mine: bool = src == my_id
	var at_me: bool = tgt == my_id
	var s: String = _name_of(src, true)
	var t: String = _name_of(tgt, false)
	var line: String = ""
	var color: Color = Color(0.8, 0.8, 0.8)
	match kind:
		Ev.HIT, Ev.CRIT:
			var verb: String = "hit" if mine else "hits"
			line = "%s %s %s for %d points of damage." % [s, verb, t, amount]
			if kind == Ev.CRIT:
				line = ("You score a critical hit! " if mine else "%s scores a critical hit! " % _cap(s)) + line
			color = Color(1.0, 1.0, 1.0) if mine else (Color(1.0, 0.45, 0.4) if at_me else Color(0.75, 0.75, 0.75))
			_float(tgt, str(amount) + ("!" if kind == Ev.CRIT else ""), Color(1.0, 0.95, 0.5) if kind == Ev.CRIT else (Color(1, 0.4, 0.35) if at_me else Color.WHITE), mine or at_me)
		Ev.MISS:
			line = ("You try to hit %s, but miss!" % t) if mine else ("%s tries to hit %s, but misses!" % [s, t])
			_float(tgt, "miss", Color(0.7, 0.7, 0.7), mine or at_me)
		Ev.DODGE, Ev.PARRY, Ev.RIPOSTE:
			var what: String = ["dodges", "parries", "ripostes"][kind - Ev.DODGE]
			var me_what: String = ["dodge", "parry", "riposte"][kind - Ev.DODGE]
			line = ("You %s %s's attack!" % [me_what, s]) if at_me else ("%s %s %s attack!" % [_cap(t), what, "your" if mine else s + "'s"])
			_float(tgt, me_what, Color(0.6, 0.85, 1.0), mine or at_me)
		Ev.ABILITY_HIT:
			line = "%s %s hits %s for %d points of damage." % ["Your" if mine else s + "'s", ability, t, amount]
			color = Color(1.0, 0.85, 0.4) if mine else Color(1.0, 0.45, 0.4)
			_float(tgt, str(amount), Color(1.0, 0.75, 0.3), mine or at_me)
		Ev.ABILITY_MISS:
			line = "%s %s misses %s." % ["Your" if mine else s + "'s", ability, t]
		Ev.TAUNT:
			line = ("You goad %s into attacking you with %s." % [t, ability]) if mine else ("%s taunts %s." % [s, t])
			color = Color(1.0, 0.7, 0.3)
		Ev.TAUNT_FAIL:
			line = "%s ignores %s %s." % [_cap(t), "your" if mine else s + "'s", ability]
		Ev.STUN:
			line = "%s is stunned!" % _cap(t)
			color = Color(1.0, 0.9, 0.5)
		Ev.STUN_FAIL:
			line = "%s shrugs off %s %s." % [_cap(t), "your" if mine else s + "'s", ability]
		Ev.HEAL:
			line = ("Your %s heals you for %d." % [ability, amount]) if mine else ("%s is healed for %d." % [s, amount])
			color = Color(0.5, 1.0, 0.5)
			_float(tgt, "+%d" % amount, Color(0.45, 1.0, 0.45), mine or at_me)
		Ev.BUFF:
			line = ("You feel the power of %s." % ability) if mine else ("%s uses %s." % [s, ability])
			color = Color(0.6, 0.8, 1.0)
		Ev.BUFF_FADE:
			line = "Your %s fades." % ability
			color = Color(0.6, 0.8, 1.0)
		Ev.DEATH:
			if at_me:
				line = "You have been slain by %s!" % s.to_lower() if not mine else "You died."
				color = Color(1.0, 0.3, 0.3)
			elif mine:
				line = "You have slain %s!" % t
				color = Color(1.0, 0.9, 0.4)
				kills += 1
			else:
				line = "%s has been slain by %s!" % [_cap(t), s]
		Ev.EVADE:
			line = "%s evades %s attack." % [_cap(t), "your" if mine else s + "'s"]
	if line != "":
		chat.add_combat(_cap(line), color)


## Floating combat text above an entity, only for hits that involve us.
func _float(eid: int, text: String, color: Color, involves_me: bool) -> void:
	if not involves_me:
		return
	var anchor: Node3D = local_player if eid == my_id else (remotes.get(eid) as Node3D)
	if anchor == null:
		return
	var label: Label3D = Label3D.new()
	label.text = text
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.no_depth_test = true
	label.font_size = 56
	label.outline_size = 12
	label.modulate = color
	label.position = anchor.global_position + Vector3(randf_range(-0.4, 0.4), 2.6, 0.0)
	add_child(label)
	var tw: Tween = create_tween()
	tw.set_parallel(true)
	tw.tween_property(label, "position:y", label.position.y + 1.6, 1.1)
	tw.tween_property(label, "modulate:a", 0.0, 1.1).set_delay(0.3)
	tw.chain().tween_callback(label.queue_free)


# ------------------------------------------------------------------------------ queries for the autotest

## Nearest entity of `kind` that passes `accept` (a Callable taking the RemoteEntity), or 0.
func nearest(kind: int, accept: Callable, max_distance: float = 80.0) -> int:
	var best: int = 0
	var best_d: float = max_distance
	for r: RemoteEntity in remotes.values():
		if r.kind != kind or not accept.call(r):
			continue
		var d: float = r.position.distance_to(local_player.position)
		if d < best_d:
			best_d = d
			best = r.entity_id
	return best


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

	# Boundary posts every 40 m so the zone edge is visible, and ring markers every 100 m from
	# the centre (danger rises outward).
	var post_mesh: BoxMesh = BoxMesh.new()
	post_mesh.size = Vector3(0.8, 3.0, 0.8)
	var post_mat: StandardMaterial3D = StandardMaterial3D.new()
	post_mat.albedo_color = Color(0.35, 0.3, 0.28)
	var x: float = min_x
	while x <= max_x:
		_post(post_mesh, post_mat, Vector3(x, ground_y + 1.5, min_z))
		_post(post_mesh, post_mat, Vector3(x, ground_y + 1.5, max_z))
		x += 40.0
	var z: float = min_z + 40.0
	while z < max_z:
		_post(post_mesh, post_mat, Vector3(min_x, ground_y + 1.5, z))
		_post(post_mesh, post_mat, Vector3(max_x, ground_y + 1.5, z))
		z += 40.0
	var ring_mat: StandardMaterial3D = StandardMaterial3D.new()
	ring_mat.albedo_color = Color(0.55, 0.32, 0.2)
	var ring_mesh: BoxMesh = BoxMesh.new()
	ring_mesh.size = Vector3(0.4, 1.2, 0.4)
	for ring: int in range(1, 10):
		var r: float = float(ring) * 100.0
		var n: int = 12 + ring * 6
		for i: int in range(n):
			var a: float = TAU * float(i) / float(n)
			_post(ring_mesh, ring_mat, Vector3(cos(a) * r, ground_y + 0.6, sin(a) * r))

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
	root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	layer.add_child(root)

	_hud_label = Label.new()
	_hud_label.position = Vector2(12, 6)
	_hud_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_hud_label.add_theme_constant_override("outline_size", 6)
	_hud_label.add_theme_font_size_override("font_size", 13)
	root.add_child(_hud_label)

	var help: Label = Label.new()
	help.text = "W/S move  A/D turn  Q/E strafe  Right-drag look  Tab/click target  R attack  1-0 hotbar  K abilities  I pack  L loot  C consider  X sit  Enter chat  Esc menu"
	help.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	help.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	help.grow_horizontal = Control.GROW_DIRECTION_BEGIN
	help.grow_vertical = Control.GROW_DIRECTION_BEGIN
	help.offset_right = -12
	help.offset_bottom = -4
	help.add_theme_font_size_override("font_size", 12)
	help.add_theme_color_override("font_outline_color", Color.BLACK)
	help.add_theme_constant_override("outline_size", 6)
	root.add_child(help)

	hud = Hud.new()
	root.add_child(hud)
	hud.slot_used.connect(use_slot)
	hud.ability_used.connect(use_ability)
	hud.loot_requested.connect(loot_target)
	hud.loot_take.connect(loot_take)
	hud.loot_close.connect(loot_close)
	hud.respawn_requested.connect(respawn)

	chat = ChatWindow.new()
	chat.anchor_left = 0.0
	chat.anchor_right = 0.0
	chat.anchor_top = 1.0
	chat.anchor_bottom = 1.0
	chat.offset_left = 12
	chat.offset_right = 392
	chat.offset_top = -262
	chat.offset_bottom = -22
	root.add_child(chat)
	chat.command.connect(_on_command)
	hud.notice.connect(chat.add_system)

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
