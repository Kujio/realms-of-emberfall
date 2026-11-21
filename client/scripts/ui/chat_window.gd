## Chat window with two tabs: Chat (say, system) and Combat (the combat log). Enter focuses the
## input; Esc cancels. Lines starting with "/" are commands, handed to the world via `command`.
extends PanelContainer

signal command(name: String, args: String)

const Proto := preload("res://scripts/net/protocol.gd")
const CHANNEL_SAY: int = 0
const CHANNEL_SYSTEM: int = 1
const CHANNEL_COMBAT: int = 2
const SAY_COLOR: Color = Color(0.95, 0.95, 0.95)
const SYSTEM_COLOR: Color = Color(1.0, 0.82, 0.4)
const COMBAT_LIMIT: int = 400

var my_name: String = ""
var lines: Array[String] = []         # chat tab, for tests and the autotest report
var combat_lines: Array[String] = []  # combat tab
var _tabs: TabContainer
var _log: RichTextLabel
var _combat: RichTextLabel
var _input: LineEdit


func _ready() -> void:
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.0, 0.0, 0.0, 0.45)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(6)
	add_theme_stylebox_override("panel", style)
	var box: VBoxContainer = VBoxContainer.new()
	add_child(box)
	_tabs = TabContainer.new()
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_tabs)
	_log = _make_log("Chat")
	_combat = _make_log("Combat")
	_input = LineEdit.new()
	_input.placeholder_text = "Press Enter to speak (say), or /sit /stand /loc /con"
	_input.max_length = 200
	_input.text_submitted.connect(_on_submit)
	_input.gui_input.connect(_on_input_gui)
	box.add_child(_input)


func _make_log(title: String) -> RichTextLabel:
	var l: RichTextLabel = RichTextLabel.new()
	l.name = title
	l.scroll_following = true
	l.size_flags_vertical = Control.SIZE_EXPAND_FILL
	l.mouse_filter = Control.MOUSE_FILTER_PASS
	_tabs.add_child(l)
	return l


func is_typing() -> bool:
	return _input.has_focus()


func focus_input() -> void:
	_input.grab_focus()


func say(text: String) -> void:
	var t: String = text.strip_edges()
	if t == "":
		return
	if t.begins_with("/"):
		var parts: PackedStringArray = t.substr(1).split(" ", false, 1)
		if parts.size() > 0:
			command.emit(parts[0].to_lower(), parts[1] if parts.size() > 1 else "")
		return
	Net.send(Proto.encode_chat_say({"text": t}))


func add_message(channel: int, from_name: String, text: String) -> void:
	if channel == CHANNEL_COMBAT:
		add_combat(text, Color(0.85, 0.85, 0.7))
		return
	var line: String
	if channel == CHANNEL_SAY:
		if from_name == my_name:
			line = "You say, '%s'" % text
		else:
			line = "%s says, '%s'" % [from_name, text]
		_log.push_color(SAY_COLOR)
	else:
		line = text
		_log.push_color(SYSTEM_COLOR)
	_log.add_text(line)
	_log.pop()
	_log.newline()
	lines.append(line)


func add_system(text: String) -> void:
	add_message(CHANNEL_SYSTEM, "", text)


func add_combat(text: String, color: Color) -> void:
	_combat.push_color(color)
	_combat.add_text(text)
	_combat.pop()
	_combat.newline()
	combat_lines.append(text)
	if combat_lines.size() > COMBAT_LIMIT:
		combat_lines.pop_front()
		_combat.remove_paragraph(0)


func show_tab(index: int) -> void:
	_tabs.current_tab = index


func _on_submit(text: String) -> void:
	say(text)
	_input.clear()
	_input.release_focus()


func _on_input_gui(event: InputEvent) -> void:
	var k: InputEventKey = event as InputEventKey
	if k != null and k.pressed and k.keycode == KEY_ESCAPE:
		_input.clear()
		_input.release_focus()
		_input.accept_event()
