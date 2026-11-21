## Another player, a mob, or a corpse. Moving entities render slightly in the past by
## interpolating buffered server snapshots.
extends Node3D

const Avatar := preload("res://scripts/world/avatar.gd")
const MAX_BUFFER: int = 40

const KIND_PLAYER: int = 0
const KIND_MOB: int = 1
const KIND_CORPSE: int = 2

const FLAG_DEAD: int = 1
const FLAG_IN_COMBAT: int = 2
const FLAG_SITTING: int = 4
const FLAG_STUNNED: int = 8

var entity_id: int = 0
var kind: int = KIND_PLAYER
var display_name: String = ""
var model: String = ""
var class_id: String = ""
var level: int = 1
var hp_pct: int = 100
var flags: int = 0
var scale_factor: float = 1.0
var _buffer: Array[Dictionary] = []
var _visual: Node3D
var _tag: Label3D


func setup(spawn: Dictionary, server_time: float) -> void:
	entity_id = int(spawn["entity_id"])
	_apply_spawn(spawn)
	var pos: Vector3 = Vector3(float(spawn["x"]), float(spawn["y"]), float(spawn["z"]))
	position = pos
	rotation.y = float(spawn["yaw"])
	push_state(server_time, pos, float(spawn["yaw"]), hp_pct, flags)


func _apply_spawn(spawn: Dictionary) -> void:
	kind = int(spawn["kind"])
	display_name = str(spawn["name"])
	model = str(spawn["race"])
	class_id = str(spawn["class_id"])
	level = int(spawn["level"])
	hp_pct = int(spawn["hp_pct"])
	flags = int(spawn["flags"])
	scale_factor = clampf(float(spawn["scale"]), 0.1, 10.0)


## A repeated EntitySpawn for a known id refreshes name/level/state without a respawn.
func refresh(spawn: Dictionary) -> void:
	var old_flags: int = flags
	_apply_spawn(spawn)
	if _tag != null:
		_tag.text = _tag_text()
	if (old_flags & FLAG_DEAD) != (flags & FLAG_DEAD):
		_update_pose()


func _ready() -> void:
	_visual = Node3D.new()
	add_child(_visual)
	match kind:
		KIND_MOB:
			Avatar.build_mob(_visual, model, scale_factor)
		KIND_CORPSE:
			Avatar.build_corpse(_visual, model, scale_factor)
		_:
			Avatar.build(_visual, Avatar.color_for_class(class_id), "", Color.WHITE)
	_tag = Label3D.new()
	_tag.text = _tag_text()
	_tag.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	var tag_y: float = 2.25
	if kind == KIND_MOB:
		tag_y = 1.1 + 1.3 * scale_factor
	elif kind == KIND_CORPSE:
		tag_y = 0.9
	_tag.position = Vector3(0.0, tag_y, 0.0)
	_tag.font_size = 36 if kind != KIND_CORPSE else 28
	_tag.outline_size = 10
	_tag.modulate = _tag_color()
	_tag.no_depth_test = true
	add_child(_tag)
	_update_pose()


func _tag_text() -> String:
	return display_name


func _tag_color() -> Color:
	match kind:
		KIND_MOB:
			return Color(1.0, 0.75, 0.6)
		KIND_CORPSE:
			return Color(0.7, 0.7, 0.7)
	return Color(0.85, 0.92, 1.0)


func _update_pose() -> void:
	if _visual == null:
		return
	# Dead players lie down until they respawn elsewhere.
	_visual.rotation.x = -PI / 2.0 if kind == KIND_PLAYER and (flags & FLAG_DEAD) != 0 else 0.0
	_visual.position.y = 0.4 if kind == KIND_PLAYER and (flags & FLAG_DEAD) != 0 else 0.0


func is_alive() -> bool:
	return kind != KIND_CORPSE and (flags & FLAG_DEAD) == 0


func set_selected(selected: bool) -> void:
	if _tag != null:
		_tag.modulate = Color(1.0, 1.0, 0.55) if selected else _tag_color()
		_tag.outline_modulate = Color(0.35, 0.2, 0.0) if selected else Color.BLACK


func push_state(t: float, pos: Vector3, yaw: float, hp: int, f: int) -> void:
	hp_pct = hp
	if f != flags:
		flags = f
		_update_pose()
	if _buffer.size() > 0 and t <= float(_buffer.back()["t"]):
		return
	_buffer.append({"t": t, "pos": pos, "yaw": yaw})
	if _buffer.size() > MAX_BUFFER:
		_buffer.pop_front()


func update_render(render_time: float) -> void:
	if _buffer.is_empty() or kind == KIND_CORPSE:
		return
	var first: Dictionary = _buffer[0]
	if render_time <= float(first["t"]):
		_apply(first["pos"], float(first["yaw"]))
		return
	for i: int in range(_buffer.size() - 1):
		var a: Dictionary = _buffer[i]
		var b: Dictionary = _buffer[i + 1]
		var tb: float = float(b["t"])
		if render_time <= tb:
			var ta: float = float(a["t"])
			var w: float = clampf((render_time - ta) / maxf(tb - ta, 0.0001), 0.0, 1.0)
			var pa: Vector3 = a["pos"]
			var pb: Vector3 = b["pos"]
			_apply(pa.lerp(pb, w), lerp_angle(float(a["yaw"]), float(b["yaw"]), w))
			# Drop samples we will never need again.
			for _n: int in range(i):
				_buffer.pop_front()
			return
	var last: Dictionary = _buffer.back()
	_apply(last["pos"], float(last["yaw"]))


func _apply(pos: Vector3, yaw: float) -> void:
	position = pos
	rotation.y = yaw
