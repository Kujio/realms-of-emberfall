// Transport abstraction so the game server can be driven by ENet in production and by an
// in-memory fake in tests.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ef::net {

using PeerId = std::uint32_t;

class Transport {
 public:
  virtual ~Transport() = default;
  Transport() = default;
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  virtual void send(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool reliable) = 0;
  // Disconnects after already-queued reliable packets are delivered.
  virtual void disconnect_later(PeerId peer) = 0;
};

class TransportHandler {
 public:
  virtual ~TransportHandler() = default;
  TransportHandler() = default;
  TransportHandler(const TransportHandler&) = delete;
  TransportHandler& operator=(const TransportHandler&) = delete;

  virtual void on_connect(PeerId peer, const std::string& address) = 0;
  virtual void on_disconnect(PeerId peer) = 0;
  virtual void on_packet(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data) = 0;
};

}  // namespace ef::net
