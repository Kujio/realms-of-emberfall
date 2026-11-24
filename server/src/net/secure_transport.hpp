// Seals and opens game packets for peers that have logged in (Phase 10). Sits between the ENet
// transport and the game server: the server sends through it, and ENet delivers to it.
//
//   GameServer --send--> SecureTransport --sealed--> EnetTransport
//   GameServer <--plain-- SecureTransport <--poll---- EnetTransport
//
// Until enable_encryption(peer) a peer's packets pass through as they are: the handshake (Hello,
// HelloResult, ServerStatus, TicketLogin and a failed AuthResult) carries nothing secret, and the
// game server's state machine accepts nothing else before login. After it, every packet both ways
// is sealed (net/packet_crypto.hpp); a packet that fails its check is dropped, and the connection
// is closed at once on the reliable channel (it can only be tampering) or after repeated failures
// on the unreliable one.
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/packet_crypto.hpp"
#include "net/transport.hpp"

namespace ef::net {

class SecureTransport final : public Transport, public TransportHandler {
 public:
  static constexpr std::uint32_t kMaxUnreliableFailures = 32;

  explicit SecureTransport(Transport& inner) : inner_(inner) {}
  // Where opened packets go; must be set before the inner transport is polled.
  void set_handler(TransportHandler& handler) { handler_ = &handler; }

  // Transport (the game server's side)
  void send(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool reliable) override;
  void disconnect_later(PeerId peer) override;
  void enable_encryption(PeerId peer, const SessionKey& session_key) override;

  // TransportHandler (the wire's side)
  void on_connect(PeerId peer, const std::string& address) override;
  void on_disconnect(PeerId peer) override;
  void on_packet(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data) override;

  bool encrypted(PeerId peer) const { return peers_.count(peer) != 0; }
  std::uint64_t rejected_packets() const { return rejected_; }

 private:
  struct Peer {
    explicit Peer(const SessionKey& k) : crypto(k, SessionCrypto::Role::Server) {}
    SessionCrypto crypto;
    std::uint32_t unreliable_failures = 0;
    bool closing = false;  // a bad packet closed it; anything still arriving is dropped
  };

  Transport& inner_;
  TransportHandler* handler_ = nullptr;
  std::unordered_map<PeerId, std::unique_ptr<Peer>> peers_;
  std::unordered_set<PeerId> failed_;  // encryption could not be set up: nothing in or out until closed
  // Reused buffers (the tick is single-threaded). Two of them: the game server answers a packet
  // while it is still handling it, and sealing the answer must not overwrite the plaintext.
  std::vector<std::uint8_t> seal_buf_;
  std::vector<std::uint8_t> open_buf_;
  std::uint64_t rejected_ = 0;
};

}  // namespace ef::net
