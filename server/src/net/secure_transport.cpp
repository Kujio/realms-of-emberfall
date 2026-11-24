#include "net/secure_transport.hpp"

#include <exception>

#include <spdlog/spdlog.h>

namespace ef::net {

void SecureTransport::send(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data, bool reliable) {
  if (failed_.count(peer) != 0) return;
  auto it = peers_.find(peer);
  if (it == peers_.end()) {
    inner_.send(peer, channel, data, reliable);
    return;
  }
  if (it->second->closing || channel >= SessionCrypto::kChannels) return;
  try {
    it->second->crypto.seal(channel, data, seal_buf_);
  } catch (const std::exception& e) {  // OpenSSL refusing AES-CTR: close this peer, not the server
    spdlog::error("peer {}: could not seal a packet ({}); closing the connection", peer, e.what());
    it->second->closing = true;
    inner_.disconnect_later(peer);
    return;
  }
  inner_.send(peer, channel, seal_buf_, reliable);
}

void SecureTransport::disconnect_later(PeerId peer) { inner_.disconnect_later(peer); }

void SecureTransport::enable_encryption(PeerId peer, const SessionKey& session_key) {
  try {
    peers_[peer] = std::make_unique<Peer>(session_key);
  } catch (const std::exception& e) {
    // Without a cipher nothing may go out unsealed: a peer left closing drops every send.
    spdlog::error("peer {}: could not set up encryption ({}); closing the connection", peer, e.what());
    peers_.erase(peer);
    inner_.disconnect_later(peer);
    failed_.insert(peer);
  }
}

void SecureTransport::on_connect(PeerId peer, const std::string& address) {
  peers_.erase(peer);  // ids are never reused, but a stale entry must never encrypt a new peer
  if (handler_) handler_->on_connect(peer, address);
}

void SecureTransport::on_disconnect(PeerId peer) {
  peers_.erase(peer);
  failed_.erase(peer);
  if (handler_) handler_->on_disconnect(peer);
}

void SecureTransport::on_packet(PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data) {
  if (!handler_ || failed_.count(peer) != 0) return;
  auto it = peers_.find(peer);
  if (it == peers_.end()) {
    handler_->on_packet(peer, channel, data);
    return;
  }
  Peer& p = *it->second;
  if (p.closing) return;
  const auto result = p.crypto.open(channel, data, open_buf_);
  if (result == PacketCipher::Open::Ok) {
    handler_->on_packet(peer, channel, open_buf_);
    return;
  }
  ++rejected_;
  const bool reliable = channel == 0;
  if (reliable || ++p.unreliable_failures >= kMaxUnreliableFailures) {
    spdlog::warn("peer {}: closing the connection after a packet failed its check ({} on channel {})", peer,
                 result == PacketCipher::Open::BadTag ? "bad tag" : result == PacketCipher::Open::Replay ? "replayed" : "malformed",
                 channel);
    p.closing = true;
    inner_.disconnect_later(peer);
  }
}

}  // namespace ef::net
