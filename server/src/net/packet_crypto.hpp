// Encryption for game traffic after login (Phase 10). The TLS login service hands the client a
// random 32-byte session key along with its ticket; from the ticket login on, every ENet packet in
// both directions is sealed with keys derived from it.
//
// Construction (encrypt-then-MAC from standard parts, so the Godot client can build it from
// AESContext and HMACContext; see DECISIONS.md "Game traffic encrypted"):
//   keys     per direction and channel: enc = HMAC-SHA256(K, label + " enc" + 0x01),
//            mac = HMAC-SHA256(K, label + " mac" + 0x01), label = "emberfall/1 c2s 0" etc.
//            (HKDF-Expand of the random session key, one block each).
//   packet   counter (u64 LE) || AES-256-CTR(enc, iv = counter LE || 8 zero bytes, plain) || tag
//   tag      the first 16 bytes of HMAC-SHA256(mac, counter || ciphertext)
// Each sender counts its own packets per channel from 0, so no (key, counter) pair repeats. A
// receiver checks the tag before decrypting, and refuses old counters: on the reliable channel the
// counter must be exactly the next one (ENet delivers reliable packets once and in order), on the
// unreliable channel it must be higher than any seen (ENet drops older sequenced packets anyway).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ef::net {

inline constexpr std::size_t kSessionKeyBytes = 32;
inline constexpr std::size_t kSealCounterBytes = 8;
inline constexpr std::size_t kSealTagBytes = 16;
inline constexpr std::size_t kSealOverhead = kSealCounterBytes + kSealTagBytes;

using SessionKey = std::array<std::uint8_t, kSessionKeyBytes>;

// A fresh key from the CSPRNG (libsodium).
SessionKey random_session_key();
// 64 lowercase hex characters, and back (false for anything that is not 64 hex digits).
std::string session_key_hex(const SessionKey& key);
bool parse_session_key_hex(std::string_view hex, SessionKey& out);

// HKDF-Expand(K, label, 32): HMAC-SHA256(K, label || 0x01).
std::array<std::uint8_t, 32> derive_key(const SessionKey& key, std::string_view label);

// One direction of one channel: seals with an increasing counter, or opens and checks the counter.
class PacketCipher {
 public:
  PacketCipher(const SessionKey& key, std::string_view label);
  ~PacketCipher();
  PacketCipher(const PacketCipher&) = delete;
  PacketCipher& operator=(const PacketCipher&) = delete;
  PacketCipher(PacketCipher&&) noexcept;
  PacketCipher& operator=(PacketCipher&&) noexcept;

  // Replaces `out` with the sealed packet.
  void seal(std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& out);

  enum class Open : std::uint8_t { Ok, Malformed, BadTag, Replay };
  // On Ok, replaces `out` with the plaintext. `in_order`: the counter must be exactly the next one
  // (the reliable channel); otherwise it must be higher than any accepted so far.
  Open open(std::span<const std::uint8_t> sealed, std::vector<std::uint8_t>& out, bool in_order);

  std::uint64_t next_send() const { return send_counter_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint64_t send_counter_ = 0;
  std::uint64_t recv_next_ = 0;  // the lowest counter still acceptable
};

// Both directions of both channels for one connection.
class SessionCrypto {
 public:
  enum class Role : std::uint8_t { Client, Server };
  static constexpr std::uint8_t kChannels = 2;  // 0 reliable, 1 unreliable (protocol.gen.hpp)

  SessionCrypto(const SessionKey& key, Role role);

  // Channels past kChannels are refused by the protocol layer before they get here.
  void seal(std::uint8_t channel, std::span<const std::uint8_t> plain, std::vector<std::uint8_t>& out);
  PacketCipher::Open open(std::uint8_t channel, std::span<const std::uint8_t> sealed, std::vector<std::uint8_t>& out);

 private:
  std::vector<PacketCipher> send_;
  std::vector<PacketCipher> recv_;
};

// "emberfall/1 c2s 0" and so on: the label both ends derive a direction's keys from.
std::string cipher_label(bool client_to_server, std::uint8_t channel);

}  // namespace ef::net
