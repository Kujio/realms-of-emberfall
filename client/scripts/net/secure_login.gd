## One TLS round trip to the server's login service: connect over TCP, verify the server presents
## exactly the pinned certificate, send one framed request, read one framed response.
## Passwords only ever travel through here, never over the ENet game connection.
extends RefCounted

signal finished(ok: bool, response: PackedByteArray, error: String)

## Must match the CN/SAN of the server's certificate (server/src/net/tls.hpp kTlsServerName).
const TLS_SERVER_NAME: String = "emberfall-server"
const TIMEOUT_MS: int = 20000
const MAX_FRAME: int = 4096

enum Phase { IDLE, CONNECTING, HANDSHAKING, AWAITING, DONE }

var _phase: Phase = Phase.IDLE
var _tcp: StreamPeerTCP = null
var _tls: StreamPeerTLS = null
var _cert: X509Certificate = null
var _deadline: int = 0
var _request: PackedByteArray = PackedByteArray()
var _buffer: PackedByteArray = PackedByteArray()


## SHA-256 over the certificate's DER, formatted like `openssl x509 -fingerprint -sha256`.
static func fingerprint(cert_pem: String) -> String:
	var body: String = ""
	var inside: bool = false
	for line: String in cert_pem.split("\n"):
		var l: String = line.strip_edges()
		if l.begins_with("-----BEGIN CERTIFICATE"):
			inside = true
		elif l.begins_with("-----END CERTIFICATE"):
			break
		elif inside:
			body += l
	if body.is_empty():
		return ""
	var der: PackedByteArray = Marshalls.base64_to_raw(body)
	var ctx: HashingContext = HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(der)
	var digest: PackedByteArray = ctx.finish()
	var parts: PackedStringArray = PackedStringArray()
	for b: int in digest:
		parts.append("%02X" % b)
	return ":".join(parts)


func start(ip: String, port: int, cert_pem: String, request_packet: PackedByteArray) -> Error:
	_cert = X509Certificate.new()
	var err: Error = _cert.load_from_string(cert_pem)
	if err != OK:
		return err
	_tcp = StreamPeerTCP.new()
	err = _tcp.connect_to_host(ip, port)
	if err != OK:
		return err
	_request = PackedByteArray()
	_request.resize(2)
	_request.encode_u16(0, request_packet.size())
	_request.append_array(request_packet)
	_buffer = PackedByteArray()
	_deadline = Time.get_ticks_msec() + TIMEOUT_MS
	_phase = Phase.CONNECTING
	return OK


func is_active() -> bool:
	return _phase != Phase.IDLE and _phase != Phase.DONE


func cancel() -> void:
	_close()
	_phase = Phase.DONE


## Drive the exchange; call once per frame while active.
func poll() -> void:
	if not is_active():
		return
	if Time.get_ticks_msec() > _deadline:
		_fail("The login server did not respond in time.")
		return
	match _phase:
		Phase.CONNECTING:
			_tcp.poll()
			var st: StreamPeerTCP.Status = _tcp.get_status()
			if st == StreamPeerTCP.STATUS_CONNECTED:
				_tls = StreamPeerTLS.new()
				var err: Error = _tls.connect_to_stream(_tcp, TLS_SERVER_NAME, TLSOptions.client(_cert))
				if err != OK:
					_fail("Secure login could not start (%s)." % error_string(err))
					return
				_phase = Phase.HANDSHAKING
			elif st == StreamPeerTCP.STATUS_ERROR or st == StreamPeerTCP.STATUS_NONE:
				_fail("Could not reach the login service on the server.")
		Phase.HANDSHAKING:
			_tls.poll()
			var st: StreamPeerTLS.Status = _tls.get_status()
			if st == StreamPeerTLS.STATUS_CONNECTED:
				if _tls.put_data(_request) != OK:
					_fail("Could not send the login request.")
					return
				_phase = Phase.AWAITING
			elif st != StreamPeerTLS.STATUS_HANDSHAKING:
				_fail("Secure login failed: the server's certificate did not match the one you trusted.")
		Phase.AWAITING:
			_tls.poll()
			var avail: int = _tls.get_available_bytes()
			if avail > 0:
				var res: Array = _tls.get_partial_data(avail)
				if int(res[0]) == OK:
					var chunk: PackedByteArray = res[1]
					_buffer.append_array(chunk)
			if _buffer.size() >= 2:
				var n: int = _buffer.decode_u16(0)
				if n == 0 or n > MAX_FRAME:
					_fail("The login server sent an invalid response.")
					return
				if _buffer.size() >= 2 + n:
					var payload: PackedByteArray = _buffer.slice(2, 2 + n)
					_close()
					_phase = Phase.DONE
					finished.emit(true, payload, "")
					return
			if _tls.get_status() != StreamPeerTLS.STATUS_CONNECTED:
				_fail("The login server closed the connection.")


func _fail(message: String) -> void:
	_close()
	_phase = Phase.DONE
	finished.emit(false, PackedByteArray(), message)


func _close() -> void:
	if _tls != null:
		_tls.disconnect_from_stream()
	elif _tcp != null:
		_tcp.disconnect_from_host()
	_tls = null
	_tcp = null
