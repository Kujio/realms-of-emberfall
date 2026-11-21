## Client copy of the server's movement rule (server/src/game/movement.cpp: step_position).
## Prediction and reconciliation replay inputs through this, so it must stay identical.
extends RefCounted


static func step(pos: Vector3, move_x: float, move_z: float, speed: float, dt: float, bounds: Rect2, ground_y: float) -> Vector3:
	var mx: float = move_x
	var mz: float = move_z
	var len_sq: float = mx * mx + mz * mz
	if len_sq > 1.0:
		var inv: float = 1.0 / sqrt(len_sq)
		mx *= inv
		mz *= inv
	var x: float = clampf(pos.x + mx * speed * dt, bounds.position.x, bounds.end.x)
	var z: float = clampf(pos.z + mz * speed * dt, bounds.position.y, bounds.end.y)
	return Vector3(x, ground_y, z)


## Facing direction for a yaw (Godot convention: yaw 0 faces -Z).
static func forward(yaw: float) -> Vector3:
	return Vector3(-sin(yaw), 0.0, -cos(yaw))


static func right(yaw: float) -> Vector3:
	return Vector3(cos(yaw), 0.0, -sin(yaw))
