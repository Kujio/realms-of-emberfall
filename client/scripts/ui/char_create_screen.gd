## Character creation: name, race, class (filtered by race), starting city (filtered by race).
extends Control

signal finished

const UI := preload("res://scripts/ui/ui.gd")

var _name: LineEdit
var _race: OptionButton
var _class: OptionButton
var _city: OptionButton
var _desc: Label
var _status: Label
var _create_btn: Button


func _ready() -> void:
	UI.backdrop(self)
	var box: VBoxContainer = UI.centered_panel(self, 460)
	UI.title(box, "Create a Character", 28)
	_name = UI.field(box, "Name (4-15 letters)", "")
	_name.max_length = 15
	UI.label(box, "Race")
	_race = OptionButton.new()
	box.add_child(_race)
	UI.label(box, "Class")
	_class = OptionButton.new()
	box.add_child(_class)
	UI.label(box, "Starting city")
	_city = OptionButton.new()
	box.add_child(_city)
	_desc = UI.label(box, "")
	_desc.custom_minimum_size = Vector2(0, 60)
	var buttons: HBoxContainer = UI.row(box)
	_create_btn = UI.button(buttons, "Create", _create)
	UI.button(buttons, "Back", func() -> void: finished.emit())
	_status = UI.label(box, "")

	for r: Dictionary in Session.create_options.get("races", []):
		_race.add_item(str(r["name"]))
		_race.set_item_metadata(_race.item_count - 1, r)
	_race.item_selected.connect(func(_i: int) -> void: _on_race_changed())
	_class.item_selected.connect(func(_i: int) -> void: _update_description())
	_name.text_submitted.connect(func(_t: String) -> void: _create())
	Session.char_create_finished.connect(_on_result)
	_on_race_changed()
	_name.grab_focus()


func _on_race_changed() -> void:
	_class.clear()
	_city.clear()
	if _race.selected < 0:
		return
	var race: Dictionary = _race.get_item_metadata(_race.selected)
	for cid: String in race["classes"]:
		_class.add_item(Session.class_name_for(cid))
		_class.set_item_metadata(_class.item_count - 1, cid)
	for city: String in race["cities"]:
		_city.add_item(Session.city_name(city))
		_city.set_item_metadata(_city.item_count - 1, city)
	_update_description()


func _update_description() -> void:
	var text: String = ""
	if _race.selected >= 0:
		var race: Dictionary = _race.get_item_metadata(_race.selected)
		text = str(race.get("description", ""))
	if _class.selected >= 0:
		var cid: String = _class.get_item_metadata(_class.selected)
		for c: Dictionary in Session.create_options.get("classes", []):
			if str(c["id"]) == cid:
				text += "\n" + str(c.get("description", ""))
	_desc.text = text


func _create() -> void:
	if _race.selected < 0 or _class.selected < 0 or _city.selected < 0:
		UI.set_status(_status, "Choose a race, class, and city.", true)
		return
	var race: Dictionary = _race.get_item_metadata(_race.selected)
	_create_btn.disabled = true
	UI.set_status(_status, "Creating...", false)
	Session.create_character(_name.text.strip_edges(), str(race["id"]), str(_class.get_item_metadata(_class.selected)),
		str(_city.get_item_metadata(_city.selected)))


func _on_result(ok: bool, message: String) -> void:
	_create_btn.disabled = false
	if ok:
		finished.emit()
	else:
		UI.set_status(_status, message, true)


## Used by the autotest driver.
func autotest_create(char_name: String, race_id: String, class_id: String, city_id: String) -> void:
	_name.text = char_name
	for i: int in range(_race.item_count):
		var r: Dictionary = _race.get_item_metadata(i)
		if str(r["id"]) == race_id:
			_race.select(i)
			_on_race_changed()
	for i: int in range(_class.item_count):
		if str(_class.get_item_metadata(i)) == class_id:
			_class.select(i)
	for i: int in range(_city.item_count):
		if str(_city.get_item_metadata(i)) == city_id:
			_city.select(i)
	_create()
