## Client copy of the server's seeded dungeon generator (server/src/game/dungeon_gen.cpp). Integer
## math only (xorshift32), so the same seed builds the same rooms, corridors and walls on both sides.
## A golden hash in the tests pins the two together.
extends RefCounted

const ZoneGeom := preload("res://scripts/world/zone_geom.gd")

const MASK: int = 0xFFFFFFFF

var seed_value: int = 1
var rooms_wanted: int = 8
var cell: float = 4.0
var sector: int = 12
var min_room: int = 4
var max_room: int = 8
var corridor: int = 2

# Output
var w: int = 0
var h: int = 0
var solid: PackedByteArray = PackedByteArray()
var origin: Vector2 = Vector2.ZERO
## Each room: {x, z, w, h, depth, parent}
var rooms: Array[Dictionary] = []
var boss: int = 0

var _s: int = 0


func _next() -> int:
	_s = (_s ^ (_s << 13)) & MASK
	_s = _s ^ (_s >> 17)
	_s = (_s ^ (_s << 5)) & MASK
	return _s


func _below(n: int) -> int:
	return 0 if n <= 1 else _next() % n


static func xorshift_first(seed: int) -> int:
	var s: int = 0x9E3779B9 if seed == 0 else seed & MASK
	s = (s ^ (s << 13)) & MASK
	s = s ^ (s >> 17)
	s = (s ^ (s << 5)) & MASK
	return s


func valid() -> bool:
	return rooms_wanted >= 2 and rooms_wanted <= 64 and cell >= 1.0 and cell <= 16.0 and sector >= 8 and sector <= 32 \
		and min_room >= 3 and max_room >= min_room and max_room <= sector - 2 and corridor >= 1 and corridor <= min_room


func generate() -> void:
	rooms.clear()
	boss = 0
	if not valid():
		return
	_s = 0x9E3779B9 if seed_value == 0 else seed_value & MASK
	var g: int = 1
	while g * g < rooms_wanted:
		g += 1
	g += 1
	var s_: int = sector
	var owner: PackedInt32Array = PackedInt32Array()
	owner.resize(g * g)
	owner.fill(-1)
	var placed: Array[Vector2i] = [Vector2i((g >> 1), g - 1)]
	owner[(g - 1) * g + (g >> 1)] = 0
	rooms.append({"x": 0, "z": 0, "w": 1, "h": 1, "depth": 0, "parent": -1})
	var dx: Array[int] = [0, 1, 0, -1]
	var dz: Array[int] = [-1, 0, 1, 0]
	while placed.size() < rooms_wanted:
		var cands: Array[Vector3i] = []  # (from, sx, sz)
		for i: int in range(placed.size()):
			for k: int in range(4):
				var nx: int = placed[i].x + dx[k]
				var nz: int = placed[i].y + dz[k]
				if nx < 0 or nz < 0 or nx >= g or nz >= g:
					continue
				if owner[nz * g + nx] >= 0:
					continue
				cands.append(Vector3i(i, nx, nz))
		if cands.is_empty():
			break
		var c: Vector3i = cands[_below(cands.size())]
		owner[c.z * g + c.y] = placed.size()
		placed.append(Vector2i(c.y, c.z))
		rooms.append({"x": 0, "z": 0, "w": 1, "h": 1, "depth": int(rooms[c.x]["depth"]) + 1, "parent": c.x})
	for i: int in range(1, rooms.size()):
		if int(rooms[i]["depth"]) > int(rooms[boss]["depth"]):
			boss = i
	var span: int = max_room - min_room + 1
	for i: int in range(rooms.size()):
		var r: Dictionary = rooms[i]
		var ox: int = placed[i].x * s_
		var oz: int = placed[i].y * s_
		if i == boss:
			r["w"] = s_ - 2
			r["h"] = s_ - 2
			r["x"] = ox + 1
			r["z"] = oz + 1
			continue
		r["w"] = min_room + _below(span)
		r["h"] = min_room + _below(span)
		r["x"] = ox + 1 + _below(s_ - 2 - int(r["w"]) + 1)
		r["z"] = oz + 1 + _below(s_ - 2 - int(r["h"]) + 1)
	w = g * s_
	h = g * s_
	origin = Vector2(-float(w) * cell * 0.5, -float(h) * cell * 0.5)
	solid = PackedByteArray()
	solid.resize(w * h)
	solid.fill(1)
	for r: Dictionary in rooms:
		_carve(int(r["x"]), int(r["z"]), int(r["x"]) + int(r["w"]) - 1, int(r["z"]) + int(r["h"]) - 1)
	var cw: int = corridor
	for i: int in range(1, rooms.size()):
		var a: Dictionary = rooms[int(rooms[i]["parent"])]
		var b: Dictionary = rooms[i]
		var ax: int = int(a["x"]) + (int(a["w"]) >> 1)
		var az: int = int(a["z"]) + (int(a["h"]) >> 1)
		var bx: int = int(b["x"]) + (int(b["w"]) >> 1)
		var bz: int = int(b["z"]) + (int(b["h"]) >> 1)
		if _below(2) == 0:
			_carve(ax, az, bx, az + cw - 1)
			_carve(bx, az, bx + cw - 1, bz)
		else:
			_carve(ax, az, ax + cw - 1, bz)
			_carve(ax, bz, bx, bz + cw - 1)


func _carve(x0: int, z0: int, x1: int, z1: int) -> void:
	if x0 > x1:
		var t: int = x0
		x0 = x1
		x1 = t
	if z0 > z1:
		var t2: int = z0
		z0 = z1
		z1 = t2
	for z: int in range(maxi(1, z0), mini(h - 2, z1) + 1):
		for x: int in range(maxi(1, x0), mini(w - 2, x1) + 1):
			solid[z * w + x] = 0


func room_center(i: int) -> Vector2:
	var r: Dictionary = rooms[i]
	return Vector2(origin.x + (float(r["x"]) + float(r["w"]) * 0.5) * cell, origin.y + (float(r["z"]) + float(r["h"]) * 0.5) * cell)


func room_radius(i: int) -> float:
	var r: Dictionary = rooms[i]
	return maxf(0.0, (float(mini(int(r["w"]), int(r["h"]))) * 0.5 - 1.0) * cell)


func hash_value() -> int:
	var lh: ZoneGeom.LayoutHash = ZoneGeom.LayoutHash.new()
	lh.add(w)
	lh.add(h)
	for c: int in solid:
		lh.add(c)
	lh.add(rooms.size())
	for r: Dictionary in rooms:
		lh.add(int(r["x"]))
		lh.add(int(r["z"]))
		lh.add(int(r["w"]))
		lh.add(int(r["h"]))
		lh.add(int(r["depth"]))
		lh.add(int(r["parent"]))
	lh.add(boss)
	return lh.h
