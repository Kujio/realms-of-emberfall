#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "net/transport.hpp"

struct _ENetHost;  // NOLINT(bugprone-reserved-identifier): ENet's own type names
struct _ENetPeer;  // NOLINT(bugprone-reserved-identifier)

namespace ef::net {

struct EnetConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 7777;
  std::uint32_t max_peers = 256;
  std::uint32_t max_packet_size = 4096;
  std::uint32_t max_connections_per_ip = 8;
  std::uint8_t channels = 2;
};

class EnetTransport final : public Transport {
 public:
  // Throws std::runtime_error if ENet cannot initialize or bind.
  explicit EnetTransport(const EnetConfig& cfg);
  ~EnetTransport() override;

  void send(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool reliable) override;
  void disconnect_later(PeerId peer) override;
  // ENet itself does not encrypt: the game server talks to it through SecureTransport, which
  // seals packets before they get here. Called directly, this fails closed (disconnects the peer).
  void enable_encryption(PeerId peer, const std::array<std::uint8_t, 32>& session_key) override;

  // Waits up to timeout_ms for the first event, then drains every pending event.
  void poll(std::uint32_t timeout_ms, TransportHandler& handler);
  void flush();
  std::size_t peer_count() const { return peers_.size(); }
  // Bytes on the wire (ENet headers, acknowledgements and resends included) since the start.
  std::uint64_t bytes_sent() const { return bytes_sent_; }
  std::uint64_t bytes_received() const { return bytes_received_; }

 private:
  bool dispatch(void* raw, TransportHandler& handler);
  void harvest_counters();  // moves ENet's 32-bit counters into our 64-bit totals

  _ENetHost* host_ = nullptr;
  std::uint8_t channels_;
  PeerId next_id_ = 1;
  std::uint64_t bytes_sent_ = 0;
  std::uint64_t bytes_received_ = 0;
  std::unordered_map<PeerId, _ENetPeer*> peers_;
  std::unordered_map<const _ENetPeer*, PeerId> ids_;
};

}  // namespace ef::net
