## The world map (Phase 12): every city and outdoor zone drawn in its cell of the map (world.json, from
## tools/gen_world.py), linked by its roads, coloured by how its level band suits you, with its dungeons
## marked where their entrances are. Click a zone for its details and the way there from where you are.
## Display only: the server decides everything about travel.
extends Control

signal picked(zone_id: String)

const CON_COLORS: Array[Color] = [
	Color(0.6, 0.6, 0.6), Color(0.35, 0.9, 0.35), Color(0.45, 0.65, 1.0),
	Color(1.0, 1.0, 1.0), Color(1.0, 0.9, 0.25), Color(1.0, 0.3, 0.3),
]
const CON_NAMES: Array[String] = ["grey", "green", "blue", "white", "yellow", "red"]
## The share of a cell its zone's box fills (the rest is road).
const BOX: float = 0.74

var zones: Dictionary = {}  # id -> zone entry from world.json
var travel: Array = []      # travel points from world.json
var current: String = ""
var selected: String = ""
var level: int = 1
var route: PackedStringArray = PackedStringArray()
var _boxes: Dictionary = {}  # id -> Rect2 on screen, from the last draw


func set_world(world: Dictionary) -> void:
	zones.clear()
	for z: Variant in world.get("zones", []):
		var zd: Dictionary = z as Dictionary
		zones[str(zd["id"])] = zd
	travel = world.get("travel", []) as Array
	_refresh_route()
	queue_redraw()


func set_current(zone_id: String, player_level: int) -> void:
	current = zone_id
	level = maxi(1, player_level)
	_refresh_route()
	queue_redraw()


func select(zone_id: String) -> void:
	selected = zone_id if zones.has(zone_id) else ""
	_refresh_route()
	queue_redraw()
	picked.emit(selected)


## A level band's colour for a player of `lvl`: white while you are inside it, yellow or red above
## you, and blue, green or grey below you by the same spans the server's con uses (rules.json "con").
static func band_con(lvl: int, lo: int, hi: int) -> int:
	if lvl < lo:
		return 5 if lo - lvl >= 3 else 4
	if lvl <= hi:
		return 3
	var gap: int = lvl - hi
	var blue_span: int = 1 + lvl / 7
	var green_span: int = 1 + lvl / 6
	if gap <= blue_span:
		return 2
	if gap <= blue_span + green_span:
		return 1
	return 0


## The map cell a zone sits in, or null for one the map leaves out (the test zone).
func cell_of(zone_id: String) -> Variant:
	var z: Dictionary = zones.get(zone_id, {}) as Dictionary
	var m: Variant = z.get("map")
	if m is Dictionary and (m as Dictionary).has("x"):
		return Vector2i(int((m as Dictionary)["x"]), int((m as Dictionary)["y"]))
	if m is Dictionary and (m as Dictionary).has("in"):
		return cell_of(str((m as Dictionary)["in"]))
	return null


## Zones joined by a road (either way round): the zone lines of both ends.
func neighbours(zone_id: String) -> PackedStringArray:
	var out: PackedStringArray = PackedStringArray()
	for l: Variant in (zones.get(zone_id, {}) as Dictionary).get("links", []):
		if zones.has(str(l)) and not out.has(str(l)):
			out.append(str(l))
	for id: String in zones:
		if id != zone_id and not out.has(id) and ((zones[id] as Dictionary).get("links", []) as Array).has(zone_id):
			out.append(id)
	return out


## The fewest zones from `from` to `to` along the roads (both ends included), or empty.
func path(from: String, to: String) -> PackedStringArray:
	if not zones.has(from) or not zones.has(to):
		return PackedStringArray()
	var prev: Dictionary = {from: ""}
	var queue: Array[String] = [from]
	while not queue.is_empty():
		var at: String = queue.pop_front()
		if at == to:
			break
		for n: String in neighbours(at):
			if not prev.has(n):
				prev[n] = at
				queue.append(n)
	if not prev.has(to):
		return PackedStringArray()
	var out: PackedStringArray = PackedStringArray()
	var step: String = to
	while step != "":
		out.insert(0, step)
		step = str(prev[step])
	return out


func _refresh_route() -> void:
	route = path(current, selected) if selected != "" and current != "" else PackedStringArray()


