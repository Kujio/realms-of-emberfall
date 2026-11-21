## The player's own character: samples input at the server tick rate, predicts movement with
## the shared step rule, sends sequenced intents, and reconciles against server snapshots.
extends Node3D

const Proto := preload("res://scripts/net/protocol.gd")
const Movement := preload("res://scripts/world/movement.gd")
const Avatar := preload("res://scripts/world/avatar.gd")

const TURN_SPEED: float = 2.6
const MOUSE_SENSITIVITY: float = 0.005
const MAX_STEPS_PER_FRAME: int = 5
const SMOOTH_SNAP_DISTANCE: float = 3.0

var input_enabled: bool = true
var autopilot: bool = false
var display_name: String = ""
var class_id: String = ""

var run_speed: float = 7.0
var tick_dt: float = 0.05
var bounds: Rect2 = Rect2()
var ground_y: float = 0.0

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


func setup(info: Dictionary, char_name: String, char_class: String) -> void:
	display_name = char_name
	class_id = char_class
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


func _ready() -> void:
	Avatar.build(self, Avatar.color_for_class(class_id), display_name, Color(1.0, 0.85, 0.35))


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


func _key(a: Key, b: Key) -> bool:
	return Input.is_physical_key_pressed(a) or Input.is_physical_key_pressed(b)


func _sim_step() -> void:
	var fwd: float = 0.0
	var strafe: float = 0.0
	var turn: float = 0.0
	if input_enabled:
		if _key(KEY_W, KEY_UP):
			fwd += 1.0
		if _key(KEY_S, KEY_DOWN):
			fwd -= 1.0
		if _key(KEY_A, KEY_LEFT):
			if mouselook:
				strafe -= 1.0
			else:
				turn += 1.0
		if _key(KEY_D, KEY_RIGHT):
			if mouselook:
				strafe += 1.0
			else:
				turn -= 1.0
		if Input.is_physical_key_pressed(KEY_Q):
			strafe -= 1.0
		if Input.is_physical_key_pressed(KEY_E):
			strafe += 1.0
	if autopilot:
		fwd = 1.0
		turn = 0.6
	yaw = wrapf(yaw + turn * TURN_SPEED * tick_dt + _mouse_yaw, -PI, PI)
	_mouse_yaw = 0.0

	var dir: Vector3 = Movement.forward(yaw) * fwd + Movement.right(yaw) * strafe
	if dir.length_squared() > 1.0:
		dir = dir.normalized()
	prev_sim_pos = sim_pos
	sim_pos = Movement.step(sim_pos, dir.x, dir.z, run_speed, tick_dt, bounds, ground_y)
	seq += 1
	pending.append({"seq": seq, "mx": dir.x, "mz": dir.z})
	if pending.size() > 256:
		pending.pop_front()
	Net.send(Proto.encode_move_input({
		"seq": seq, "move_x": dir.x, "move_z": dir.z, "yaw": yaw,
		"pred_x": sim_pos.x, "pred_y": sim_pos.y, "pred_z": sim_pos.z,
	}))


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
		p = Movement.step(p, float(inp["mx"]), float(inp["mz"]), run_speed, tick_dt, bounds, ground_y)
	var err: Vector3 = p - sim_pos
	if hard or err.length() > 0.01:
		if not hard and err.length() < SMOOTH_SNAP_DISTANCE:
			_visual_offset -= err
		else:
			_visual_offset = Vector3.ZERO
		sim_pos = p
		prev_sim_pos += err
