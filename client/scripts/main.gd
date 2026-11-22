## Root of the client: swaps between the login, character select/create, and world screens.
extends Node

const LoginScreen := preload("res://scripts/ui/login_screen.gd")
const CharSelectScreen := preload("res://scripts/ui/char_select_screen.gd")
const CharCreateScreen := preload("res://scripts/ui/char_create_screen.gd")
const World := preload("res://scripts/world/world.gd")
const AutoTest := preload("res://scripts/autotest.gd")

var _current: Node = null


func _ready() -> void:
	get_window().title = "Realms of Emberfall"
	Session.auth_succeeded.connect(_show_char_select)
	Session.returned_to_select.connect(_show_char_select)
	Session.entered_world.connect(_on_entered_world)
	Session.session_ended.connect(_on_session_ended)
	_show_login("")
	if OS.get_cmdline_user_args().has("--autotest"):
		var tester: Node = AutoTest.new()
		tester.name = "AutoTest"
		add_child(tester)


func _swap(node: Node) -> void:
	if _current != null:
		# Out of the tree now (not at the end of the frame): a world being replaced by a zone change must
		# stop handling messages before the next one arrives.
		remove_child(_current)
		_current.queue_free()
	_current = node
	add_child(node)


func _show_login(message: String) -> void:
	var screen: Control = LoginScreen.new()
	screen.name = "LoginScreen"
	_swap(screen)
	if message != "":
		screen.call("show_message", message, true)


func _show_char_select() -> void:
	var screen: Control = CharSelectScreen.new()
	screen.name = "CharSelectScreen"
	screen.connect("create_requested", _show_char_create)
	_swap(screen)


func _show_char_create() -> void:
	var screen: Control = CharCreateScreen.new()
	screen.name = "CharCreateScreen"
	screen.connect("finished", _show_char_select)
	_swap(screen)


func _on_entered_world(_info: Dictionary) -> void:
	var world: Node3D = World.new()
	world.name = "World"
	_swap(world)


func _on_session_ended(reason: String) -> void:
	_show_login(reason)


func current_screen() -> Node:
	return _current
