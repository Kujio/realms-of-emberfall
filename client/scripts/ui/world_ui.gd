## World HUD pieces (Phase 6): the travel window (teleport stones, ferries, soulbinders). Pure
## presentation: a choice goes out as a signal and the world sends it to the server.
extends Control

signal travel_chosen(npc_id: int, dest: String)

var travel_npc: int = 0
var travel_options: Array = []
var _travel_win: PanelContainer
var _travel_title: Label
var _travel_text: Label
var _travel_list: VBoxContainer


func _ready() -> void:
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_build_travel_window()


static func coin_text(copper: int) -> String:
	if copper <= 0:
		return "free"
	var parts: PackedStringArray = PackedStringArray()
	var units: Array = [[1000, "p"], [100, "g"], [10, "s"], [1, "c"]]
	var left: int = copper
	for u: Array in units:
		var n: int = left / int(u[0])
		left -= n * int(u[0])
		if n > 0:
			parts.append("%d%s" % [n, str(u[1])])
	return " ".join(parts)


func _build_travel_window() -> void:
	_travel_win = PanelContainer.new()
	_travel_win.name = "TravelWindow"
	_travel_win.visible = false
	_travel_win.mouse_filter = Control.MOUSE_FILTER_STOP
	_travel_win.set_anchors_preset(Control.PRESET_CENTER)
	_travel_win.position = Vector2(-180.0, -160.0)
	_travel_win.custom_minimum_size = Vector2(360.0, 0.0)
	add_child(_travel_win)
	var box: VBoxContainer = VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	_travel_win.add_child(box)
	_travel_title = Label.new()
	_travel_title.add_theme_font_size_override("font_size", 18)
	box.add_child(_travel_title)
	_travel_text = Label.new()
	_travel_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_travel_text.custom_minimum_size = Vector2(340.0, 0.0)
	box.add_child(_travel_text)
	_travel_list = VBoxContainer.new()
	box.add_child(_travel_list)
	var close: Button = Button.new()
	close.text = "Close"
	close.pressed.connect(close_travel)
	box.add_child(close)


## A TravelOptions message: the NPC's name, greeting and destinations.
func show_travel(msg: Dictionary) -> void:
	travel_npc = int(msg["npc_id"])
	travel_options = msg["options"] as Array
	_travel_title.text = str(msg["title"])
	_travel_text.text = str(msg["text"])
	for c: Node in _travel_list.get_children():
		c.queue_free()
	for o: Variant in travel_options:
		var od: Dictionary = o as Dictionary
		var b: Button = Button.new()
		var label: String = str(od["name"])
		if str(od["id"]) != "bind":
			label += "  (%s%s)" % [coin_text(int(od["fee"])), ", sails in %ds" % (int(od["delay_ms"]) / 1000) if int(od["delay_ms"]) > 0 else ""]
		b.text = label
		var dest: String = str(od["id"])
		b.pressed.connect(func() -> void:
			travel_chosen.emit(travel_npc, dest)
			close_travel())
		_travel_list.add_child(b)
	_travel_win.visible = true


func close_travel() -> void:
	_travel_win.visible = false


func travel_visible() -> bool:
	return _travel_win.visible