## Everything there is to say about a zone, for the details line under the map.
func details(zone_id: String) -> String:
	if not zones.has(zone_id):
		return "Click a place on the map."
	var z: Dictionary = zones[zone_id]
	var lv: Dictionary = z.get("levels", {}) as Dictionary
	var lo: int = int(lv.get("min", 1))
	var hi: int = int(lv.get("max", 60))
	var kind: String = str(z.get("kind", ""))
	var lines: PackedStringArray = PackedStringArray()
	var band: String = "levels %d to %d" % [lo, hi]
	if kind == "hub":
		band = "a city"
	lines.append("%s  (%s, %s)%s" % [str(z["name"]), kind if kind != "hub" else "city", band,
		"" if kind == "hub" else "  - %s for you" % CON_NAMES[band_con(level, lo, hi)]])
	lines.append(str(z.get("description", "")))
	var inside: PackedStringArray = PackedStringArray()
	for id: String in zones:
		var m: Variant = (zones[id] as Dictionary).get("map")
		if m is Dictionary and str((m as Dictionary).get("in", "")) == zone_id:
			inside.append(str((zones[id] as Dictionary)["name"]))
	if not inside.is_empty():
		lines.append("Dungeons: " + ", ".join(inside))
	var stops: PackedStringArray = PackedStringArray()
	for t: Variant in travel:
		if str((t as Dictionary).get("zone", "")) == zone_id:
			stops.append(str((t as Dictionary)["name"]))
	if not stops.is_empty():
		lines.append("Travel: " + ", ".join(stops))
	if zone_id == current:
		lines.append("You are here.")
	elif route.size() >= 2 and route[route.size() - 1] == zone_id:
		var names: PackedStringArray = PackedStringArray()
		for r: String in route:
			names.append(str((zones[r] as Dictionary)["name"]))
		lines.append("The way there: " + " > ".join(names))
	elif current != "":
		lines.append("No road leads there from here (try a ferry, a teleport stone or a portal).")
	return "\n".join(lines)


func _gui_input(event: InputEvent) -> void:
	var mb: InputEventMouseButton = event as InputEventMouseButton
	if mb == null or not mb.pressed or mb.button_index != MOUSE_BUTTON_LEFT:
		return
	# The smallest mark under the pointer: a dungeon's diamond lies inside its zone's box.
	var best: String = ""
	var best_area: float = INF
	for id: String in _boxes:
		var r: Rect2 = _boxes[id]
		if r.has_point(mb.position) and r.get_area() < best_area:
			best = id
			best_area = r.get_area()
	if best != "":
		select(best)
		accept_event()


