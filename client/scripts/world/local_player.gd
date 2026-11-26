## The player's own character: samples input at the server tick rate, predicts movement with
## the shared step rule, sends sequenced intents, and reconciles against server snapshots.
extends Node3D

const Proto := preload("res://scripts/net/protocol.gd")
const Movement := preload("res://scripts/world/movement.gd")
const CharacterVisual := preload("res://scripts/world/character_visual.gd")
const VisualLibrary := preload("res://scripts/world/visual_library.gd")
const FACE_SECONDS: float = 4.0

const TURN_SPEED: float = 2.6
const MOUSE_SENSITIVITY: float = 0.005
const MAX_STEPS_PER_FRAME: int = 5
const SMOOTH_SNAP_DISTANCE: float = 3.0

var input_enabled: bool = true
## Dead or stunned: the server ignores movement, so prediction must too.
var frozen: bool = false
## Phase 15: fleeing in fear: we run along flee_yaw (the server's heading) and the keys do nothing.
var fleeing: bool = false
var flee_yaw: float = 0.0
## Autotest helpers: walk in a circle, or walk to goto_point and stop goto_stop metres short.
var autopilot: bool = false
var goto_active: bool = false
var goto_point: Vector3 = Vector3.ZERO
var goto_stop: float = 2.0
var display_name: String = ""
var class_id: String = ""
var race: String = ""
var gender: int = 0
## An illusion's race or model (empty = our own look).
var illusion_race: String = ""
var illusion_model: String = ""
var equipment: Variant = null  # {main_hand, off_hand, ranged} item ids worn; null until the first inventory
var visual: CharacterVisual

var run_speed: float = 7.0
var tick_dt: float = 0.05
var bounds: Rect2 = Rect2()
var ground_y: float = 0.0
## The zone's terrain and collision (Phase 6): prediction steps through the same rule as the server.
var geom: RefCounted = null  # ZoneGeom (zone_geom.gd)
const BODY_RADIUS: float = 0.4  # server zone.hpp kPlayerRadius

var sim_pos: Vector3 = Vector3.ZERO
var prev_sim_pos: Vector3 = Vector3.ZERO
var yaw: float = 0.0
var seq: int = 0
var last_ack: int = 0
var pending: Array[Dictionary] = []
var mouselook: bool = false

var _accum: float = 0.0
var _last_usec: int = 0
var _visual_offset: Vector3 = Vector3.ZERO
var _mouse_yaw: float = 0.0
var _speed: float = 0.0
var _backward: bool = false
var _face_target: WeakRef = null
var _face_until: float = 0.0
var _casting_at: WeakRef = null
var _now: float = 0.0
var _tag: Label3D


func setup(info: Dictionary, char_name: String, char_class: String, char_race: String = "", char_gender: int = 0) -> void:
	display_name = char_name
	class_id = char_class
	race = char_race
	gender = char_gender
	run_speed = float(info["run_speed"])
	tick_dt = 1.0 / float(maxi(1, int(info["tick_rate"])))
	var min_x: float = float(info["min_x"])
	var min_z: float = float(info["min_z"])
	bounds = Rect2(min_x, min_z, float(info["max_x"]) - min_x, float(info["max_z"]) - min_z)
	ground_y = float(info["y"])
	sim_pos = Vector3(float(info["x"]), float(info["y"]), float(info["z"]))
	prev_sim_pos = sim_pos
	yaw = float(info["yaw"])
	position = sim_pos
	rotation.y = yaw


func _look() -> Dictionary:
	if illusion_model != "":
		return VisualLibrary.mob_look(illusion_model)
	return VisualLibrary.player_look(illusion_race if illusion_race != "" else race, gender, class_id, equipment)


## What we hold follows what we wear (from our Inventory and EntityEquipment).
func set_equipment(held: Dictionary) -> void:
	if equipment is Dictionary and equipment == held:
		return
	equipment = held
	if visual != null:
		_rebuild_body()


func _ready() -> void:
	visual = CharacterVisual.new()
	visual.name = "Visual"
	visual.build(_look(), 1.0)
	add_child(visual)
	_tag = Label3D.new()
	_tag.text = display_name
	_tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	_tag.position = Vector3(0.0, visual.nameplate_height(), 0.0)
	_tag.font_size = 40
	_tag.outline_size = 10
	_tag.modulate = Color(1.0, 0.85, 0.35)
	_tag.no_depth_test = true
	add_child(_tag)


