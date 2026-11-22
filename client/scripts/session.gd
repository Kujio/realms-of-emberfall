## Autoload "Session": drives the login -> character select -> world flow over Net and keeps
## the account-level state the UI screens read.
extends Node

signal status_changed(text: String, is_error: bool)
signal auth_succeeded
signal char_list_updated
signal char_create_finished(ok: bool, message: String)
signal char_delete_finished(ok: bool, message: String)
signal entered_world(info: Dictionary)
signal enter_world_failed(message: String)
signal returned_to_select
signal session_ended(reason: String)
signal world_message(msg: Dictionary)
## First connection to a server: the UI must show the fingerprint and call trust_server().
signal trust_requested(server: String, fingerprint: String)
## Phase 8: the server's message of the day and population (after every handshake).
signal server_status_received(msg: Dictionary)
## Phase 8: a world is about to load (entering, or crossing into another zone): show the loading screen.
signal world_loading(zone_name: String)

const UI := preload("res://scripts/ui/ui.gd")
const Proto := preload("res://scripts/net/protocol.gd")
const SecureLogin := preload("res://scripts/net/secure_login.gd")
const SETTINGS_PATH: String = "user://settings.cfg"
const KNOWN_SERVERS_PATH: String = "user://known_servers.cfg"
const DEFAULT_PORT: int = 7777
const CLIENT_BUILD: String = "emberfall-client 0.1.0"

var server_address: String = "127.0.0.1:7777"
var last_username: String = ""
var username: String = ""
var characters: Array[Dictionary] = []
var max_characters: int = 4
var create_options: Dictionary = {}
var world_info: Dictionary = {}
var in_world: bool = false
var entering_char_id: int = -1
var authenticated: bool = false
var server_status: Dictionary = {}
## The last movement input sequence the world sent (carried across zone changes).
var input_seq: int = 0

var _pending_user: String = ""
var _pending_pass: String = ""
var _pending_register: bool = false
## Where certificate pins live (the autotest driver points this at its own file).
var known_servers_path: String = KNOWN_SERVERS_PATH
var _server_ip: String = ""
var _hello: Dictionary = {}
var _login: SecureLogin = null


func _ready() -> void:
	_load_settings()
	Net.connected.connect(_on_connected)
	Net.connection_failed.connect(_on_connection_failed)
	Net.disconnected.connect(_on_disconnected)
	Net.message_received.connect(_on_message)


## Parses "host:port" (port optional). Returns [host, port] or [] if invalid.
static func parse_address(text: String) -> Array:
	var t: String = text.strip_edges()
	if t.is_empty():
		return []
	var host: String = t
	var port: int = DEFAULT_PORT
	var colon: int = t.rfind(":")
	if colon > 0:
		host = t.substr(0, colon)
		var port_text: String = t.substr(colon + 1)
		if not port_text.is_valid_int():
			return []
		port = port_text.to_int()
	if port < 1 or port > 65535 or host.is_empty():
		return []
	return [host, port]


func login(address: String, user: String, password: String, register: bool) -> void:
	var parsed: Array = parse_address(address)
	if parsed.is_empty():
		status_changed.emit("Invalid server address. Use host or host:port.", true)
		return
	if user.strip_edges().is_empty() or password.is_empty():
		status_changed.emit("Enter a username and password.", true)
		return
	# Same limits the server enforces (server/src/auth/names.hpp); checked here for a quick answer.
	if user.strip_edges().length() < 3 or user.strip_edges().length() > 16:
		status_changed.emit("Username must be 3-16 characters.", true)
		return
	if password.length() < 8 or password.length() > 64:
		status_changed.emit("Password must be 8-64 characters.", true)
		return
	var host: String = str(parsed[0])
	var ip: String = host if host.is_valid_ip_address() else IP.resolve_hostname(host, IP.TYPE_ANY)
	if ip.is_empty():
		status_changed.emit("Could not resolve server address '%s'." % host, true)
		return
	server_address = address.strip_edges()
	last_username = user.strip_edges()
	_save_settings()
	_pending_user = last_username
	_pending_pass = password
	_pending_register = register
	authenticated = false
	in_world = false
	_server_ip = ip
	_cancel_login()
	status_changed.emit("Connecting to %s..." % server_address, false)
	var err: Error = Net.connect_to_server(ip, int(parsed[1]))
	if err != OK:
		status_changed.emit("Could not start connection (%s)." % error_string(err), true)


