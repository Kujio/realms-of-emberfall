## Phase 10: sealed game packets, the client's half of server/src/net/packet_crypto.hpp.
## The TLS login hands over a random 32-byte session key with the ticket; from the ticket login on,
## every ENet packet both ways is: counter (u64 LE) + AES-256-CTR ciphertext + 16-byte tag, the tag
## being the first 16 bytes of HMAC-SHA256 over counter and ciphertext. Each direction and channel
## has its own keys, derived as HMAC-SHA256(K, label + " enc"/" mac" + 0x01) from labels like
## "emberfall/1 c2s 0". Built from Godot's AESContext (ECB, to make the CTR keystream) and
## HMACContext; the tests check it against vectors made with the openssl command line.
extends RefCounted

const COUNTER_BYTES: int = 8
const TAG_BYTES: int = 16
const OVERHEAD: int = COUNTER_BYTES + TAG_BYTES
const CHANNELS: int = 2  # 0 reliable (in order), 1 unreliable (sequenced)

enum Open { OK, MALFORMED, BAD_TAG, REPLAY }


## One direction of one channel.
class Cipher extends RefCounted:
	var _aes: AESContext = AESContext.new()
	var _mac_key: PackedByteArray
	var _send_counter: int = 0
	var _recv_next: int = 0  # the lowest counter still acceptable

	func _init(key: PackedByteArray, purpose: String) -> void:
		_aes.start(AESContext.MODE_ECB_ENCRYPT, derive(key, purpose + " enc"))
		_mac_key = derive(key, purpose + " mac")

	static func derive(key: PackedByteArray, purpose: String) -> PackedByteArray:
		var h: HMACContext = HMACContext.new()
		h.start(HashingContext.HASH_SHA256, key)
		var data: PackedByteArray = purpose.to_utf8_buffer()
		data.append(1)
		h.update(data)
		return h.finish()

	## AES-256-CTR with the IV counter LE + 8 zero bytes, as OpenSSL counts it: block i is the IV
	## plus i as a big-endian 128-bit number (packets stay far below 2^16 blocks).
	func _crypt(counter: int, data: PackedByteArray) -> PackedByteArray:
		var n: int = data.size()
		if n == 0:
			return PackedByteArray()
		var blocks: int = (n + 15) / 16
		var ctr: PackedByteArray = PackedByteArray()
		ctr.resize(blocks * 16)
		for i: int in blocks:
			ctr.encode_u64(i * 16, counter)
			ctr[i * 16 + 14] = (i >> 8) & 0xff
			ctr[i * 16 + 15] = i & 0xff
		var ks: PackedByteArray = _aes.update(ctr)
		var out: PackedByteArray = data.duplicate()
		var whole: int = n - n % 8
		var k: int = 0
		while k < whole:
			out.encode_s64(k, data.decode_s64(k) ^ ks.decode_s64(k))
			k += 8
		while k < n:
			out[k] = data[k] ^ ks[k]
			k += 1
		return out

	func _tag(counter_and_ct: PackedByteArray) -> PackedByteArray:
		var h: HMACContext = HMACContext.new()
		h.start(HashingContext.HASH_SHA256, _mac_key)
		h.update(counter_and_ct)
		return h.finish().slice(0, TAG_BYTES)

	func seal(plain: PackedByteArray) -> PackedByteArray:
		var counter: int = _send_counter
		_send_counter += 1
		var out: PackedByteArray = PackedByteArray()
		out.resize(COUNTER_BYTES)
		out.encode_u64(0, counter)
		out.append_array(_crypt(counter, plain))
		out.append_array(_tag(out))
		return out

	## [Open, plaintext]. `in_order`: the counter must be exactly the next one (reliable channel);
	## otherwise higher than any accepted so far.
	func open(sealed: PackedByteArray, in_order: bool) -> Array:
		if sealed.size() < OVERHEAD:
			return [Open.MALFORMED, PackedByteArray()]
		var body: int = sealed.size() - TAG_BYTES
		var want: PackedByteArray = _tag(sealed.slice(0, body))
		var diff: int = 0
		for i: int in TAG_BYTES:  # every byte, so the time taken says nothing about the tag
			diff |= want[i] ^ sealed[body + i]
		if diff != 0:
			return [Open.BAD_TAG, PackedByteArray()]
		var counter: int = sealed.decode_u64(0)
		if (in_order and counter != _recv_next) or (not in_order and counter < _recv_next):
			return [Open.REPLAY, PackedByteArray()]
		_recv_next = counter + 1
		return [Open.OK, _crypt(counter, sealed.slice(COUNTER_BYTES, body))]


var _send: Array[Cipher] = []
var _recv: Array[Cipher] = []


static func label(client_to_server: bool, channel: int) -> String:
	return "emberfall/1 %s %d" % ["c2s" if client_to_server else "s2c", channel]


static func derive_key(key: PackedByteArray, label_text: String) -> PackedByteArray:
	return Cipher.derive(key, label_text)


## `client`: this end sends c2s (the game client); false plays the server (tests only).
func _init(key: PackedByteArray, client: bool = true) -> void:
	for ch: int in CHANNELS:
		_send.append(Cipher.new(key, label(client, ch)))
		_recv.append(Cipher.new(key, label(not client, ch)))


func seal(channel: int, plain: PackedByteArray) -> PackedByteArray:
	return _send[channel].seal(plain)


## [Open, plaintext].
func open(channel: int, sealed: PackedByteArray) -> Array:
	if channel < 0 or channel >= CHANNELS:
		return [Open.MALFORMED, PackedByteArray()]
	return _recv[channel].open(sealed, channel == 0)