func _unhandled_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb != null and mb.button_index == MOUSE_BUTTON_RIGHT:
		mouselook = mb.pressed
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED if mouselook else Input.MOUSE_MODE_VISIBLE
	var mm: InputEventMouseMotion = event as InputEventMouseMotion
	if mm != null and mouselook:
		_mouse_yaw -= mm.relative.x * MOUSE_SENSITIVITY


func _process(_delta: float) -> void:
	# Pace the simulation from the wall clock, not the engine's (possibly smoothed) delta, so
	# the client always produces exactly one input per server tick of real time.
	var now: int = Time.get_ticks_usec()
	var delta: float = 0.0 if _last_usec == 0 else float(now - _last_usec) / 1000000.0
	_last_usec = now
	_accum += delta
	var steps: int = 0
	while _accum >= tick_dt and steps < MAX_STEPS_PER_FRAME:
		_accum -= tick_dt
		steps += 1
		_sim_step()
	if _accum > tick_dt:
		_accum = 0.0  # we fell far behind (hitch); drop the backlog instead of bursting inputs
	_visual_offset = _visual_offset.lerp(Vector3.ZERO, clampf(delta * 10.0, 0.0, 1.0))
	var alpha: float = clampf(_accum / tick_dt, 0.0, 1.0)
	position = prev_sim_pos.lerp(sim_pos, alpha) + _visual_offset
	rotation.y = yaw
	_animate(delta)


func _held(action: String) -> bool:
	return InputMap.has_action(action) and Input.is_action_pressed(action)


func _sim_step() -> void:
	var fwd: float = 0.0
	var strafe: float = 0.0
	var turn: float = 0.0
	if input_enabled:
		# Rebindable (Settings, Keys); the defaults are W/S/A/D, the arrows, and Q/E.
		if _held("move_forward"):
			fwd += 1.0
		if _held("move_back"):
			fwd -= 1.0
		if _held("turn_left"):
			if mouselook:
				strafe -= 1.0
			else:
				turn += 1.0
		if _held("turn_right"):
			if mouselook:
				strafe += 1.0
			else:
				turn -= 1.0
		if _held("strafe_left"):
			strafe -= 1.0
		if _held("strafe_right"):
			strafe += 1.0
	if autopilot:
		fwd = 1.0
		turn = 0.6
	if goto_active:
		var to: Vector3 = goto_point - sim_pos
		to.y = 0.0
		var dist: float = to.length()
		strafe = 0.0
		turn = 0.0
		fwd = 0.0
		if dist > 0.05:
			yaw = atan2(-to.x, -to.z)
		if dist > goto_stop + 0.05:
			fwd = minf(1.0, (dist - goto_stop) / (run_speed * tick_dt))
	if frozen:
		fwd = 0.0
		strafe = 0.0
		turn = 0.0
	if fleeing:
		fwd = 1.0
		strafe = 0.0
		turn = 0.0
		_mouse_yaw = 0.0
		yaw = flee_yaw
	yaw = wrapf(yaw + turn * TURN_SPEED * tick_dt + _mouse_yaw, -PI, PI)
	_mouse_yaw = 0.0

	var dir: Vector3 = Movement.forward(yaw) * fwd + Movement.right(yaw) * strafe
	if dir.length_squared() > 1.0:
		dir = dir.normalized()
	prev_sim_pos = sim_pos
	sim_pos = _step(sim_pos, dir.x, dir.z)
	seq += 1
	pending.append({"seq": seq, "mx": dir.x, "mz": dir.z})
	if pending.size() > 256:
		pending.pop_front()
	Net.send(Proto.encode_move_input({
		"seq": seq, "move_x": dir.x, "move_z": dir.z, "yaw": yaw,
		"pred_x": sim_pos.x, "pred_y": sim_pos.y, "pred_z": sim_pos.z,
	}))


func _step(p: Vector3, mx: float, mz: float) -> Vector3:
	if geom != null:
		return geom.call("step", p, mx, mz, run_speed, tick_dt, bounds, BODY_RADIUS) as Vector3
	return Movement.step(p, mx, mz, run_speed, tick_dt, bounds, ground_y)