func request_char_list() -> void:
	Net.send(Proto.encode_char_list_request())


## gender: 0 = female, 1 = male (picks the model; see content/appearance.json).
func create_character(char_name: String, race: String, class_id: String, city: String, gender: int = 0) -> void:
	Net.send(Proto.encode_char_create({"name": char_name, "race": race, "class_id": class_id, "city": city,
		"gender": clampi(gender, 0, 1)}))


func delete_character(char_id: int) -> void:
	Net.send(Proto.encode_char_delete({"char_id": char_id}))


func enter_world(char_id: int) -> void:
	entering_char_id = char_id
	world_loading.emit("")
	Net.send(Proto.encode_enter_world({"char_id": char_id}))


## In world: return to character select. At character select: disconnect.
func logout() -> void:
	if in_world:
		Net.send(Proto.encode_logout())
	else:
		quit_session("Logged out.")


## Answer to trust_requested: pin the fingerprint and continue, or abort the login.
func trust_server(accept: bool) -> void:
	if _hello.is_empty():
		return
	if not accept:
		_pending_pass = ""
		Net.close()
		status_changed.emit("Login cancelled: server not trusted.", true)
		return
	_set_known_fingerprint(server_address, SecureLogin.fingerprint(str(_hello["cert_pem"])))
	_start_secure_login()


func quit_session(reason: String) -> void:
	_cancel_login()
	Net.close()
	in_world = false
	authenticated = false
	session_ended.emit(reason)


func race_name(id: String) -> String:
	return _option_name("races", id)


func class_name_for(id: String) -> String:
	return _option_name("classes", id)


func city_name(id: String) -> String:
	return _option_name("cities", id)


func _option_name(kind: String, id: String) -> String:
	var list: Array = create_options.get(kind, [])
	for entry: Dictionary in list:
		if str(entry.get("id", "")) == id:
			return str(entry.get("name", id))
	return id.capitalize()


func _on_connected() -> void:
	status_changed.emit("Connected. Checking version...", false)
	Net.send(Proto.encode_hello({"protocol_version": Proto.PROTOCOL_VERSION, "client_build": CLIENT_BUILD}))


func _on_connection_failed(reason: String) -> void:
	status_changed.emit(reason, true)


func _on_disconnected(reason: String) -> void:
	var was_active: bool = authenticated
	in_world = false
	authenticated = false
	if was_active:
		session_ended.emit(reason)
	else:
		status_changed.emit(reason, true)


func _on_message(msg: Dictionary) -> void:
	var id: int = msg["_id"]
	match id:
		Proto.HELLO_RESULT:
			if bool(msg["ok"]):
				_on_hello_ok(msg)
			else:
				status_changed.emit(str(msg["message"]), true)
		Proto.SERVER_STATUS:
			server_status = msg
			server_status_received.emit(msg)
		Proto.AUTH_RESULT:
			_pending_pass = ""
			if bool(msg["ok"]):
				UI.sound("login_ok")
				authenticated = true
				username = _pending_user
				auth_succeeded.emit()
			else:
				# Close before notifying, so a listener that immediately retries gets a fresh connection.
				Net.close()
				status_changed.emit(str(msg["message"]), true)
		Proto.CREATE_OPTIONS:
			create_options = msg
		Proto.CHAR_LIST:
			max_characters = int(msg["max_characters"])
			characters.clear()
			for c: Dictionary in msg["characters"]:
				characters.append(c)
			char_list_updated.emit()
		Proto.CHAR_CREATE_RESULT:
			char_create_finished.emit(bool(msg["ok"]), str(msg["message"]))
		Proto.CHAR_DELETE_RESULT:
			char_delete_finished.emit(bool(msg["ok"]), str(msg["message"]))
		Proto.ENTER_WORLD_RESULT:
			if bool(msg["ok"]):
				if bool(msg.get("zone_change", false)):
					world_loading.emit(str(msg.get("zone_name", "")))
				world_info = msg
				in_world = true
				entered_world.emit(msg)
			else:
				enter_world_failed.emit(str(msg["message"]))
		Proto.LOGOUT_RESULT:
			if in_world:
				in_world = false
				returned_to_select.emit()
		Proto.KICK:
			status_changed.emit(str(msg["reason"]), true)
		_:
			if in_world:
				world_message.emit(msg)


