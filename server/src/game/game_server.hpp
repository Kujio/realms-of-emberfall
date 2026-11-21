// Session state machine and message handling. Owns zones; talks to the network through
// net::Transport and to storage through db::DbWorker. Single-threaded: every method runs on
// the game thread (DB completions are drained there too).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth/tickets.hpp"
#include "core/config.hpp"
#include "db/db_worker.hpp"
#include "game/content.hpp"
#include "game/zone.hpp"
#include "net/protocol.gen.hpp"
#include "net/rate_limiter.hpp"
#include "net/transport.hpp"

namespace ef::game {

// Returns the player-facing rejection message, or nullopt when versions match.
std::optional<std::string> protocol_mismatch_message(std::uint16_t client_version, std::uint16_t server_version);

class GameServer final : public net::TransportHandler, public ZoneSink {
 public:
  // `tickets` is shared with the TLS login service; `login_port`/`cert_pem` are advertised in
  // HelloResult so clients know where to log in and which certificate to pin.
  GameServer(const Config& cfg, const Content& content, net::Transport& transport, db::DbWorker& db,
             auth::TicketStore& tickets, std::uint16_t login_port, std::string cert_pem);
  ~GameServer() override;

  // net::TransportHandler
  void on_connect(net::PeerId peer, const std::string& address) override;
  void on_disconnect(net::PeerId peer) override;
  void on_packet(net::PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data) override;

  // Monotonic time in seconds, used for rate limiting.
  void set_time(double now_seconds) { now_ = now_seconds; }
  // One fixed simulation step.
  void tick();
  // Saves everyone, sends a Kick with `reason` to every session, and schedules disconnects.
  void shutdown(const std::string& reason);

  std::size_t session_count() const { return sessions_.size(); }
  std::size_t players_in_world() const;
  std::uint32_t server_tick() const { return tick_; }
  // Read-only access for tests and the health endpoint.
  Zone* zone(const std::string& id);

 private:
  enum class State { AwaitHello, AwaitAuth, CharSelect, EnteringWorld, InWorld };

  struct Session {
    Session(net::PeerId p, std::string addr, const Config& cfg)
        : peer(p), address(std::move(addr)),
          limiter(cfg.packets_per_second, cfg.packet_burst, cfg.bytes_per_second, cfg.byte_burst) {}
    net::PeerId peer;
    std::string address;
    State state = State::AwaitHello;
    net::PeerRateLimiter limiter;
    net::TokenBucket chat_limiter{1.0, 5.0};
    net::TokenBucket action_limiter{10.0, 20.0};  // combat/loot intents per second
    std::uint32_t violations = 0;
    std::uint32_t rate_drops = 0;
    std::uint32_t failed_logins = 0;
    std::uint32_t inflight = 0;  // outstanding DB requests
    bool closing = false;
    std::int64_t account_id = 0;
    std::string username;
    bool is_admin = false;
    std::int64_t char_id = 0;
    std::string char_name;
    std::string city;
    std::string zone_id;
    EntityId entity = 0;
    std::string nonce;  // binds TLS login tickets to this connection
  };

  // Handlers. The template catches server->client types, which a client must never send.
  template <class M>
  void handle(Session& s, const M& /*msg*/) {
    violation(s, "unexpected message type");
  }
  void handle(Session& s, const proto::Hello& m);
  void handle(Session& s, const proto::TicketLogin& m);
  void handle(Session& s, const proto::CharListRequest& m);
  void handle(Session& s, const proto::CharCreate& m);
  void handle(Session& s, const proto::CharDelete& m);
  void handle(Session& s, const proto::EnterWorld& m);
  void handle(Session& s, const proto::Logout& m);
  void handle(Session& s, const proto::MoveInput& m);
  void handle(Session& s, const proto::ChatSay& m);
  void handle(Session& s, const proto::SetTarget& m);
  void handle(Session& s, const proto::AutoAttack& m);
  void handle(Session& s, const proto::UseAbility& m);
  void handle(Session& s, const proto::RespawnRequest& m);
  void handle(Session& s, const proto::LootOpen& m);
  void handle(Session& s, const proto::LootTake& m);
  void handle(Session& s, const proto::LootClose& m);
  void handle(Session& s, const proto::SetSitting& m);
  void handle(Session& s, const proto::CastSpell& m);
  void handle(Session& s, const proto::StopCast& m);
  void handle(Session& s, const proto::MemorizeSpell& m);
  void handle(Session& s, const proto::ScribeSpell& m);
  void handle(Session& s, const proto::TrainerOpen& m);
  void handle(Session& s, const proto::TrainerBuy& m);
  void handle(Session& s, const proto::RezResponse& m);

  void complete_login(Session& s, const db::AccountRecord& account);
  void request_char_list(Session& s);
  void on_enter_world(net::PeerId peer, const std::optional<db::CharacterRecord>& rec);
  void leave_world(Session& s);
  void autosave();
  void load_corpses();
  // Gate for in-world intents: state check (violation if wrong) plus a soft rate limit.
  Zone* world_action(Session& s, const char* what);

  db::CharacterRecord snapshot(const Session& s, const PlayerEntity& p) const;
  db::CorpseRecord corpse_record(const std::string& zone_id, const CorpseEntity& c, const Zone& zone) const;
  void save_soon(const PlayerEntity& p);

  void violation(Session& s, const char* reason);
  void kick(Session& s, const std::string& reason);
  void close_silently(Session& s);
  void process_closes();
  Session* find_session(net::PeerId peer);
  Zone* zone_for(const Session& s);
  Zone* zone_of_player(const PlayerEntity& p, std::string* zone_id = nullptr);

  template <class M>
  void send(net::PeerId peer, const M& msg) {
    proto::encode_packet_into(msg, send_buf_);
    transport_.send(peer, M::kChannel, send_buf_, M::kChannel == proto::kChannelReliable);
  }

  // ZoneSink
  void zone_send(const PlayerEntity& to, std::uint8_t channel, std::span<const std::uint8_t> packet) override;
  void zone_correction(const PlayerEntity& to, const char* reason) override;
  void zone_violation(const PlayerEntity& who, const char* reason) override;
  void zone_player_died(const PlayerEntity& p, const CorpseEntity& corpse) override;
  void zone_corpse_looted(const CorpseEntity& corpse, const PlayerEntity& looter) override;
  void zone_corpse_removed(const CorpseEntity& corpse) override;
  void zone_character_changed(const PlayerEntity& p) override;
  void zone_corpse_resurrected(const CorpseEntity& corpse, const PlayerEntity& owner) override;

  const Config& cfg_;
  const Content& content_;
  net::Transport& transport_;
  db::DbWorker& db_;
  auth::TicketStore& tickets_;
  std::uint16_t login_port_;
  std::string cert_pem_;

  double now_ = 0.0;
  std::uint32_t tick_ = 0;
  std::uint32_t autosave_every_ticks_;
  float dt_;
  std::unordered_map<net::PeerId, Session> sessions_;
  std::unordered_map<std::int64_t, net::PeerId> online_accounts_;
  std::unordered_map<std::string, std::unique_ptr<Zone>> zones_;
  std::vector<net::PeerId> pending_close_;
  proto::CreateOptions create_options_;
  std::vector<std::uint8_t> send_buf_;
};

}  // namespace ef::game
