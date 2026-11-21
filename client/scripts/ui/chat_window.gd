## Basic chat window (say channel only in Phase 1). Enter focuses input; Esc cancels.
extends PanelContainer

const Proto := preload("res://scripts/net/protocol.gd")
const CHANNEL_SAY: int = 0
const SAY_COLOR: Color = Color(0.95, 0.95, 0.95)
const SYSTEM_COLOR: Color = Color(1.0, 0.82, 0.4)

var my_name: String = ""
var lines: Array[String] = []
var _log: RichTextLabel
var _input: LineEdit


func _ready() -> void:
	var style: StyleBoxFlat = StyleBoxFlat.new()
	style.bg_color = Color(0.0, 0.0, 0.0, 0.45)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(6)
	add_theme_stylebox_override("panel", style)
	var box: VBoxContainer = VBoxContainer.new()
	add_child(box)
	_log = RichTextLabel.new()
	_log.scroll_following = true
	_log.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_log.mouse_filter = Control.MOUSE_FILTER_PASS
	box.add_child(_log)
	_input = LineEdit.new()
	_input.placeholder_text = "Press Enter to speak (say)"
	_input.max_length = 200
	_input.text_submitted.connect(_on_submit)
	_input.gui_input.connect(_on_input_gui)
	box.add_child(_input)


func is_typing() -> bool:
	return _input.has_focus()


func focus_input() -> void:
	_input.grab_focus()


func say(text: String) -> void:
	var t: String = text.strip_edges()
	if t != "":
		Net.send(Proto.encode_chat_say({"text": t}))


func add_message(channel: int, from_name: String, text: String) -> void:
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