func _process(_delta: float) -> void:
	if _login != null:
		_login.poll()


# Checks the server certificate against the pin for this address (trust on first use, like SSH).
func _on_hello_ok(msg: Dictionary) -> void:
	_hello = msg
	var fp: String = SecureLogin.fingerprint(str(msg["cert_pem"]))
	if fp.is_empty():
		_abort_login("The server did not provide a valid certificate.")
		return
	var known: String = _known_fingerprint(server_address)
	if known.is_empty():
		status_changed.emit("New server: confirm its fingerprint to continue.", false)
		trust_requested.emit(server_address, fp)
	elif known != fp:
		_abort_login("WARNING: this server's identity has changed since you last connected (fingerprint %s). "
			% fp + "This can mean someone is intercepting your connection. If the server admin regenerated "
			+ "the certificate, remove %s from %s and try again." % [server_address, ProjectSettings.globalize_path(known_servers_path)])
	else:
		_start_secure_login()


func _start_secure_login() -> void:
	status_changed.emit("Signing in securely...", false)
	var request: PackedByteArray = Proto.encode_tls_login({
		"username": _pending_user, "password": _pending_pass, "is_register": _pending_register,
		"session_nonce": str(_hello["session_nonce"]),
	})
	_pending_pass = ""
	_cancel_login()
	_login = SecureLogin.new()
	_login.finished.connect(_on_secure_login_finished)
	var err: Error = _login.start(_server_ip, int(_hello["login_port"]), str(_hello["cert_pem"]), request)
	if err != OK:
		_abort_login("Secure login could not start (%s)." % error_string(err))


func _on_secure_login_finished(ok: bool, response: PackedByteArray, error: String) -> void:
	_login = null
	if not ok:
		_abort_login(error)
		return
	var r: Dictionary = Proto.decode(response)
	if r.is_empty() or int(r["_id"]) != Proto.TLS_LOGIN_RESULT:
		_abort_login("The login server sent an invalid response.")
		return
	if not bool(r["ok"]):
		_abort_login(str(r["message"]))
		return
	Net.send(Proto.encode_ticket_login({"ticket": str(r["ticket"])}))


# Close before notifying, so a listener that immediately retries gets a fresh connection.
func _abort_login(message: String) -> void:
	_pending_pass = ""
	_cancel_login()
	Net.close()
	status_changed.emit(message, true)


func _cancel_login() -> void:
	if _login != null:
		_login.cancel()
		_login = null


func _known_fingerprint(server: String) -> String:
	var cfg: ConfigFile = ConfigFile.new()
	if cfg.load(known_servers_path) != OK:
		return ""
	return str(cfg.get_value("fingerprints", server.replace(":", "_"), ""))


func _set_known_fingerprint(server: String, fp: String) -> void:
	var cfg: ConfigFile = ConfigFile.new()
	cfg.load(known_servers_path)
	cfg.set_value("fingerprints", server.replace(":", "_"), fp)
	cfg.save(known_servers_path)


func _load_settings() -> void:
	var cfg: ConfigFile = ConfigFile.new()
	if cfg.load(SETTINGS_PATH) == OK:
		server_address = str(cfg.get_value("login", "server", server_address))
		last_username = str(cfg.get_value("login", "username", ""))


func _save_settings() -> void:
	var cfg: ConfigFile = ConfigFile.new()
	cfg.set_value("login", "server", server_address)
	cfg.set_value("login", "username", last_username)
	cfg.save(SETTINGS_PATH)
