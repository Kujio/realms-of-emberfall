## Client copy of the server's world geometry (server/src/game/world_geom.cpp): terrain height and
## the 2D collision push-out the movement rule applies. Prediction replays inputs through step(), so
## shapes, their order and the push-out rule must match the server exactly.
extends RefCounted

const EPS: float = 0.0001

## Terrain: base height plus round hills [x, z, radius, height].
var base: float = 0.0
var hills: Array[PackedFloat32Array] = []
## Circles [x, z, radius] and boxes [x, z, hx, hz, yaw], in content order.
var circles: Array[PackedFloat32Array] = []
var boxes: Array[PackedFloat32Array] = []
## Wall tiles (dungeons): row-major by z, 1 = solid; cell (0,0) starts at tile_origin.
var tiles: PackedByteArray = PackedByteArray()
var tile_w: int = 0
var tile_h: int = 0
var tile_cell: float = 4.0
var tile_origin: Vector2 = Vector2.ZERO


func height(x: float, z: float) -> float:
	var y: float = base
	for h: PackedFloat32Array in hills:
		var dx: float = x - h[0]
		var dz: float = z - h[1]
		var r2: float = h[2] * h[2]
		var d2: float = dx * dx + dz * dz
		if d2 >= r2:
			continue
		var t: float = 1.0 - d2 / r2
		y += h[3] * t * t
	return y


func has_collision() -> bool:
	return not circles.is_empty() or not boxes.is_empty() or tile_w > 0


func solid_cell(ix: int, iz: int) -> bool:
	if ix < 0 or iz < 0 or ix >= tile_w or iz >= tile_h:
		return true
	return tiles[iz * tile_w + ix] != 0


func cell_x(x: float) -> int:
	return floori((x - tile_origin.x) / tile_cell)


func cell_z(z: float) -> int:
	return floori((z - tile_origin.y) / tile_cell)


static func push_circle(p: Vector2, radius: float, c: PackedFloat32Array) -> Vector2:
	var dx: float = p.x - c[0]
	var dz: float = p.y - c[1]
	var need: float = c[2] + radius
	var d2: float = dx * dx + dz * dz
	if d2 >= need * need:
		return p
	var d: float = sqrt(d2)
	if d < EPS:
		return Vector2(c[0] + need, p.y)
	return Vector2(c[0] + dx / d * need, c[1] + dz / d * need)


static func push_box(p: Vector2, radius: float, bx: float, bz: float, hx: float, hz: float, yaw: float) -> Vector2:
	var c: float = cos(yaw)
	var s: float = sin(yaw)
	var dx: float = p.x - bx
	var dz: float = p.y - bz
	var lx: float = dx * c - dz * s
	var lz: float = dx * s + dz * c
	var cx: float = clampf(lx, -hx, hx)
	var cz: float = clampf(lz, -hz, hz)
	var ox: float = lx - cx
	var oz: float = lz - cz
	var d2: float = ox * ox + oz * oz
	if d2 >= radius * radius:
		return p
	if d2 > EPS * EPS:
		var d: float = sqrt(d2)
		lx = cx + ox / d * radius
		lz = cz + oz / d * radius
	else:
		var px: float = hx - absf(lx)
		var pz: float = hz - absf(lz)
		if px < pz:
			lx = (-1.0 if lx < 0.0 else 1.0) * (hx + radius)
		else:
			lz = (-1.0 if lz < 0.0 else 1.0) * (hz + radius)
	return Vector2(bx + lx * c + lz * s, bz - lx * s + lz * c)


## Pushes a body out of everything it overlaps: circles, then boxes, then tiles, twice over.
func resolve(p: Vector2, radius: float) -> Vector2:
	if not has_collision():
		return p
	for _pass: int in range(2):
		for c: PackedFloat32Array in circles:
			if absf(p.x - c[0]) < c[2] + radius + 2.0 and absf(p.y - c[1]) < c[2] + radius + 2.0:
				p = push_circle(p, radius, c)
		for b: PackedFloat32Array in boxes:
			var reach: float = sqrt(b[2] * b[2] + b[3] * b[3]) + radius + 2.0
			if absf(p.x - b[0]) < reach and absf(p.y - b[1]) < reach:
				p = push_box(p, radius, b[0], b[1], b[2], b[3], b[4])
		if tile_w > 0:
			var half: float = tile_cell * 0.5
			for iz: int in range(cell_z(p.y - radius), cell_z(p.y + radius) + 1):
				for ix: int in range(cell_x(p.x - radius), cell_x(p.x + radius) + 1):
					if solid_cell(ix, iz):
						p = push_box(p, radius, tile_origin.x + (float(ix) + 0.5) * tile_cell,
							tile_origin.y + (float(iz) + 0.5) * tile_cell, half, half, 0.0)
	return p


## True if a body at (x, z) touches anything.
func overlaps(x: float, z: float, radius: float) -> bool:
	var p: Vector2 = Vector2(x, z)
	for c: PackedFloat32Array in circles:
		if push_circle(p, radius, c) != p:
			return true
	for b: PackedFloat32Array in boxes:
		if push_box(p, radius, b[0], b[1], b[2], b[3], b[4]) != p:
			return true
	if tile_w > 0:
		for iz: int in range(cell_z(z - radius), cell_z(z + radius) + 1):
			for ix: int in range(cell_x(x - radius), cell_x(x + radius) + 1):
				if solid_cell(ix, iz):
					return true
	return false


## The movement rule with collision (server movement.cpp step_position, zone-aware overload).
func step(pos: Vector3, move_x: float, move_z: float, speed: float, dt: float, bounds: Rect2, radius: float) -> Vector3:
	var mx: float = move_x
	var mz: float = move_z
	var len_sq: float = mx * mx + mz * mz
	if len_sq > 1.0:
		var inv: float = 1.0 / sqrt(len_sq)
		mx *= inv
		mz *= inv
	var p: Vector2 = Vector2(pos.x + mx * speed * dt, pos.z + mz * speed * dt)
	p = resolve(p, radius)
	var x: float = clampf(p.x, bounds.position.x, bounds.end.x)
	var z: float = clampf(p.y, bounds.position.y, bounds.end.y)
	return Vector3(x, height(x, z), z)


## FNV-1a over 8 little-endian bytes per value: the server's LayoutHash.
class LayoutHash:
	var h: int = 2166136261

	func add(v: int) -> void:
		for i: int in range(8):
			h = h ^ ((v >> (8 * i)) & 0xFF)
			h = (h * 16777619) & 0xFFFFFFFF

	func add_q(v: float, scale: float) -> void:
		add(int(roundf(v * scale)))

	func add_cm(v: float) -> void:
		add_q(v, 100.0)
