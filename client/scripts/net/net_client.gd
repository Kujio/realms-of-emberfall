## Autoload "Net": low-level ENet connection (ENetConnection, not the high-level multiplayer API)
## plus the generated protocol codec. Emits decoded messages as Dictionaries.
extends Node

signal connected
signal connection_failed(reason: String)
signal disconnected(reason: String)
signal message_received(msg: Dictionary)

const Proto := preload("res://scripts/net/protocol.gd")
const PacketCrypto := preload("res://scripts/net/packet_crypto.gd")
const CONNECT_TIMEOUT_MS: int = 6000

var _host: ENetConnection = null
var _peer: ENetPacketPeer = null
var _connecting: bool = false
var _connect_deadline: int = 0
var _last_reason: String = ""
var packets_in: int = 0
var packets_out: int = 0
var malformed_in: int = 0
## Phase 10: set by enable_encryption after the ticket login; seals and opens every packet.
var _crypto: PacketCrypto = null
var _sealed_seen: bool = false  # a sealed packet has arrived, so nothing unsealed is accepted
var rejected_in: int = 0


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
	_crypto = null
	_sealed_seen = false
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
	var wire: PackedByteArray = _crypto.seal(channel, packet) if _crypto != null else packet
	if _peer.send(channel, wire, flags) == OK:
		packets_out += 1


## From now on every packet both ways is sealed with the session key from the TLS login (Phase 10).
## Called right after sending TicketLogin: the server seals its AuthResult and all that follows.
func enable_encryption(session_key: PackedByteArray) -> void:
	_crypto = PacketCrypto.new(session_key, true)
	_sealed_seen = false


func is_encrypted() -> bool:
	return _crypto != null and _sealed_seen


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
	_crypto = null
	_sealed_seen = false


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
				if _crypto != null:
					data = _unseal(data, int(ev[3]))
				if _host == null:  # _unseal closed the connection
					running = false
				elif _crypto != null and data.is_empty():
					pass  # refused (counted in rejected_in)
				else:
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


## The plaintext of a sealed packet, or empty if it is refused. Before the first sealed packet, the
## messages the server sends unsealed may still arrive: a ServerStatus that ENet had to resend (it
## follows HelloResult, and the TLS login can finish first), a refused login's AuthResult, a Kick.
## A reliable packet that fails its check can only be tampering, so the connection is closed.
func _unseal(data: PackedByteArray, channel: int) -> PackedByteArray:
	var r: Array = _crypto.open(channel, data)
	if int(r[0]) == PacketCrypto.Open.OK:
		_sealed_seen = true
		return r[1]
	if not _sealed_seen:
		var msg: Dictionary = Proto.decode(data)
		if not msg.is_empty():
			var id: int = msg["_id"]
			if id == Proto.KICK or id == Proto.SERVER_STATUS or (id == Proto.AUTH_RESULT and not bool(msg["ok"])):
				return data
	rejected_in += 1
	if channel == Proto.CHANNEL_RELIABLE:
		close()
		disconnected.emit("The secure connection to the server failed. Please log in again.")
	return PackedByteArray()


func _remember_reason(msg: Dictionary) -> void:
	var id: int = msg["_id"]
	if id == Proto.KICK:
		_last_reason = str(msg["reason"])
	elif id == Proto.HELLO_RESULT and not bool(msg["ok"]):
		_last_reason = str(msg["message"])


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST or what == NOTIFICATION_PREDELETE:
		close()
