## Login / register screen with an editable server address, the server's status (players online,
## message of the day) and the settings window.
extends Control

const UI := preload("res://scripts/ui/ui.gd")
const Probe := preload("res://scripts/net/server_probe.gd")
const SettingsWindow := preload("res://scripts/ui/settings_window.gd")
const PROBE_EVERY_S: float = 30.0

var _address: LineEdit
var _user: LineEdit
var _pass: LineEdit
var _status: Label
var _buttons: Array[Button] = []
var _trust: ConfirmationDialog
var _server_line: Label
var _motd: Label
var _probe: Probe = null
var _probe_address: String = ""  # the address the running probe asked
var _probe_in: float = 0.0  # seconds until the next status check
var _settings: Control = null


func _ready() -> void:
	UI.backdrop(self)
	var box: VBoxContainer = UI.centered_panel(self, 400)
	UI.title(box, "Realms of Emberfall", 34)
	var sub: Label = UI.label(box, "Version %s" % str(ProjectSettings.get_setting("application/config/version", "")))
	sub.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_address = UI.field(box, "Server address", Session.server_address)
	_address.text_changed.connect(_on_address_changed)
	_server_line = UI.label(box, "Checking the server...")
	_server_line.add_theme_font_size_override("font_size", 13)
	_motd = UI.label(box, "")
	_motd.add_theme_color_override("font_color", Color(0.95, 0.85, 0.6))
	_motd.custom_minimum_size = Vector2(0, 0)
	_user = UI.field(box, "Username", Session.last_username)
	_pass = UI.field(box, "Password", "")
	_pass.secret = true
	_pass.text_submitted.connect(func(_t: String) -> void: _submit(false))
	var buttons: HBoxContainer = UI.row(box)
	_buttons.append(UI.button(buttons, "Log In", func() -> void: _submit(false)))
	_buttons.append(UI.button(buttons, "Register", func() -> void: _submit(true)))
	var more: HBoxContainer = UI.row(box)
	UI.button(more, "Settings", _open_settings)
	UI.button(more, "Quit", func() -> void: get_tree().quit())
	_status = UI.label(box, "")
	_status.custom_minimum_size = Vector2(0, 48)
	Session.status_changed.connect(_on_status)
	Session.server_status_received.connect(_on_server_status)
	_trust = ConfirmationDialog.new()
	_trust.title = "Trust this server?"
	_trust.ok_button_text = "Trust and log in"
	_trust.confirmed.connect(func() -> void: Session.trust_server(true))
	_trust.canceled.connect(func() -> void: Session.trust_server(false))
	add_child(_trust)
	# The autotest driver answers trust prompts itself, and checks the server on its own schedule.
	var autotest: bool = OS.get_cmdline_user_args().has("--autotest")
	if not autotest:
		Session.trust_requested.connect(_on_trust_requested)
	_probe_in = 0.1
	if _user.text.is_empty():
		_user.grab_focus()
	else:
		_pass.grab_focus()
	UI.sound_loop("ambience", true)


func _exit_tree() -> void:
	UI.sound_loop("ambience", false)


func _process(delta: float) -> void:
	if _probe != null:
		_probe.poll()
		if _probe.done:
			if _probe_address == _address.text.strip_edges():
				_show_server(_probe.result)
				_probe_in = PROBE_EVERY_S
			else:
				_probe_in = 0.3  # the address changed meanwhile: ask the new one instead
			_probe = null
		return
	_probe_in -= delta
	if _probe_in <= 0.0 and not Net.is_online():
		check_server()


## Starts a status check of the address in the field (also used by the autotest driver).
func check_server() -> void:
	var parsed: Array = Session.parse_address(_address.text)
	if parsed.is_empty():
		_server_line.text = "Enter a server address (host or host:port)."
		_probe_in = PROBE_EVERY_S
		return
	_probe = Probe.new()
	_probe_address = _address.text.strip_edges()
	_probe.start(str(parsed[0]), int(parsed[1]), Session.CLIENT_BUILD)


func _on_address_changed(_text: String) -> void:
	_probe_in = 0.8  # check again once typing pauses
	_server_line.text = "Checking the server..."
	_server_line.remove_theme_color_override("font_color")
	_motd.text = ""


func server_line() -> String:
	return _server_line.text


func motd() -> String:
	return _motd.text


func _show_server(r: Dictionary) -> void:
	_server_line.text = Probe.describe(r)
	var ok: bool = bool(r.get("online", false)) and bool(r.get("version_ok", true))
	_server_line.add_theme_color_override("font_color", UI.OK_COLOR if ok else UI.ERROR_COLOR)
	_motd.text = str(r.get("motd", ""))


func _on_server_status(msg: Dictionary) -> void:
	# From the login connection: only if it went to the address now in the field.
	if Session.server_address.strip_edges() != _address.text.strip_edges():
		return
	_show_server(Probe.parse({"ok": true}, msg))


func show_message(text: String, is_error: bool) -> void:
	UI.set_status(_status, text, is_error)


func _open_settings() -> void:
	UI.sound("ui_open")
	if _settings == null:
		_settings = SettingsWindow.new()
		add_child(_settings)
	_settings.visible = true


func _on_trust_requested(server: String, fingerprint: String) -> void:
	_trust.dialog_text = ("This is your first time connecting to %s.\n\n"
		+ "Check that this fingerprint matches the one the server admin published\n"
		+ "(the server prints it at startup):\n\n%s\n\n"
		+ "Your password is sent only after you trust it, and only over an encrypted connection.") % [server, fingerprint]
	_trust.popup_centered()


func _submit(register: bool) -> void:
	UI.sound("ui_click")
	_set_busy(true)
	Session.login(_address.text, _user.text, _pass.text, register)


func _on_status(text: String, is_error: bool) -> void:
	show_message(text, is_error)
	if is_error:
		UI.sound("ui_error")
		_set_busy(false)


func _set_busy(busy: bool) -> void:
	for b: Button in _buttons:
		b.disabled = busy


## Used by the autotest driver.
func autotest_login(address: String, user: String, password: String, register: bool) -> void:
	_address.text = address
	_user.text = user
	_pass.text = password
	_submit(register)