func _draw() -> void:
	var s: Vector2 = size
	draw_rect(Rect2(Vector2.ZERO, s), Color(0.05, 0.06, 0.07, 0.85))
	_boxes.clear()
	var lo: Vector2i = Vector2i(1 << 20, 1 << 20)
	var hi: Vector2i = Vector2i(-(1 << 20), -(1 << 20))
	for id: String in zones:
		var m: Variant = (zones[id] as Dictionary).get("map")
		if m is Dictionary and (m as Dictionary).has("x"):
			var c: Vector2i = Vector2i(int((m as Dictionary)["x"]), int((m as Dictionary)["y"]))
			lo = Vector2i(mini(lo.x, c.x), mini(lo.y, c.y))
			hi = Vector2i(maxi(hi.x, c.x), maxi(hi.y, c.y))
	if lo.x > hi.x:
		return
	var cols: int = hi.x - lo.x + 1
	var rows: int = hi.y - lo.y + 1
	# Cells as wide as the window allows (names need the room), at most three times their height.
	var ch: float = (s.y - 16.0) / float(rows)
	var cw: float = minf((s.x - 16.0) / float(cols), ch * 3.0)
	var cell_size: Vector2 = Vector2(cw, ch)
	var cell: float = ch
	var origin: Vector2 = (s - Vector2(cols, rows) * cell_size) * 0.5
	var box_size: Vector2 = cell_size * BOX
	var rect_of: Callable = func(c: Vector2i) -> Rect2:
		var centre: Vector2 = origin + (Vector2(c - lo) + Vector2(0.5, 0.5)) * cell_size
		return Rect2(centre - box_size * 0.5, box_size)
	var font: Font = ThemeDB.fallback_font
	# Roads first, under the boxes; the way to the selected zone in gold.
	var drawn: Dictionary = {}
	for id: String in zones:
		var a: Variant = cell_of(id)
		if a == null or (zones[id] as Dictionary).get("kind") == "dungeon":
			continue
		for n: String in neighbours(id):
			var b: Variant = cell_of(n)
			if b == null or (zones[n] as Dictionary).get("kind") == "dungeon":
				continue
			var key: String = id + "|" + n if id < n else n + "|" + id
			if drawn.has(key):
				continue
			drawn[key] = true
			var on_route: bool = _on_route(id, n)
			draw_line((rect_of.call(a) as Rect2).get_center(), (rect_of.call(b) as Rect2).get_center(),
				Color(1.0, 0.8, 0.3, 0.95) if on_route else Color(0.55, 0.5, 0.42, 0.7), 4.0 if on_route else 2.0)
	# Zones and cities.
	for id: String in zones:
		var z: Dictionary = zones[id]
		var m: Variant = z.get("map")
		if not (m is Dictionary and (m as Dictionary).has("x")):
			continue
		var r: Rect2 = rect_of.call(Vector2i(int((m as Dictionary)["x"]), int((m as Dictionary)["y"])))
		_boxes[id] = r
		var lv: Dictionary = z.get("levels", {}) as Dictionary
		var hub: bool = str(z.get("kind", "")) == "hub"
		var con_c: Color = Color(0.85, 0.75, 0.55) if hub else CON_COLORS[band_con(level, int(lv.get("min", 1)), int(lv.get("max", 60)))]
		draw_rect(r, con_c.darkened(0.72))
		draw_rect(r, con_c.darkened(0.2), false, 1.5)
		if id == current:
			draw_rect(r.grow(3.0), Color(1.0, 0.85, 0.3), false, 3.0)
		if id == selected:
			draw_rect(r.grow(6.0), Color(1.0, 1.0, 1.0, 0.9), false, 2.0)
		# The name on the top line with the band at its right; the lower half is left for dungeons.
		var fs: int = clampi(int(cell * 0.2), 10, 13)
		var title: String = str(z["name"]).trim_prefix("The ")
		var band: String = "city" if hub else "%d-%d" % [int(lv.get("min", 1)), int(lv.get("max", 60))]
		var band_w: float = font.get_string_size(band, HORIZONTAL_ALIGNMENT_LEFT, -1, fs - 1).x
		var title_w: float = font.get_string_size(title, HORIZONTAL_ALIGNMENT_LEFT, -1, fs).x
		# A long name takes the whole top line and the band drops to the line below, at the right.
		var band_y: float = r.position.y + fs + 3.0
		if title_w + band_w + 14.0 > r.size.x:
			band_y += float(fs) + 3.0
		draw_string(font, r.position + Vector2(5, fs + 3), title, HORIZONTAL_ALIGNMENT_LEFT, r.size.x - 10.0, fs, Color(0.95, 0.92, 0.85))
		draw_string(font, Vector2(r.end.x - band_w - 5, band_y), band, HORIZONTAL_ALIGNMENT_LEFT, -1, fs - 1, con_c)
	# Dungeons: a diamond where the entrance is, inside the zone's box.
	for id: String in zones:
		var m2: Variant = (zones[id] as Dictionary).get("map")
		if not (m2 is Dictionary and (m2 as Dictionary).has("in")) or not _boxes.has(str((m2 as Dictionary)["in"])):
			continue
		var pr: Rect2 = _boxes[str((m2 as Dictionary)["in"])]
		# Where the entrance is across the zone, kept to the box's lower half, under the name.
		var fx: float = float((m2 as Dictionary)["fx"])
		var fz: float = float((m2 as Dictionary)["fz"])
		var p: Vector2 = pr.position + Vector2(pr.size.x * (0.08 + 0.6 * fx), pr.size.y * (0.55 + 0.35 * fz))  # clear of the band at the right
		var d: float = clampf(cell * 0.05, 4.0, 8.0)
		var dl: Dictionary = (zones[id] as Dictionary).get("levels", {}) as Dictionary
		var dc: Color = CON_COLORS[band_con(level, int(dl.get("min", 1)), int(dl.get("max", 60)))]
		draw_colored_polygon(PackedVector2Array([p + Vector2(0, -d), p + Vector2(d, 0), p + Vector2(0, d), p + Vector2(-d, 0)]), dc)
		draw_polyline(PackedVector2Array([p + Vector2(0, -d), p + Vector2(d, 0), p + Vector2(0, d), p + Vector2(-d, 0), p + Vector2(0, -d)]),
			Color(0.1, 0.1, 0.1), 1.0)
		var dr: Rect2 = Rect2(p - Vector2(d, d), Vector2(d, d) * 2.0)
		_boxes[id] = dr  # dungeons can be clicked too
		if id == current or id == selected:
			draw_rect(dr.grow(3.0), Color(1.0, 0.85, 0.3) if id == current else Color(1, 1, 1), false, 2.0)


func _on_route(a: String, b: String) -> bool:
	for i: int in range(route.size() - 1):
		if (route[i] == a and route[i + 1] == b) or (route[i] == b and route[i + 1] == a):
			return true
	return false
