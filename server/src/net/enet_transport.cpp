#include "net/enet_transport.hpp"

#include <array>
#include <stdexcept>

#include <enet/enet.h>
#include <spdlog/spdlog.h>

namespace ef::net {

namespace {
std::string format_address(const ENetAddress& a) {
  std::array<char, 64> buf{};
  if (enet_address_get_host_ip(&a, buf.data(), buf.size()) != 0) return "unknown";
  return std::string(buf.data()) + ":" + std::to_string(a.port);
}
}  // namespace

EnetTransport::EnetTransport(const EnetConfig& cfg) : channels_(cfg.channels) {
  if (enet_initialize() != 0) throw std::runtime_error("enet_initialize failed");
  ENetAddress addr{};
  if (enet_address_set_host_ip(&addr, cfg.bind_address.c_str()) != 0) {
    enet_deinitialize();
    throw std::runtime_error("invalid bind address: " + cfg.bind_address);
  }
  addr.port = cfg.port;
  host_ = enet_host_create(&addr, cfg.max_peers, cfg.channels, 0, 0);
  if (host_ == nullptr) {
    enet_deinitialize();
    throw std::runtime_error("cannot bind UDP " + cfg.bind_address + ":" + std::to_string(cfg.port));
  }
  host_->maximumPacketSize = cfg.max_packet_size;
  host_->maximumWaitingData = static_cast<std::size_t>(cfg.max_packet_size) * 64u;
  host_->duplicatePeers = cfg.max_connections_per_ip;
}

EnetTransport::~EnetTransport() {
  if (host_) {
    for (auto& [id, peer] : peers_) enet_peer_reset(peer);
    enet_host_destroy(host_);
  }
  enet_deinitialize();
}

void EnetTransport::send(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool reliable) {
  auto it = peers_.find(peer);
  if (it == peers_.end() || channel >= channels_) return;
  const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
  ENetPacket* pkt = enet_packet_create(data.data(), data.size(), flags);
  if (pkt == nullptr) return;
  if (enet_peer_send(it->second, channel, pkt) != 0) enet_packet_destroy(pkt);
}

void EnetTransport::disconnect_later(PeerId peer) {
  auto it = peers_.find(peer);
  if (it != peers_.end()) enet_peer_disconnect_later(it->second, 0);
}

bool EnetTransport::dispatch(void* raw, TransportHandler& handler) {
  auto& ev = *static_cast<ENetEvent*>(raw);
  switch (ev.type) {
    case ENET_EVENT_TYPE_CONNECT: {
      const PeerId id = next_id_++;
      if (next_id_ == 0) next_id_ = 1;
      enet_peer_timeout(ev.peer, 0, 5000, 15000);
      peers_[id] = ev.peer;
      ids_[ev.peer] = id;
      handler.on_connect(id, format_address(ev.peer->address));
      return true;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
      auto it = ids_.find(ev.peer);
      if (it == ids_.end()) return true;
      const PeerId id = it->second;
      ids_.erase(it);
      peers_.erase(id);
      handler.on_disconnect(id);
      return true;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
      auto it = ids_.find(ev.peer);
      if (it != ids_.end()) {
        handler.on_packet(it->second, ev.channelID, std::span<const std::uint8_t>(ev.packet->data, ev.packet->dataLength));
      }
      enet_packet_destroy(ev.packet);
      return true;
    }
    case ENET_EVENT_TYPE_NONE:
      return false;
  }
  return false;
}

void EnetTransport::poll(std::uint32_t timeout_ms, TransportHandler& handler) {
  ENetEvent ev{};
  int rc = enet_host_service(host_, &ev, timeout_ms);
  // Bound the drain so a flood can't starve the tick; leftovers are handled next poll.
  for (int n = 0; rc > 0 && n < 4096; ++n) {
    dispatch(&ev, handler);
    rc = enet_host_service(host_, &ev, 0);
  }
  if (rc > 0) dispatch(&ev, handler);
  if (rc < 0) spdlog::warn("enet_host_service reported a socket error");
}

void EnetTransport::flush() { enet_host_flush(host_); }

}  // namespace ef::net
