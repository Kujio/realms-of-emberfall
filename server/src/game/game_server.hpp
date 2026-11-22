// Session state machine and message handling. Owns zones; talks to the network through
// net::Transport and to storage through db::DbWorker. Single-threaded: every method runs on
// the game thread (DB completions are drained there too).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "auth/tickets.hpp"
#include "core/config.hpp"
#include "db/db_worker.hpp"
#include "game/auction.hpp"
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
  const AuctionHouse& auctions() const { return auctions_; }

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
    net::TokenBucket item_limiter{20.0, 40.0};    // item moves (dragging things around is quick)
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
  void handle(Session& s, const proto::PetCommand& m);
  void handle(Session& s, const proto::MoveItem& m);
  void handle(Session& s, const proto::DestroyItem& m);
  void handle(Session& s, const proto::UseItem& m);
  void handle(Session& s, const proto::VendorOpen& m);
  void handle(Session& s, const proto::VendorBuy& m);
  void handle(Session& s, const proto::VendorSell& m);
  void handle(Session& s, const proto::BankOpen& m);
  void handle(Session& s, const proto::BankCoin& m);
  void handle(Session& s, const proto::TradeRequest& m);
  void handle(Session& s, const proto::TradeRespond& m);
  void handle(Session& s, const proto::TradeSetItem& m);
  void handle(Session& s, const proto::TradeSetCoin& m);
  void handle(Session& s, const proto::TradeAccept& m);
  void handle(Session& s, const proto::TradeCancel& m);
  void handle(Session& s, const proto::AuctionOpen& m);
  void handle(Session& s, const proto::AuctionSearch& m);
  void handle(Session& s, const proto::AuctionPost& m);
  void handle(Session& s, const proto::AuctionBuy& m);
  void handle(Session& s, const proto::AuctionCancel& m);
  void handle(Session& s, const proto::LootRollChoice& m);
  void handle(Session& s, const proto::SetLootRule& m);
  void handle(Session& s, const proto::TravelOpen& m);
  void handle(Session& s, const proto::TravelChoose& m);

  void complete_login(Session& s, const db::AccountRecord& account);
  void request_char_list(Session& s);
  void on_enter_world(net::PeerId peer, const std::optional<db::CharacterRecord>& rec);
  // Builds what a zone needs to place a character from its stored (or snapshotted) record.
  ZonePlayerInit init_from_record(net::PeerId peer, const db::CharacterRecord& rec) const;
  void send_enter_result(Session& s, Zone& zone, const PlayerEntity& p, bool zone_change);
  // Zones (Phase 6): loaded on demand, unloaded when empty for cfg.zone_idle_seconds.
  Zone* ensure_zone(const std::string& id);
  void unload_idle_zones();
  // Moves every player the zones asked to move (zone lines, travel, portals, cross-zone respawns).
  void process_transfers();
  void transfer(Session& s, Zone& from, const ZoneTransfer& t);
  void leave_world(Session& s);
  void autosave();
  void load_corpses();
  void load_auctions();
  // The one save path for in-world characters and the auction board: every dirty character (all of them
  // with `all`), every character whose last save failed, and every listing changed since, in one
  // transaction. A failed batch is retried (from current state) with the next flush, so related changes
  // (both sides of a trade, a listing and its seller) are never stored apart.
  void flush_item_saves(bool all = false);
  void mark_dirty(const Session& s);
  // DB thread: commit a batch together with whatever failed before (carry_); on failure the whole merged
  // batch becomes the new carry. Returns true when everything is stored.
  bool commit_with_carry(db::Repository& repo, std::vector<db::CharacterRecord> chars, std::vector<db::AuctionRecord> up,
                         std::vector<std::int64_t> del);
  // Auctions: collect what sold or expired for this seller, then show the board.
  void collect_auctions(Session& s, Zone& z);
  void send_auction_page(Session& s, const AuctionQuery& q);
  void expire_auctions();
  std::optional<db::CharacterRecord> snapshot_in_world(const Session& s);
  Session* session_of_char(std::int64_t char_id);
  // A held-tighter gate for item intents (their own rate bucket).
  Zone* item_action(Session& s, const char* what);
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
  void zone_trade_completed(const PlayerEntity& a, const PlayerEntity& b) override;
  void zone_items_awarded(const PlayerEntity& p) override;

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
  std::unordered_map<std::string, std::uint32_t> zone_empty_since_;  // zone id -> tick it was last seen busy
  std::unordered_map<std::string, std::vector<CorpseInit>> parked_corpses_;  // corpses in zones not loaded
  std::vector<net::PeerId> pending_close_;
  proto::CreateOptions create_options_;
  AuctionHouse auctions_;
  bool auctions_loaded_ = false;
  std::set<std::int64_t> pending_auctions_;  // listing ids changed since the last flush (game thread)
  bool retry_saves_ = false;                  // a save batch failed: flush again even with nothing new (game thread)
  std::uint32_t auction_retry_tick_ = 0;      // a failed board load is retried at this tick
  bool auctions_loading_ = false;
  // Writes that failed and must go out with the next transaction, newest per character and listing.
  // Touched only by DB jobs, which run one at a time and in order.
  struct SaveCarry {
    std::map<std::int64_t, db::CharacterRecord> chars;
    std::map<std::int64_t, db::AuctionRecord> up;
    std::set<std::int64_t> del;
    bool empty() const { return chars.empty() && up.empty() && del.empty(); }
  };
  SaveCarry carry_;
  std::vector<std::uint8_t> send_buf_;
};

}  // namespace ef::game
