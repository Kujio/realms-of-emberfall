## Root of the client: swaps between the login, character select/create, and world screens; shows the
## loading screen while a world loads and a dialog when the connection ends unexpectedly.
extends Node

const UI := preload("res://scripts/ui/ui.gd")
const LoginScreen := preload("res://scripts/ui/login_screen.gd")
const CharSelectScreen := preload("res://scripts/ui/char_select_screen.gd")
const CharCreateScreen := preload("res://scripts/ui/char_create_screen.gd")
const World := preload("res://scripts/world/world.gd")
const AutoTest := preload("res://scripts/autotest.gd")
const Lineup := preload("res://scripts/lineup.gd")
const LoadingScreen := preload("res://scripts/ui/loading_screen.gd")
const BASE_TITLE: String = "Realms of Emberfall"

var _current: Node = null
## Menu screens (Controls) live on a canvas layer, which sizes them to the window; a Control under a plain
## Node gets no size, so panels sat in the corner. The 3D world stays a direct child.
var _screens: CanvasLayer
var _loading: LoadingScreen
var _dialog: AcceptDialog
## The last message shown in the disconnect dialog (the autotest driver reads it).
var last_dialog: String = ""


func _ready() -> void:
	if OS.get_cmdline_user_args().has("--lineup"):
		var lineup: Node3D = Lineup.new()
		lineup.name = "Lineup"
		add_child(lineup)
		return
	_set_title("")
	_screens = CanvasLayer.new()
	_screens.name = "Screens"
	add_child(_screens)
	_loading = LoadingScreen.new()
	_loading.name = "LoadingScreen"
	add_child(_loading)
	_dialog = AcceptDialog.new()
	_dialog.name = "DisconnectDialog"
	_dialog.title = "Disconnected"
	_dialog.exclusive = true
	add_child(_dialog)
	Session.auth_succeeded.connect(_show_char_select)
	Session.returned_to_select.connect(_on_returned_to_select)
	Session.entered_world.connect(_on_entered_world)
	Session.session_ended.connect(_on_session_ended)
	Session.world_loading.connect(_on_world_loading)
	Session.enter_world_failed.connect(func(_m: String) -> void: _loading.hide_loading())
	_show_login("")
	if OS.get_cmdline_user_args().has("--autotest"):
		var tester: Node = AutoTest.new()
		tester.name = "AutoTest"
		add_child(tester)


func _set_title(detail: String) -> void:
	var version: String = str(ProjectSettings.get_setting("application/config/version", ""))
	get_window().title = "%s %s" % [BASE_TITLE, version] if detail == "" else "%s - %s" % [BASE_TITLE, detail]


func _swap(node: Node) -> void:
	if _current != null:
		# Out of the tree now (not at the end of the frame): a world being replaced by a zone change must
		# stop handling messages before the next one arrives.
		_current.get_parent().remove_child(_current)
		_current.queue_free()
	_current = node
	if node is Control:
		_screens.add_child(node)
		(node as Control).set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	else:
		add_child(node)
	move_child(_loading, -1)
	move_child(_dialog, -1)


func _show_login(message: String) -> void:
	_set_title("")
	var screen: Control = LoginScreen.new()
	screen.name = "LoginScreen"
	_swap(screen)
	if message != "":
		screen.call("show_message", message, true)


func _show_char_select() -> void:
	_set_title(Session.username)
	var screen: Control = CharSelectScreen.new()
	screen.name = "CharSelectScreen"
	screen.connect("create_requested", _show_char_create)
	_swap(screen)


func _on_returned_to_select() -> void:
	_loading.hide_loading()
	_show_char_select()


func _show_char_create() -> void:
	var screen: Control = CharCreateScreen.new()
	screen.name = "CharCreateScreen"
	screen.connect("finished", _show_char_select)
	_swap(screen)


func _on_world_loading(zone_name: String) -> void:
	_loading.show_loading(zone_name)


func _on_entered_world(info: Dictionary) -> void:
	_loading.set_zone(str(info.get("zone_name", "")))
	if not _loading.showing:
		_loading.show_loading(str(info.get("zone_name", "")))
	var world: Node3D = World.new()
	world.name = "World"
	_swap(world)
	var who: String = str(world.get("local_player").get("display_name")) if world.get("local_player") != null else Session.username
	_set_title("%s (%s)" % [who, str(info.get("zone_name", ""))])
	if not bool(info.get("zone_change", false)):
		UI.sound("enter_world")
	# Two frames: the world's first one is built, the second one drawn.
	await get_tree().process_frame
	await get_tree().process_frame
	_loading.hide_loading()


func _on_session_ended(reason: String) -> void:
	_loading.hide_loading()
	_show_login(reason)
	var text: String = UI.disconnect_text(reason)
	if text == "":
		return
	last_dialog = text
	UI.sound("ui_error")
	_dialog.dialog_text = text
	if not OS.get_cmdline_user_args().has("--autotest"):
		_dialog.popup_centered()


func current_screen() -> Node:
	return _current
