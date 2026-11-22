## Asks a server how it is doing without logging in (Phase 8): connect, Hello, read HelloResult and
## ServerStatus (message of the day, players online, server version), disconnect. Uses its own ENet host,
## so it never disturbs the real connection. Poll it every frame until `done`.
class_name ServerProbe
extends RefCounted

const Proto := preload("res://scripts/net/protocol.gd")
const TIMEOUT_MS: int = 4000

var done: bool = false
## online, version_ok, message, motd, players_online, max_players, server_version
var result: Dictionary = {}

var _host: ENetConnection = null
var _peer: ENetPacketPeer = null
var _deadline: int = 0
var _hello: Dictionary = {}


func start(address: String, port: int, client_build: String) -> void:
	_host = ENetConnection.new()
	if _host.create_host(1, Proto.CHANNEL_COUNT) != OK:
		_finish(false, "Could not open a network connection.")
		return
	_peer = _host.connect_to_host(address, port, Proto.CHANNEL_COUNT)
	if _peer == null:
		_finish(false, "That server address is not valid.")
		return
	_deadline = Time.get_ticks_msec() + TIMEOUT_MS
	set_meta("build", client_build)


func poll() -> void:
	if done or _host == null:
		return
	if Time.get_ticks_msec() > _deadline:
		_finish(false, "The server is not answering.")
		return
	for _i: int in 16:
		var ev: Array = _host.service(0)
		match int(ev[0]):
			ENetConnection.EVENT_NONE:
				return
			ENetConnection.EVENT_CONNECT:
				var hello: PackedByteArray = Proto.encode_hello({"protocol_version": Proto.PROTOCOL_VERSION, "client_build": str(get_meta("build", ""))})
				_peer.send(Proto.CHANNEL_RELIABLE, hello, ENetPacketPeer.FLAG_RELIABLE)
			ENetConnection.EVENT_DISCONNECT:
				if _hello.is_empty():
					_finish(false, "The server refused the connection.")
				else:
					_finish_from(_hello, {})
				return
			ENetConnection.EVENT_RECEIVE:
				var msg: Dictionary = Proto.decode((ev[1] as ENetPacketPeer).get_packet())
				if msg.is_empty():
					continue
				if int(msg["_id"]) == Proto.HELLO_RESULT:
					_hello = msg
				elif int(msg["_id"]) == Proto.SERVER_STATUS:
					_finish_from(_hello, msg)
					return
			ENetConnection.EVENT_ERROR:
				_finish(false, "Network error.")
				return


func _finish_from(hello: Dictionary, status: Dictionary) -> void:
	result = parse(hello, status)
	_close()
	done = true


## The probe's answer from a HelloResult (may be empty) and a ServerStatus (may be empty).
static func parse(hello: Dictionary, status: Dictionary) -> Dictionary:
	var r: Dictionary = {
		"online": true,
		"version_ok": bool(hello.get("ok", true)),
		"message": str(hello.get("message", "")),
		"motd": str(status.get("motd", "")),
		"players_online": int(status.get("players_online", -1)),
		"max_players": int(status.get("max_players", 0)),
		"server_version": str(status.get("server_version", "")),
	}
	return r


## One line for the login screen.
static func describe(r: Dictionary) -> String:
	if not bool(r.get("online", false)):
		return "Offline: %s" % str(r.get("message", ""))
	if not bool(r.get("version_ok", true)):
		return str(r.get("message", "This client does not match the server."))
	var players: int = int(r.get("players_online", -1))
	var line: String = "Online"
	if players >= 0:
		line += ": %d player%s" % [players, "" if players == 1 else "s"]
		if int(r.get("max_players", 0)) > 0:
			line += " of %d" % int(r["max_players"])
	if str(r.get("server_version", "")) != "":
		line += "  (server %s)" % str(r["server_version"])
	return line


func _finish(online: bool, message: String) -> void:
	result = {"online": online, "version_ok": false, "message": message, "motd": "", "players_online": -1, "max_players": 0, "server_version": ""}
	_close()
	done = true


func _close() -> void:
	if _peer != null and _peer.get_state() == ENetPacketPeer.STATE_CONNECTED:
		_peer.peer_disconnect_now()
	if _host != null:
		_host.flush()
		_host.destroy()
	_host = null
	_peer = null
