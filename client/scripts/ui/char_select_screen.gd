## Character list with enter / create / delete / log out, and a turning preview of the selected character.
extends Control

signal create_requested

const UI := preload("res://scripts/ui/ui.gd")
const ModelPreview := preload("res://scripts/ui/model_preview.gd")

var _list: ItemList
var _status: Label
var _enter_btn: Button
var _create_btn: Button
var _delete_btn: Button
var _confirm: ConfirmationDialog
var _pending_delete_id: int = -1
var preview: ModelPreview


func _ready() -> void:
	UI.backdrop(self)
	var box: VBoxContainer = UI.centered_panel(self, 520)
	UI.title(box, "Choose Your Character", 28)
	UI.label(box, "Account: %s" % Session.username)
	_list = ItemList.new()
	_list.custom_minimum_size = Vector2(0, 220)
	_list.item_activated.connect(func(_i: int) -> void: _enter())
	_list.item_selected.connect(func(_i: int) -> void: _refresh_buttons())
	box.add_child(_list)
	var buttons: HBoxContainer = UI.row(box)
	_enter_btn = UI.button(buttons, "Enter World", _enter)
	_create_btn = UI.button(buttons, "Create New", func() -> void: create_requested.emit())
	_delete_btn = UI.button(buttons, "Delete", _ask_delete)
	UI.button(buttons, "Log Out", func() -> void: Session.logout())
	_status = UI.label(box, "")
	preview = ModelPreview.new()
	UI.add_beside_panel(box, preview)
	_confirm = ConfirmationDialog.new()
	_confirm.title = "Delete character"
	_confirm.confirmed.connect(_delete)
	_confirm.canceled.connect(func() -> void: _pending_delete_id = -1)
	add_child(_confirm)

	Session.char_list_updated.connect(_populate)
	Session.char_delete_finished.connect(func(ok: bool, message: String) -> void: UI.set_status(_status, message, not ok))
	Session.enter_world_failed.connect(_on_enter_failed)
	Session.status_changed.connect(func(text: String, is_error: bool) -> void: UI.set_status(_status, text, is_error))
	_populate()
	Session.request_char_list()


func _populate() -> void:
	var previous: int = _selected_id()
	_list.clear()
	for c: Dictionary in Session.characters:
		var line: String = "%s  -  Level %d %s %s  (%s)" % [
			str(c["name"]), int(c["level"]), Session.race_name(str(c["race"])),
			Session.class_name_for(str(c["class_id"])), Session.city_name(str(c["city"]))]
		var idx: int = _list.add_item(line)
		_list.set_item_metadata(idx, int(c["id"]))
		if int(c["id"]) == previous:
			_list.select(idx)
	if _list.item_count > 0 and not _list.is_anything_selected():
		_list.select(0)
	_create_btn.text = "Create New (%d/%d)" % [Session.characters.size(), Session.max_characters]
	# If the character awaiting delete confirmation vanished, cancel rather than retarget.
	if _pending_delete_id >= 0 and not _has_character(_pending_delete_id):
		_pending_delete_id = -1
		_confirm.hide()
	_refresh_buttons()


func _refresh_buttons() -> void:
	var has: bool = _list.is_anything_selected()
	_update_preview()
	_enter_btn.disabled = not has
	_delete_btn.disabled = not has
	_create_btn.disabled = Session.characters.size() >= Session.max_characters


func _update_preview() -> void:
	var id: int = _selected_id()
	for c: Dictionary in Session.characters:
		if int(c["id"]) == id:
			preview.show_character(str(c["race"]), int(c.get("gender", 0)), str(c["class_id"]))
			return
	preview.clear()


func _selected_id() -> int:
	if _list == null or not _list.is_anything_selected():
		return -1
	return int(_list.get_item_metadata(_list.get_selected_items()[0]))


func _enter() -> void:
	var id: int = _selected_id()
	if id < 0:
		return
	_enter_btn.disabled = true
	UI.set_status(_status, "Entering the world...", false)
	Session.enter_world(id)


func _on_enter_failed(message: String) -> void:
	UI.set_status(_status, message, true)
	_refresh_buttons()


func _ask_delete() -> void:
	var id: int = _selected_id()
	if id < 0:
		return
	# Capture the target now: the list can refresh (and the selection move) while the dialog is open.
	_pending_delete_id = id
	_confirm.dialog_text = "Permanently delete %s? This cannot be undone." % _list.get_item_text(_list.get_selected_items()[0]).get_slice("  -  ", 0)
	_confirm.popup_centered()


func _delete() -> void:
	var id: int = _pending_delete_id
	_pending_delete_id = -1
	if id >= 0 and _has_character(id):
		Session.delete_character(id)


func _has_character(id: int) -> bool:
	for c: Dictionary in Session.characters:
		if int(c["id"]) == id:
			return true
	return false


## Used by the autotest driver: returns the character id with this name, or -1.
func find_character(char_name: String) -> int:
	for c: Dictionary in Session.characters:
		if str(c["name"]).to_lower() == char_name.to_lower():
			return int(c["id"])
	return -1
