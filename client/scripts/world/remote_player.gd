## Another player, rendered slightly in the past by interpolating buffered server snapshots.
extends Node3D

const Avatar := preload("res://scripts/world/avatar.gd")
const MAX_BUFFER: int = 40

var entity_id: int = 0
var display_name: String = ""
var class_id: String = ""
var _buffer: Array[Dictionary] = []


func setup(spawn: Dictionary, server_time: float) -> void:
	entity_id = int(spawn["entity_id"])
	display_name = str(spawn["name"])
	class_id = str(spawn["class_id"])
	var pos: Vector3 = Vector3(float(spawn["x"]), float(spawn["y"]), float(spawn["z"]))
	position = pos
	rotation.y = float(spawn["yaw"])
	push_state(server_time, pos, float(spawn["yaw"]))


func _ready() -> void:
	Avatar.build(self, Avatar.color_for_class(class_id), display_name, Color(0.85, 0.92, 1.0))


func push_state(t: float, pos: Vector3, yaw: float) -> void:
	if _buffer.size() > 0 and t <= float(_buffer.back()["t"]):
		return
	_buffer.append({"t": t, "pos": pos, "yaw": yaw})
	if _buffer.size() > MAX_BUFFER:
		_buffer.pop_front()


func update_render(render_time: float) -> void:
	if _buffer.is_empty():
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
