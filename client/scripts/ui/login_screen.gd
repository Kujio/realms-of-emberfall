## Login / register screen with an editable server address.
extends Control

const UI := preload("res://scripts/ui/ui.gd")

var _address: LineEdit
var _user: LineEdit
var _pass: LineEdit
var _status: Label
var _buttons: Array[Button] = []
var _trust: ConfirmationDialog


func _ready() -> void:
	UI.backdrop(self)
	var box: VBoxContainer = UI.centered_panel(self, 380)
	UI.title(box, "Realms of Emberfall", 34)
	var sub: Label = UI.label(box, "Phase 1 test client")
	sub.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_address = UI.field(box, "Server address", Session.server_address)
	_user = UI.field(box, "Username", Session.last_username)
	_pass = UI.field(box, "Password", "")
	_pass.secret = true
	_pass.text_submitted.connect(func(_t: String) -> void: _submit(false))
	var buttons: HBoxContainer = UI.row(box)
	_buttons.append(UI.button(buttons, "Log In", func() -> void: _submit(false)))
	_buttons.append(UI.button(buttons, "Register", func() -> void: _submit(true)))
	_buttons.append(UI.button(buttons, "Quit", func() -> void: get_tree().quit()))
	_status = UI.label(box, "")
	_status.custom_minimum_size = Vector2(0, 48)
	Session.status_changed.connect(_on_status)
	_trust = ConfirmationDialog.new()
	_trust.title = "Trust this server?"
	_trust.ok_button_text = "Trust and log in"
	_trust.confirmed.connect(func() -> void: Session.trust_server(true))
	_trust.canceled.connect(func() -> void: Session.trust_server(false))
	add_child(_trust)
	# The autotest driver answers trust prompts itself.
	if not OS.get_cmdline_user_args().has("--autotest"):
		Session.trust_requested.connect(_on_trust_requested)
	if _user.text.is_empty():
		_user.grab_focus()
	else:
		_pass.grab_focus()


func show_message(text: String, is_error: bool) -> void:
	UI.set_status(_status, text, is_error)


func _on_trust_requested(server: String, fingerprint: String) -> void:
	_trust.dialog_text = ("This is your first time connecting to %s.\n\n"
		+ "Check that this fingerprint matches the one the server admin published\n"
		+ "(the server prints it at startup):\n\n%s\n\n"
		+ "Your password is sent only after you trust it, and only over an encrypted connection.") % [server, fingerprint]
	_trust.popup_centered()


func _submit(register: bool) -> void:
	_set_busy(true)
	Session.login(_address.text, _user.text, _pass.text, register)


func _on_status(text: String, is_error: bool) -> void:
	show_message(text, is_error)
	if is_error:
		_set_busy(false)


func _set_busy(busy: bool) -> void:
	for b: Button in _buttons:
		if b.text != "Quit":
			b.disabled = busy


## Used by the autotest driver.
func autotest_login(address: String, user: String, password: String, register: bool) -> void:
	_address.text = address
	_user.text = user
	_pass.text = password
	_submit(register)