## Rewind to the server's authoritative position at `ack`, replay unacknowledged inputs,
## and absorb any error into a decaying visual offset so the camera doesn't jump.
func reconcile(ack: int, server_pos: Vector3, hard: bool) -> void:
	if not hard and ack < last_ack:
		return
	last_ack = maxi(last_ack, ack)
	while pending.size() > 0 and int(pending[0]["seq"]) <= ack:
		pending.pop_front()
	var p: Vector3 = server_pos
	for inp: Dictionary in pending:
		p = _step(p, float(inp["mx"]), float(inp["mz"]))
	var err: Vector3 = p - sim_pos
	if hard or err.length() > 0.01:
		if not hard and err.length() < SMOOTH_SNAP_DISTANCE:
			_visual_offset -= err
		else:
			_visual_offset = Vector3.ZERO
		sim_pos = p
		prev_sim_pos += err


# ------------------------------------------------------------------------------ animation

## Server state for the animation machine (PlayerStats flags; world.gd calls this).
## Our own EntitySpawn (an illusion started or ended): rebuild the body if the look changed.
func apply_spawn_look(spawn: Dictionary) -> void:
	var new_race: String = str(spawn.get("race", race))
	var new_model: String = str(spawn.get("model", ""))
	var r: String = new_race if new_race != race else ""
	if r == illusion_race and new_model == illusion_model:
		return
	illusion_race = r
	illusion_model = new_model
	if visual == null:
		return
	_rebuild_body()


func _rebuild_body() -> void:
	var old: CharacterVisual = visual
	remove_child(old)
	visual = CharacterVisual.new()
	visual.name = "Visual"
	visual.build(_look(), 1.0)
	add_child(visual)
	visual.carry_state_from(old)  # a chest piece moved mid-cast keeps the cast going
	old.queue_free()
	move_child(visual, 0)
	if _tag != null:
		_tag.position = Vector3(0.0, visual.nameplate_height(), 0.0)


## Feigning lies us down; hiding draws us faintly.
func set_flags2(f2: int) -> void:
	feigned = (f2 & 1) != 0
	if visual != null:
		for n: Node in visual.find_children("*", "GeometryInstance3D", true, false):
			(n as GeometryInstance3D).transparency = 0.6 if (f2 & 2) != 0 else 0.0


var feigned: bool = false


func set_state_flags(dead: bool, sitting: bool, stunned: bool, in_combat: bool) -> void:
	if visual == null:
		return
	visual.anim.set_dead(dead or feigned, false)
	visual.anim.sitting = sitting
	visual.anim.stunned = stunned
	visual.anim.in_combat = in_combat


func on_attack(target: Node3D, ability: bool) -> void:
	_face(target)
	if visual != null:
		visual.anim.trigger_attack(2 if ability else 0)


func on_hit() -> void:
	if visual != null:
		visual.anim.trigger_hit()


func on_cast_start(target: Node3D) -> void:
	if visual != null:
		visual.anim.casting = true
	_casting_at = weakref(target) if target != null and target != self else null


## Phase 16: working at a crafting station (the combine's progress bar runs): the hands move as in a cast.
func set_working(on: bool) -> void:
	if visual != null:
		visual.anim.casting = on


func on_cast_end(success: bool, instant: bool) -> void:
	if visual != null:
		visual.anim.casting = false
		if success and instant:
			visual.anim.trigger_cast_release()
	var t: Node3D = _casting_at.get_ref() as Node3D if _casting_at != null else null
	if success and t != null:
		_face(t)
	_casting_at = null


func _face(target: Node3D) -> void:
	if target == null or target == self:
		return
	_face_target = weakref(target)
	_face_until = _now + FACE_SECONDS


func _face_yaw() -> float:
	var t: Node3D = _casting_at.get_ref() as Node3D if _casting_at != null else null
	if t == null and _face_target != null and _now < _face_until:
		t = _face_target.get_ref() as Node3D
	if t == null or not is_instance_valid(t):
		return NAN
	var d: Vector3 = t.global_position - global_position
	if Vector2(d.x, d.z).length() < 0.05:
		return NAN
	return atan2(-d.x, -d.z)


## Speed comes from the predicted simulation (one step per tick), not the smoothed render position.
func _animate(delta: float) -> void:
	_now += delta
	if visual == null or delta <= 0.0:
		return
	var v: Vector3 = (sim_pos - prev_sim_pos) / tick_dt
	v.y = 0.0
	var inst: float = v.length()
	if inst > 40.0:
		inst = 0.0
	_speed = lerpf(_speed, inst, 1.0 - exp(-delta * 12.0))
	_backward = inst > 0.3 and v.normalized().dot(Movement.forward(yaw)) < -0.3
	visual.step(delta, _speed, _backward, rotation.y, _face_yaw())
