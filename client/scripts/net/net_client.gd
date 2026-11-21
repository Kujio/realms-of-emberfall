## Autoload "Net": low-level ENet connection (ENetConnection, not the high-level multiplayer API)
## plus the generated protocol codec. Emits decoded messages as Dictionaries.
extends Node

signal connected
signal connection_failed(reason: String)
signal disconnected(reason: String)
signal message_received(msg: Dictionary)

const Proto := preload("res://scripts/net/protocol.gd")
const CONNECT_TIMEOUT_MS: int = 6000

var _host: ENetConnection = null
var _peer: ENetPacketPeer = null
var _connecting: bool = false
var _connect_deadline: int = 0
var _last_reason: String = ""
var packets_in: int = 0
var packets_out: int = 0
var malformed_in: int = 0


func connect_to_server(address: String, port: int) -> Error:
	close()
	_host = ENetConnection.new()
	var err: Error = _host.create_host(1, Proto.CHANNEL_COUNT)
	if err != OK:
		_host = null
		return err
	_peer = _host.connect_to_host(address, port, Proto.CHANNEL_COUNT)
	if _peer == null:
		_host.destroy()
		_host = null
		return ERR_CANT_CONNECT
	_connecting = true
	_connect_deadline = Time.get_ticks_msec() + CONNECT_TIMEOUT_MS
	_last_reason = ""
	return OK


func is_online() -> bool:
	return _peer != null and not _connecting and _peer.get_state() == ENetPacketPeer.STATE_CONNECTED


func round_trip_ms() -> int:
	if not is_online():
		return -1
	return int(_peer.get_statistic(ENetPacketPeer.PEER_ROUND_TRIP_TIME))


## Sends an encoded packet on the channel the protocol assigns to its message id.
func send(packet: PackedByteArray) -> void:
	if _peer == null or packet.size() < 2:
		return
	var id: int = packet.decode_u16(0)
	var channel: int = int(Proto.CHANNELS.get(id, Proto.CHANNEL_RELIABLE))
	var flags: int = ENetPacketPeer.FLAG_RELIABLE if channel == Proto.CHANNEL_RELIABLE else 0
	if _peer.send(channel, packet, flags) == OK:
		packets_out += 1


## Graceful disconnect (flushes queued packets first).
func close() -> void:
	if _peer != null and _peer.get_state() == ENetPacketPeer.STATE_CONNECTED:
		_peer.peer_disconnect()
		if _host != null:
			_host.flush()
	if _host != null:
		_host.destroy()
	_host = null
	_peer = null
	_connecting = false


func _process(_delta: float) -> void:
	if _host == null:
		return
	if _connecting and Time.get_ticks_msec() > _connect_deadline:
		close()
		connection_failed.emit("Could not reach the server. Check the address and that the server is running.")
		return
	var running: bool = true
	while running and _host != null:
		var ev: Array = _host.service(0)
		var kind: int = ev[0]
		match kind:
			ENetConnection.EVENT_NONE:
				running = false
			ENetConnection.EVENT_CONNECT:
				_connecting = false
				connected.emit()
			ENetConnection.EVENT_DISCONNECT:
				var was_connecting: bool = _connecting
				_host.destroy()
				_host = null
				_peer = null
				_connecting = false
				running = false
				if was_connecting:
					connection_failed.emit("The server refused the connection.")
				else:
					disconnected.emit(_last_reason if _last_reason != "" else "Connection to the server was lost.")
			ENetConnection.EVENT_RECEIVE:
				var peer: ENetPacketPeer = ev[1]
				var data: PackedByteArray = peer.get_packet()
				packets_in += 1
				var msg: Dictionary = Proto.decode(data)
				if msg.is_empty():
					malformed_in += 1
					push_warning("Dropped malformed packet (%d bytes)" % data.size())
				else:
					_remember_reason(msg)
					message_received.emit(msg)
			ENetConnection.EVENT_ERROR:
				close()
				running = false
				disconnected.emit("Network error.")


func _remember_reason(msg: Dictionary) -> void:
	var id: int = msg["_id"]
	if id == Proto.KICK:
		_last_reason = str(msg["reason"])
	elif id == Proto.HELLO_RESULT and not bool(msg["ok"]):
		_last_reason = str(msg["message"])


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST or what == NOTIFICATION_PREDELETE:
		close()
