## Third-person follow camera. Mouse wheel zooms; right-drag pitches (yaw follows the player).
extends Node3D

const PITCH_SENSITIVITY: float = 0.004

var target: Node3D = null
var distance: float = 7.0
var pitch: float = -0.35
var _camera: Camera3D


func _ready() -> void:
	_camera = Camera3D.new()
	_camera.far = 800.0
	_camera.fov = 70.0
	_camera.current = true
	add_child(_camera)


func _unhandled_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb != null and mb.pressed:
		if mb.button_index == MOUSE_BUTTON_WHEEL_UP:
			distance = maxf(2.5, distance - 0.75)
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			distance = minf(25.0, distance + 0.75)
	var mm: InputEventMouseMotion = event as InputEventMouseMotion
	if mm != null and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		pitch = clampf(pitch - mm.relative.y * PITCH_SENSITIVITY, -1.3, 0.35)


func _process(_delta: float) -> void:
	if target == null:
		return
	global_position = target.global_position + Vector3(0.0, 1.7, 0.0)
	rotation = Vector3(pitch, target.rotation.y, 0.0)
	_camera.position = Vector3(0.0, 0.0, distance)
