#include "game/game_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

#include <spdlog/spdlog.h>

#include "auth/bans.hpp"
#include "auth/names.hpp"

namespace ef::game {

namespace {

proto::CharSummary to_summary(const db::CharacterRecord& r) {
  proto::CharSummary s;
  s.id = static_cast<std::uint64_t>(r.id);
  s.name = r.name;
  s.race = r.race;
  s.class_id = r.class_id;
  s.city = r.city;
  s.level = static_cast<std::uint8_t>(std::clamp(r.level, 1, 255));
  s.zone = r.zone;
  s.gender = static_cast<std::uint8_t>(std::clamp(r.gender, 0, 1));
  return s;
}

bool contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

constexpr std::uint32_t kMaxInflightDb = 4;

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A pinned seed gives every zone its own stream, by zone id (stable as zones are added).
std::uint64_t seed_for(const Config& cfg, const std::string& zone_id) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char ch : zone_id) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
  if (cfg.rng_seed != 0) return cfg.rng_seed + h * 0x9E3779B97F4A7C15ULL;
  std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ h;
}

std::vector<db::StoredItem> stored_items(const std::vector<CorpseItem>& items) {
  std::vector<db::StoredItem> out;
  for (const auto& it : items)
    if (!it.empty()) out.push_back({static_cast<int>(it.origin), it.stack.item_id, it.stack.count, 0.0f});
  return out;
}

std::int64_t random_id() {
  static std::mt19937_64 rng{std::random_device{}()};
  return static_cast<std::int64_t>((rng() >> 2) | 1ULL);
}

AuctionListing to_listing(const db::AuctionRecord& r) {
  AuctionListing l;
  l.id = r.id;
  l.seller_char = r.seller_char;
  l.seller_name = r.seller_name;
  l.item_id = r.item_id;
  l.count = r.count;
  l.price = r.price;
  l.listed_at = r.listed_at;
  l.expires_at = r.expires_at;
  l.state = static_cast<ListingState>(std::clamp(r.state, 0, 2));
  l.buyer = r.buyer;
  return l;
}

db::AuctionRecord to_record(const AuctionListing& l) {
  return {l.id, l.seller_char, l.seller_name, l.item_id, l.count, l.price, l.listed_at, l.expires_at, static_cast<int>(l.state), l.buyer};
}

}  // namespace

std::optional<std::string> protocol_mismatch_message(std::uint16_t client_version, std::uint16_t server_version) {
  if (client_version == server_version) return std::nullopt;
  if (client_version < server_version) {
    return "Your client is out of date (protocol v" + std::to_string(client_version) + ", server requires v" +
           std::to_string(server_version) + "). Please update your client.";
  }
  return "Your client is newer than this server (protocol v" + std::to_string(client_version) + ", server runs v" +
         std::to_string(server_version) + "). Please use a matching client or wait for the server update.";
}

GameServer::GameServer(const Config& cfg, const Content& content, net::Transport& transport, db::DbWorker& db,
                       auth::TicketStore& tickets, std::uint16_t login_port, std::string cert_pem)
    : cfg_(cfg), content_(content), transport_(transport), db_(db), tickets_(tickets), login_port_(login_port),
      cert_pem_(std::move(cert_pem)),
      autosave_every_ticks_(std::max<std::uint32_t>(1, cfg.autosave_seconds * cfg.tick_rate)),
      dt_(1.0f / static_cast<float>(cfg.tick_rate)), groups_(content.social.group_max), filter_(make_filter(content.social)) {
  {
    std::string part;
    for (char ch : cfg_.start_zone + ",") {
      if (ch != ',') {
        if (ch != ' ') part += ch;
      } else if (!part.empty()) {
        start_zones_.push_back(part);
        part.clear();
      }
    }
  }
  for (const auto& z : start_zones_)
    if (!content_.find_zone(z)) throw std::runtime_error("start_zone '" + z + "' is not a zone in the content");
  for (const auto& z : content_.zones)
    if (z.persistent) ensure_zone(z.id);
  load_corpses();
  load_auctions();
  load_guilds();

  for (const auto& r : content_.races) {
    proto::RaceOption o;
    o.id = r.id;
    o.name = r.name;
    o.description = r.description;
    o.classes = r.allowed_classes;
    o.cities = r.cities;
    create_options_.races.push_back(std::move(o));
  }
  for (const auto& c : content_.classes) create_options_.classes.push_back({c.id, c.name, c.description});
  for (const auto& c : content_.cities) create_options_.cities.push_back({c.id, c.name, c.description});
}

GameServer::~GameServer() = default;

std::size_t GameServer::players_in_world() const {
  std::size_t n = 0;
  for (const auto& [id, z] : zones_) n += z->player_count();
  return n;
}

Zone* GameServer::zone(const std::string& id) {
  auto it = zones_.find(id);
  return it == zones_.end() ? nullptr : it->second.get();
}

Zone* GameServer::ensure_zone(const std::string& id) {
  if (auto it = zones_.find(id); it != zones_.end()) return it->second.get();
  const auto zit = std::find_if(content_.zones.begin(), content_.zones.end(), [&](const ZoneDef& z) { return z.id == id; });
  if (zit == content_.zones.end()) return nullptr;
  auto zone = std::make_unique<Zone>(*zit, content_, dt_, seed_for(cfg_, zit->id));
  Zone* z = zone.get();
  z->set_groups(&groups_);
  zones_.emplace(id, std::move(zone));
  zone_empty_since_[id] = tick_;
  if (auto pc = parked_corpses_.find(id); pc != parked_corpses_.end()) {
    for (const auto& c : pc->second) z->add_corpse(c);
    parked_corpses_.erase(pc);
  }
  spdlog::info("zone '{}' loaded ({} mobs, {} npcs)", id, z->mob_count(), z->npc_count());
  return z;
}

void GameServer::unload_idle_zones() {
  if (cfg_.zone_idle_seconds == 0) return;
  const std::uint32_t idle = cfg_.zone_idle_seconds * std::max<std::uint32_t>(1, cfg_.tick_rate);
  for (auto it = zones_.begin(); it != zones_.end();) {
    Zone& z = *it->second;
    // Player corpses keep a zone loaded (they are saved, but a loaded zone is simplest to rez in).
    if (z.def().persistent || z.player_count() > 0 || z.has_player_corpses()) {
      zone_empty_since_[it->first] = tick_;
      ++it;
      continue;
    }
    if (tick_ - zone_empty_since_[it->first] < idle) {
      ++it;
      continue;
    }
    spdlog::info("zone '{}' unloaded (empty for {} s)", it->first, cfg_.zone_idle_seconds);
    zone_empty_since_.erase(it->first);
    for (std::size_t i = 0; i < kTickPhaseCount; ++i) unloaded_phases_[i] += it->second->tick_phases()[i];
    it = zones_.erase(it);
  }
}

void GameServer::load_corpses() {
  const std::int64_t now = unix_now();
  db_.post([this, now](db::Repository& repo, db::DbWorker& w) {
    auto corpses = repo.load_corpses(now);
    w.complete([this, now, corpses = std::move(corpses)] {
      std::size_t placed = 0;
      for (const auto& c : corpses) {
        if (!content_.find_zone(c.zone)) {
          spdlog::warn("corpse {} of '{}' is in unknown zone '{}'; leaving it in the database", c.id, c.owner_name, c.zone);
          continue;
        }
        CorpseInit init;
        init.db_id = c.id;
        init.owner_char_id = c.char_id;
        init.owner_name = c.owner_name;
        init.level = c.level;
        init.pos = {c.x, c.y, c.z};
        init.yaw = c.yaw;
        init.coin = c.coin;
        for (const auto& it : c.items)
          init.items.push_back({ItemStackState{it.item_id, it.count, 0}, static_cast<SlotCode>(std::clamp(it.slot, 0, 0xFFFF)), 0, 0});
        init.xp_lost = c.xp_lost;
        init.seconds_left = static_cast<float>(std::max<std::int64_t>(1, c.expires_at - now));
        // A corpse keeps its zone loaded: bring it up now.
        if (Zone* z = ensure_zone(c.zone)) z->add_corpse(init);
        else parked_corpses_[c.zone].push_back(std::move(init));
        ++placed;
      }
      if (placed > 0) spdlog::info("restored {} player corpse(s)", placed);
    });
  });
}

void GameServer::load_auctions() {
  auctions_loading_ = true;
  db_.post([this](db::Repository& repo, db::DbWorker& w) {
    auto rows = repo.load_auctions();
    w.complete([this, rows = std::move(rows)] {
      auctions_loading_ = false;
      if (!rows) {
        // A partial board would hide listings from their sellers: stay closed and read it again shortly.
        spdlog::error("loading the auction board failed; retrying in 10 s");
        auction_retry_tick_ = tick_ + 10 * std::max<std::uint32_t>(1, cfg_.tick_rate);
        return;
      }
      std::vector<AuctionListing> listings;
      listings.reserve(rows->size());
      for (const auto& r : *rows) listings.push_back(to_listing(r));
      auctions_.load(std::move(listings));
      auctions_loaded_ = true;
      if (!rows->empty()) spdlog::info("restored {} auction listing(s)", rows->size());
    });
  });
}

db::CharacterRecord GameServer::snapshot(const Session& s, const PlayerEntity& p) const {
  db::CharacterRecord r;
  r.id = p.char_id;
  r.account_id = s.account_id;
  r.name = p.name;
  r.race = p.race;
  r.class_id = p.class_id;
  r.gender = p.gender;
  r.city = s.city;
  r.level = p.level;
  r.zone = s.zone_id;
  r.x = p.pos.x;
  r.y = p.pos.y;
  r.z = p.pos.z;
  r.yaw = p.yaw;
  r.xp = p.xp;
  r.hp = p.dead ? -1 : p.hp;
  r.mana = p.mana;
  r.endurance = p.endurance;
  r.coin = p.coin;
  r.bank_coin = p.bank_coin;
  r.bind_zone = p.bind_zone;
  r.bind_x = p.bind_pos.x;
  r.bind_y = p.bind_pos.y;
  r.bind_z = p.bind_pos.z;
  r.bind_yaw = p.bind_yaw;
  for (const auto& sk : p.skills) r.skills.emplace_back(sk.id, sk.value);
  const Zone* zone = zones_.contains(s.zone_id) ? zones_.at(s.zone_id).get() : nullptr;
  p.inv.for_each([&](SlotCode code, const ItemStackState& st) {
    r.items.push_back({static_cast<int>(code), st.item_id, st.count, zone ? zone->item_seconds_left(st) : 0.0f});
  });
  r.spellbook = p.spellbook;
  r.gems.assign(p.gems.begin(), p.gems.end());
  r.factions = p.factions.all();
  r.quests = p.quests.to_json();
  r.social = s.social.to_json();
  if (const Zone* z = zones_.contains(s.zone_id) ? zones_.at(s.zone_id).get() : nullptr) {
    for (const auto& b : z->saved_buffs(p)) r.buffs.push_back({b.ability_id, b.caster_level, b.caster_name, b.seconds_left, b.next_tick_seconds});
    for (const auto& t : z->saved_timers(p)) r.timers.emplace_back(t.ability_id, t.seconds_left);
  }
  return r;
}

db::CorpseRecord GameServer::corpse_record(const std::string& zone_id, const CorpseEntity& c, const Zone& zone) const {
  db::CorpseRecord r;
  r.id = c.db_id;
  r.char_id = c.owner_char_id;
  r.owner_name = c.owner_name;
  r.level = c.level;
  r.zone = zone_id;
  r.x = c.pos.x;
  r.y = c.pos.y;
  r.z = c.pos.z;
  r.yaw = c.yaw;
  r.coin = c.coin;
  r.xp_lost = c.xp_lost;
  const double left = static_cast<double>(c.expires_tick > zone.now() ? c.expires_tick - zone.now() : 0) * static_cast<double>(dt_);
  r.expires_at = unix_now() + static_cast<std::int64_t>(std::ceil(left));
  r.items = stored_items(c.items);
  return r;
}

Zone* GameServer::zone_of_player(const PlayerEntity& p, std::string* zone_id) {
  Session* s = find_session(p.session);
  if (!s || s->state != State::InWorld) return nullptr;
  if (zone_id) *zone_id = s->zone_id;
  return zone_for(*s);
}

void GameServer::save_soon(const PlayerEntity& p) {
  Session* s = find_session(p.session);
  if (!s || s->state != State::InWorld || s->char_id != p.char_id) return;
  mark_dirty(*s);
  flush_item_saves();
}

void GameServer::mark_dirty(const Session& s) {
  if (s.state != State::InWorld) return;
  Zone* z = zone_for(s);
  if (PlayerEntity* p = z ? z->find(s.entity) : nullptr) p->save_dirty = true;
}

GameServer::Session* GameServer::find_session(net::PeerId peer) {
  auto it = sessions_.find(peer);
  return it == sessions_.end() ? nullptr : &it->second;
}

Zone* GameServer::zone_for(const Session& s) {
  auto it = zones_.find(s.zone_id);
  return it == zones_.end() ? nullptr : it->second.get();
}

// --------------------------------------------------------------------------- transport events

void GameServer::on_connect(net::PeerId peer, const std::string& address) {
  if (sessions_.size() >= static_cast<std::size_t>(cfg_.max_players) + 16) {
    spdlog::warn("rejecting {}: server full", address);
    send(peer, proto::Kick{"The server is full. Please try again later."});
    transport_.disconnect_later(peer);
    return;
  }
  auto [it, inserted] = sessions_.emplace(peer, Session(peer, address, cfg_, content_.social));
  if (inserted) it->second.connected_at = now_;
  spdlog::debug("peer {} connected from {}", peer, address);
}

void GameServer::on_disconnect(net::PeerId peer) {
  Session* s = find_session(peer);
  if (!s) return;
  if (s->state == State::InWorld && start_linger(*s)) return;
  if (s->state == State::InWorld) leave_world(*s);
  if (s->account_id != 0) {
    auto it = online_accounts_.find(s->account_id);
    if (it != online_accounts_.end() && it->second == peer) online_accounts_.erase(it);
  }
  spdlog::info("peer {} ({}) disconnected{}", peer, s->address, s->username.empty() ? "" : " [" + s->username + "]");
  sessions_.erase(peer);
}

void GameServer::on_packet(net::PeerId peer, std::uint8_t channel, std::span<const std::uint8_t> data) {
  Session* sp = find_session(peer);
  if (!sp || sp->closing) return;
  Session& s = *sp;

  if (data.size() > cfg_.max_packet_size) {
    violation(s, "oversized packet");
    return;
  }
  if (!s.limiter.allow(data.size(), now_)) {
    ++s.rate_drops;
    if (s.rate_drops == 1 || s.rate_drops % 100 == 0) spdlog::warn("peer {} ({}) rate limited ({} drops)", peer, s.address, s.rate_drops);
    if (s.rate_drops >= cfg_.max_violations * 20) kick(s, "Disconnected for flooding the server.");
    return;
  }

  std::uint16_t id = 0;
  proto::Direction dir{};
  std::uint8_t expected_channel = 0;
  if (!proto::peek_id(data, id) || !proto::msg_info(id, dir, expected_channel) || dir != proto::Direction::ClientToServer) {
    violation(s, "unknown message id");
    return;
  }
  if (channel != expected_channel) {
    violation(s, "message on wrong channel");
    return;
  }
  const bool decoded = proto::visit_packet(data, [&](const auto& msg) { this->handle(s, msg); });
  if (!decoded) violation(s, "malformed packet");
}

// --------------------------------------------------------------------------- handshake / auth

void GameServer::handle(Session& s, const proto::Hello& m) {
  if (s.state != State::AwaitHello) return violation(s, "duplicate hello");
  if (auto err = protocol_mismatch_message(m.protocol_version, proto::kProtocolVersion)) {
    spdlog::info("peer {} ({}) rejected: protocol v{} (server v{}), build '{}'", s.peer, s.address, m.protocol_version,
                 proto::kProtocolVersion, m.client_build);
    send(s.peer, proto::HelloResult{false, proto::kProtocolVersion, *err, 0, "", ""});
    send_server_status(s);
    close_silently(s);
    return;
  }
  s.state = State::AwaitAuth;
  s.nonce = auth::random_hex_token(16);
  send(s.peer, proto::HelloResult{true, proto::kProtocolVersion, "Welcome to Realms of Emberfall.", login_port_, cert_pem_, s.nonce});
  send_server_status(s);
}

void GameServer::handle(Session& s, const proto::TicketLogin& m) {
  if (s.state != State::AwaitAuth) return violation(s, "ticket login in wrong state");
  auto account = tickets_.redeem(m.ticket, s.nonce, auth::TicketStore::Clock::now());
  if (!account) {
    ++s.failed_logins;
    spdlog::info("peer {} ({}) presented an invalid or expired login ticket", s.peer, s.address);
    send(s.peer, proto::AuthResult{false, "Your login expired or was invalid. Please log in again."});
    if (s.failed_logins >= cfg_.max_failed_logins) kick(s, "Too many failed login attempts.");
    return;
  }
  complete_login(s, *account);
}

void GameServer::complete_login(Session& s, const db::AccountRecord& account) {
  // The login service refuses banned accounts; a ban placed after the ticket was issued still counts.
  if (auto refusal = auth::ban_refusal(account.banned_until, account.ban_reason, unix_now())) {
    send(s.peer, proto::AuthResult{false, *refusal});
    kick(s, *refusal);
    return;
  }
  if (auto it = online_accounts_.find(account.id); it != online_accounts_.end() && it->second != s.peer) {
    // A lingering character waits to be reclaimed (EnterWorld); a live session elsewhere is replaced.
    if (Session* old = find_session(it->second); old && !old->lingering) kick(*old, "Your account was logged in from another location.");
  }
  online_accounts_[account.id] = s.peer;
  s.account_id = account.id;
  s.username = account.username;
  s.is_admin = account.is_admin;
  s.muted_until = account.muted_until;
  s.state = State::CharSelect;
  spdlog::info("peer {} ({}) authenticated as '{}'", s.peer, s.address, s.username);
  send(s.peer, proto::AuthResult{true, "Welcome, " + s.username + "."});
  send(s.peer, create_options_);
  request_char_list(s);
}

// --------------------------------------------------------------------------- characters

void GameServer::request_char_list(Session& s) {
  if (s.inflight >= kMaxInflightDb) return;
  ++s.inflight;
  const net::PeerId peer = s.peer;
  const std::int64_t account = s.account_id;
  const auto max_chars = static_cast<std::uint8_t>(content_.rules.max_characters_per_account);
  db_.post([this, peer, account, max_chars](db::Repository& repo, db::DbWorker& w) {
    proto::CharList list;
    list.max_characters = max_chars;
    for (const auto& r : repo.list_characters(account)) list.characters.push_back(to_summary(r));
    w.complete([this, peer, list] {
      Session* sp = find_session(peer);
      if (!sp) return;
      if (sp->inflight > 0) --sp->inflight;
      if (!sp->closing) send(peer, list);
    });
  });
}

void GameServer::handle(Session& s, const proto::CharListRequest& /*m*/) {
  if (s.state != State::CharSelect) return violation(s, "char list in wrong state");
  request_char_list(s);
}

void GameServer::handle(Session& s, const proto::CharCreate& m) {
  if (s.state != State::CharSelect) return violation(s, "char create in wrong state");
  auto fail = [&](const std::string& msg) { send(s.peer, proto::CharCreateResult{false, msg, {}}); };
  if (s.inflight > 0) return fail("Please wait for the previous request to finish.");

  const std::string name = auth::normalize_character_name(m.name);
  if (auto err = auth::validate_character_name(name)) return fail(*err);
  const RaceDef* race = content_.find_race(m.race);
  if (!race) return fail("Unknown race.");
  const ClassDef* cls = content_.find_class(m.class_id);
  if (!cls) return fail("Unknown class.");
  if (!contains(race->allowed_classes, cls->id)) return fail("A " + race->name + " cannot become a " + cls->name + ".");
  if (!contains(race->cities, m.city)) return fail("That starting city is not available to the " + race->name + ".");
  const CityDef* city = content_.find_city(m.city);
  if (!city) return fail("Unknown starting city.");
  if (m.gender > 1) return fail("Unknown gender.");

  db::CharacterRecord rec;
  rec.account_id = s.account_id;
  rec.name = name;
  rec.race = race->id;
  rec.class_id = cls->id;
  rec.city = city->id;
  rec.gender = m.gender;
  rec.level = 1;
  // New characters start, and are bound, at their starting city (or the configured test zone).
  std::string start_zone = city->zone;
  SpawnPoint start = city->spawn;
  if (!start_zones_.empty()) {
    // Several test zones: spread characters over them by name (stable across runs).
    const std::size_t pick = start_zones_.size() == 1 ? 0 : std::hash<std::string>{}(name) % start_zones_.size();
    const ZoneDef* sz = content_.find_zone(start_zones_[pick]);
    start_zone = sz->id;
    start = sz->safe_spawn;
    if (const ZoneEntryDef* e = sz->find_entry(city->id)) start = e->at;  // a test zone may keep a spot per city
  }
  rec.zone = start_zone;
  rec.x = start.x;
  rec.y = start.y;
  rec.z = start.z;
  rec.yaw = start.yaw;
  rec.bind_zone = start_zone;
  rec.bind_x = start.x;
  rec.bind_y = start.y;
  rec.bind_z = start.z;
  rec.bind_yaw = start.yaw;
  rec.coin = static_cast<std::uint64_t>(std::max(0, content_.rules.starting_coin));
  {
    // The class's starting kit: worn where it fits, the rest in the pack.
    Inventory kit;
    const Wearer w{race, cls, 1, false};
    for (const auto& si : cls->starting_items) {
      const ItemDef* it = content_.find_item(si.item);
      if (!it) continue;
      ItemStackState st{it->id, si.count, 0};
      if (si.equip) {
        const SlotCode slot = auto_equip_slot(kit, *it, w);
        if (slot != kNoSlot && kit.at(slot)->empty()) {
          *kit.at(slot) = st;
          continue;
        }
      }
      if (!give_item(kit, content_, st)) return fail("Your class's starting items do not fit. Please report this to a game master.");
    }
    kit.for_each([&](SlotCode code, const ItemStackState& st) { rec.items.push_back({static_cast<int>(code), st.item_id, st.count, 0.0f}); });
  }

  ++s.inflight;
  const net::PeerId peer = s.peer;
  const int max_chars = content_.rules.max_characters_per_account;
  db_.post([this, peer, rec, max_chars](db::Repository& repo, db::DbWorker& w) mutable {
    const auto st = repo.create_character(rec, max_chars);
    w.complete([this, peer, rec, st, max_chars] {
      Session* sp = find_session(peer);
      if (!sp) return;
      if (sp->inflight > 0) --sp->inflight;
      if (sp->closing) return;
      proto::CharCreateResult r;
      r.ok = st == db::DbStatus::Ok;
      switch (st) {
        case db::DbStatus::Ok:
          r.message = rec.name + " has entered the realm.";
          r.character = to_summary(rec);
          spdlog::info("account '{}' created character '{}' ({} {})", sp->username, rec.name, rec.race, rec.class_id);
          break;
        case db::DbStatus::Duplicate: r.message = "That name is already taken."; break;
        case db::DbStatus::LimitReached: r.message = "You already have " + std::to_string(max_chars) + " characters."; break;
        case db::DbStatus::NotFound:
        case db::DbStatus::Error: r.message = "Character creation failed. Please try again."; break;
      }
      send(peer, r);
      request_char_list(*sp);
    });
  });
}

void GameServer::handle(Session& s, const proto::CharDelete& m) {
  if (s.state != State::CharSelect) return violation(s, "char delete in wrong state");
  if (s.inflight > 0) return send(s.peer, proto::CharDeleteResult{false, m.char_id, "Please wait for the previous request to finish."});
  if (Session* old = lingering_for_account(s.account_id); old && old->char_id == static_cast<std::int64_t>(m.char_id)) end_linger(old->peer);
  ++s.inflight;
  const net::PeerId peer = s.peer;
  const std::int64_t account = s.account_id;
  const auto char_id = static_cast<std::int64_t>(m.char_id);
  db_.post([this, peer, account, char_id](db::Repository& repo, db::DbWorker& w) {
    const auto st = repo.delete_character(account, char_id);
    if (st == db::DbStatus::Ok) carry_.chars.erase(char_id);  // nothing left to retry for a deleted character
    w.complete([this, peer, char_id, st] {
      Session* sp = find_session(peer);
      if (!sp) return;
      if (sp->inflight > 0) --sp->inflight;
      if (sp->closing) return;
      const bool ok = st == db::DbStatus::Ok;
      send(peer, proto::CharDeleteResult{ok, static_cast<std::uint64_t>(char_id), ok ? "Character deleted." : "Character not found."});
      if (ok) spdlog::info("account '{}' deleted character {}", sp->username, char_id);
      if (ok) guild_after_character_deleted(char_id);
      request_char_list(*sp);
    });
  });
}

// --------------------------------------------------------------------------- world

void GameServer::handle(Session& s, const proto::EnterWorld& m) {
  if (s.state != State::CharSelect) return violation(s, "enter world in wrong state");
  if (s.inflight > 0) return;
  if (Session* old = lingering_for_account(s.account_id)) {
    // Back from a dropped connection: the same character is picked up where it stands; any other choice
    // lets the lingering one leave the world first (its save is queued ahead of the load below).
    if (old->char_id == static_cast<std::int64_t>(m.char_id)) return reclaim(s, *old);
    end_linger(old->peer);
  }
  flush_item_saves();  // anything pending goes out first (the DB worker runs jobs in order)
  s.state = State::EnteringWorld;
  ++s.inflight;
  const net::PeerId peer = s.peer;
  const std::int64_t account = s.account_id;
  const auto char_id = static_cast<std::int64_t>(m.char_id);
  db_.post([this, peer, account, char_id](db::Repository& repo, db::DbWorker& w) {
    // A newer copy of this character that failed to save must not be shadowed by the stored one.
    if (carry_.chars.contains(char_id)) {
      w.complete([this, peer] {
        Session* sp = find_session(peer);
        if (!sp) return;
        if (sp->inflight > 0) --sp->inflight;
        if (sp->state != State::EnteringWorld) return;
        sp->state = State::CharSelect;
        proto::EnterWorldResult r;
        r.ok = false;
        r.message = "Your character is still being saved. Please try again in a moment.";
        send(peer, r);
      });
      return;
    }
    auto rec = repo.load_character(account, char_id);
    w.complete([this, peer, rec] { on_enter_world(peer, rec); });
  });
}

ZonePlayerInit GameServer::init_from_record(net::PeerId peer, const db::CharacterRecord& rec) const {
  ZonePlayerInit init;
  init.session = peer;
  init.char_id = rec.id;
  init.name = rec.name;
  init.race = rec.race;
  init.class_id = rec.class_id;
  init.gender = rec.gender;
  init.level = std::clamp(rec.level, 1, content_.rules.max_level);
  init.pos = {rec.x, rec.y, rec.z};
  init.yaw = rec.yaw;
  init.xp = rec.xp;
  init.hp = rec.hp;
  init.mana = rec.mana;
  init.endurance = rec.endurance;
  init.coin = rec.coin;
  init.bank_coin = rec.bank_coin;
  for (const auto& it : rec.items) {
    init.items.emplace_back(it.slot, ItemStackState{it.item_id, it.count, 0});
    if (it.seconds_left > 0.0f) init.item_seconds.emplace_back(it.slot, it.seconds_left);
  }
  for (const auto& [id, v] : rec.skills) init.skills.push_back({id, v});
  init.bind_zone = rec.bind_zone;
  init.bind_pos = {rec.bind_x, rec.bind_y, rec.bind_z};
  init.bind_yaw = rec.bind_yaw;
  init.spellbook = rec.spellbook;
  init.gems = rec.gems;
  for (const auto& b : rec.buffs) init.buffs.push_back({b.ability_id, b.caster_level, b.caster_name, b.seconds_left, b.next_tick_seconds});
  for (const auto& [id, left] : rec.timers) init.timers.push_back({id, left});
  init.factions = rec.factions;
  init.quests = rec.quests;
  if (const Guild* g = guilds_.of(rec.id)) init.guild_tag = g->name;
  if (init.bind_zone.empty() || !content_.find_zone(init.bind_zone)) {  // from before binds, or a removed zone
    if (const CityDef* city = content_.find_city(rec.city)) {
      init.bind_zone = city->zone;
      init.bind_pos = {city->spawn.x, city->spawn.y, city->spawn.z};
      init.bind_yaw = city->spawn.yaw;
    }
  }
  return init;
}

void GameServer::send_enter_result(Session& s, Zone& zone, const PlayerEntity& p, bool zone_change) {
  const ZoneDef& zd = zone.def();
  proto::EnterWorldResult r;
  r.ok = true;
  r.entity_id = p.id;
  r.zone_id = zd.id;
  r.zone_name = zd.name;
  r.min_x = zd.min_x;
  r.min_z = zd.min_z;
  r.max_x = zd.max_x;
  r.max_z = zd.max_z;
  r.x = p.pos.x;
  r.y = p.pos.y;
  r.z = p.pos.z;
  r.yaw = p.yaw;
  r.run_speed = content_.rules.movement.run_speed;
  r.tick_rate = static_cast<std::uint8_t>(cfg_.tick_rate);
  r.server_tick = tick_;
  r.zone_kind = zd.kind;
  r.seed = zd.has_dungeon ? zd.dungeon.seed : 0;
  r.layout_hash = zd.layout_hash;
  r.zone_change = zone_change;
  r.level_min = static_cast<std::uint8_t>(std::clamp(zd.level_min, 1, 255));
  r.level_max = static_cast<std::uint8_t>(std::clamp(zd.level_max, 1, 255));
  send(s.peer, r);
  zone.send_initial_state(p.id, *this);
  send(s.peer, proto::ChatMessage{kChatSystem, "", "You have entered " + zd.name + "."});
}

void GameServer::on_enter_world(net::PeerId peer, const std::optional<db::CharacterRecord>& rec) {
  Session* s = find_session(peer);
  if (!s) return;
  if (s->inflight > 0) --s->inflight;
  if (s->closing || s->state != State::EnteringWorld) return;
  auto refuse = [&](const char* message) {
    s->state = State::CharSelect;
    proto::EnterWorldResult r;
    r.ok = false;
    r.message = message;
    send(peer, r);
  };
  if (!rec) return refuse("Character not found.");
  // Sessions may exceed max_players slightly (so full-server players can still reach character
  // select), but the world itself never does.
  if (players_in_world() >= cfg_.max_players) return refuse("The world is full. Please try again shortly.");

  ZonePlayerInit init = init_from_record(peer, *rec);
  Zone* zp = ensure_zone(rec->zone);
  if (!zp) {
    // A zone that no longer exists: back to the bind point (or the first zone's safe spot).
    spdlog::warn("character '{}' was in unknown zone '{}'; moving to their bind point", rec->name, rec->zone);
    zp = ensure_zone(init.bind_zone);
    if (zp) {
      init.pos = init.bind_pos;
      init.yaw = init.bind_yaw;
    } else {
      zp = ensure_zone(content_.zones.front().id);
      const auto& sp = zp->def().safe_spawn;
      init.pos = {sp.x, sp.y, sp.z};
      init.yaw = sp.yaw;
    }
  }
  Zone& zone = *zp;
  const ZoneDef& zd = zone.def();
  if (init.pos.x < zd.min_x || init.pos.x > zd.max_x || init.pos.z < zd.min_z || init.pos.z > zd.max_z) {
    init.pos = {zd.safe_spawn.x, zd.safe_spawn.y, zd.safe_spawn.z};
    init.yaw = zd.safe_spawn.yaw;
  }
  const EntityId eid = zone.add_player(init);
  const PlayerEntity* p = zone.find(eid);
  if (p->unplaced_items > 0) {
    // Some saved items found no slot (a shrunken bank or bag, all full). Entering would save the character
    // without them; refuse instead, leaving the stored inventory untouched.
    spdlog::error("'{}': {} saved item stack(s) have no slot; refusing entry", rec->name, p->unplaced_items);
    zone.remove_player(eid, *this);
    return refuse("Some of your items could not be placed. Please contact a game master.");
  }

  s->state = State::InWorld;
  s->entity = eid;
  s->char_id = rec->id;
  s->char_name = rec->name;
  s->city = rec->city;
  s->race = rec->race;
  s->zone_id = zd.id;
  s->social = SocialLists::from_json(rec->social, content_.social.max_friends, content_.social.max_ignored);
  send_enter_result(*s, zone, *p, false);
  on_world_enter_social(*s);
  spdlog::info("'{}' entered {} as {} (entity {}, level {})", s->username, zd.id, rec->name, eid, p->level);
}

void GameServer::process_transfers() {
  // Moving people can queue more (a cross-zone respawn during removal): repeat a few times.
  for (int round = 0; round < 4; ++round) {
    std::vector<std::pair<std::string, ZoneTransfer>> all;
    for (auto& [id, z] : zones_)
      for (auto& t : z->take_transfers()) all.emplace_back(id, std::move(t));
    if (all.empty()) return;
    for (auto& [zone_id, t] : all) {
      Zone* from = zone(zone_id);
      const PlayerEntity* p = from ? from->find(t.player) : nullptr;
      Session* s = p ? find_session(p->session) : nullptr;
      if (!s || s->state != State::InWorld || s->entity != t.player || s->zone_id != zone_id) continue;
      transfer(*s, *from, t);
    }
  }
}

void GameServer::transfer(Session& s, Zone& from, const ZoneTransfer& t) {
  Zone* to = ensure_zone(t.to.zone);
  PlayerEntity* p = from.find(s.entity);
  if (!p) return;
  if (!to) {
    spdlog::error("'{}': transfer to unknown zone '{}' refused", s.char_name, t.to.zone);
    p->transferring = false;
    return;
  }
  // A summoned pet follows; a charmed one does not (the charm breaks as its master leaves).
  std::string pet_id;
  int pet_level = 1;
  float pet_hp = 1.0f;
  if (p->pet != 0)
    if (MobEntity* pet = from.find_mob(p->pet); pet && pet->pet_def && pet->alive()) {
      pet_id = pet->pet_def->id;
      pet_level = pet->def->level;
      pet_hp = pet->stats.max_hp > 0 ? static_cast<float>(pet->hp) / static_cast<float>(pet->stats.max_hp) : 1.0f;
    }
  from.trade_cancel(s.entity, s.char_name + " is no longer here; the trade is cancelled.", *this);
  db::CharacterRecord rec = snapshot(s, *p);  // buffs, timers and items as they are now
  rec.zone = to->def().id;
  rec.x = t.to.pos.x;
  rec.y = t.to.pos.y;
  rec.z = t.to.pos.z;
  rec.yaw = t.to.yaw;
  const std::string from_id = from.def().id;
  const bool had_seq = p->has_seq;
  const std::uint32_t old_seq = p->last_queued_seq;
  from.remove_player(s.entity, *this);
  const ZonePlayerInit init = init_from_record(s.peer, rec);
  const EntityId eid = to->add_player(init);
  // Inputs still in flight from the old zone must not set the new zone's sequence (they would freeze
  // the player until the client's restarted counter caught up).
  to->reset_input_seq(eid, had_seq, old_seq + Zone::kZoneChangeSeqGap);
  const PlayerEntity* np = to->find(eid);
  s.entity = eid;
  s.zone_id = to->def().id;
  spdlog::info("'{}' ({}) moved {} -> {} by {}", s.char_name, s.username, from_id, to->def().id, t.reason);
  send_enter_result(s, *to, *np, true);
  if (!pet_id.empty()) to->restore_pet(eid, pet_id, pet_level, pet_hp, *this);
  // A zone change saves the character, together with everyone else's pending changes.
  mark_dirty(s);
  flush_item_saves();
}

void GameServer::leave_world(Session& s) {
  if (s.state != State::InWorld) return;
  on_world_leave_social(s);
  if (Zone* zone = zone_for(s)) {
    // A dead character camps out at their bind point; the corpse stays where it fell.
    if (const PlayerEntity* p = zone->find(s.entity); p && p->dead) {
      zone->respawn(s.entity, *this);
      process_transfers();  // a bind in another zone: move there first, so that is what gets saved
      zone = zone_for(s);
      if (!zone) return;
    }
    zone->trade_cancel(s.entity, s.char_name + " is no longer here; the trade is cancelled.", *this);
    mark_dirty(s);
    flush_item_saves();  // with everyone else's pending changes, in one transaction
    zone->remove_player(s.entity, *this);
  }
  spdlog::info("'{}' left the world ({})", s.username, s.char_name);
  s.entity = 0;
  s.char_id = 0;
  s.char_name.clear();
  s.zone_id.clear();
  s.state = State::CharSelect;
}

void GameServer::handle(Session& s, const proto::Logout& /*m*/) {
  if (s.state == State::InWorld) {
    leave_world(s);
    send(s.peer, proto::LogoutResult{true});
    request_char_list(s);
    return;
  }
  if (s.state == State::CharSelect) {
    send(s.peer, proto::LogoutResult{true});
    return;
  }
  violation(s, "logout in wrong state");
}

void GameServer::handle(Session& s, const proto::MoveInput& m) {
  if (s.state != State::InWorld) {
    // Late inputs right after leaving the world are normal on an unreliable channel.
    if (s.state != State::CharSelect) violation(s, "movement in wrong state");
    return;
  }
  Zone* zone = zone_for(s);
  if (!zone) return;
  const auto res = zone->queue_input(s.entity, m.seq, MoveIntent{m.move_x, m.move_z, m.yaw}, Vec3{m.pred_x, m.pred_y, m.pred_z});
  if (res == InputResult::QueueFull) spdlog::debug("peer {} movement queue full; input dropped", s.peer);
}

void GameServer::handle(Session& s, const proto::ChatSay& m) {
  if (s.state != State::InWorld) return violation(s, "chat in wrong state");
  chat(s, ChatChannel::Say, "", m.text);
}

// --------------------------------------------------------------------------- combat and loot intents

Zone* GameServer::world_action(Session& s, const char* what) {
  if (s.state != State::InWorld) {
    // A combat key pressed while camping can arrive just after leaving the world.
    if (s.state != State::CharSelect) violation(s, what);
    return nullptr;
  }
  if (!s.action_limiter.consume(1.0, now_)) return nullptr;  // spammy but harmless: drop
  return zone_for(s);
}

void GameServer::handle(Session& s, const proto::SetTarget& m) {
  if (Zone* z = world_action(s, "target in wrong state")) z->set_target(s.entity, m.entity_id, *this);
}

void GameServer::handle(Session& s, const proto::AutoAttack& m) {
  if (Zone* z = world_action(s, "attack in wrong state")) z->set_auto_attack(s.entity, m.enabled, *this);
}

void GameServer::handle(Session& s, const proto::UseAbility& m) {
  if (Zone* z = world_action(s, "ability in wrong state")) z->use_ability(s.entity, m.ability_id, m.target_id, *this);
}

void GameServer::handle(Session& s, const proto::RespawnRequest& /*m*/) {
  if (Zone* z = world_action(s, "respawn in wrong state")) {
    z->respawn(s.entity, *this);
    if (const PlayerEntity* p = z->find(s.entity)) save_soon(*p);
  }
}

void GameServer::handle(Session& s, const proto::LootOpen& m) {
  if (Zone* z = world_action(s, "loot in wrong state")) z->loot_open(s.entity, m.corpse_id, *this);
}

void GameServer::handle(Session& s, const proto::LootTake& m) {
  if (Zone* z = world_action(s, "loot in wrong state")) z->loot_take(s.entity, m.corpse_id, m.slot, *this);
}

void GameServer::handle(Session& s, const proto::LootClose& m) {
  if (Zone* z = world_action(s, "loot in wrong state")) z->loot_close(s.entity, m.corpse_id, *this);
}

void GameServer::handle(Session& s, const proto::SetSitting& m) {
  if (Zone* z = world_action(s, "sit in wrong state")) z->set_sitting(s.entity, m.sitting, *this);
}

void GameServer::handle(Session& s, const proto::CastSpell& m) {
  if (Zone* z = world_action(s, "cast in wrong state")) z->cast_spell(s.entity, m.gem, m.target_id, *this);
}

void GameServer::handle(Session& s, const proto::StopCast& /*m*/) {
  if (Zone* z = world_action(s, "stop cast in wrong state")) z->stop_cast(s.entity, *this);
}

void GameServer::handle(Session& s, const proto::MemorizeSpell& m) {
  if (Zone* z = world_action(s, "memorize in wrong state")) z->memorize_spell(s.entity, m.gem, m.spell_id, *this);
}

void GameServer::handle(Session& s, const proto::ScribeSpell& m) {
  if (Zone* z = world_action(s, "scribe in wrong state")) z->scribe_spell(s.entity, m.slot, *this);
}

void GameServer::handle(Session& s, const proto::TrainerOpen& m) {
  if (Zone* z = world_action(s, "trainer in wrong state")) z->trainer_open(s.entity, m.npc_id, *this);
}

void GameServer::handle(Session& s, const proto::TrainerBuy& m) {
  if (Zone* z = world_action(s, "trainer in wrong state")) z->trainer_buy(s.entity, m.npc_id, m.item_id, *this);
}

void GameServer::handle(Session& s, const proto::RezResponse& m) {
  if (Zone* z = world_action(s, "resurrect in wrong state")) z->rez_response(s.entity, m.offer_id, m.accept, *this);
}

void GameServer::handle(Session& s, const proto::PetCommand& m) {
  if (m.command > static_cast<std::uint8_t>(PetOrder::Dismiss)) return violation(s, "unknown pet command");
  if (Zone* z = world_action(s, "pet command in wrong state")) z->pet_command(s.entity, static_cast<PetOrder>(m.command), m.target_id, *this);
}

// --------------------------------------------------------------------------- items, vendors, bank, trade

Zone* GameServer::item_action(Session& s, const char* what) {
  if (s.state != State::InWorld) {
    if (s.state != State::CharSelect) violation(s, what);
    return nullptr;
  }
  if (!s.item_limiter.consume(1.0, now_)) return nullptr;
  return zone_for(s);
}

void GameServer::handle(Session& s, const proto::MoveItem& m) {
  if (Zone* z = item_action(s, "item move in wrong state")) z->move_item(s.entity, m.from_slot, m.to_slot, m.count, *this);
}

void GameServer::handle(Session& s, const proto::DestroyItem& m) {
  if (Zone* z = item_action(s, "destroy in wrong state")) z->destroy_item(s.entity, m.slot, m.item_id, *this);
}

void GameServer::handle(Session& s, const proto::UseItem& m) {
  if (Zone* z = world_action(s, "use item in wrong state")) z->use_item(s.entity, m.slot, m.target_id, *this);
}

void GameServer::handle(Session& s, const proto::VendorOpen& m) {
  if (Zone* z = world_action(s, "vendor in wrong state")) z->vendor_open(s.entity, m.npc_id, *this);
}

void GameServer::handle(Session& s, const proto::VendorBuy& m) {
  if (Zone* z = item_action(s, "vendor in wrong state")) z->vendor_buy(s.entity, m.npc_id, m.item_id, m.count, m.buyback, *this);
}

void GameServer::handle(Session& s, const proto::VendorSell& m) {
  if (Zone* z = item_action(s, "vendor in wrong state")) z->vendor_sell(s.entity, m.npc_id, m.slot, m.count, *this);
}

void GameServer::handle(Session& s, const proto::BankOpen& m) {
  if (Zone* z = world_action(s, "bank in wrong state")) z->bank_open(s.entity, m.npc_id, *this);
}

void GameServer::handle(Session& s, const proto::BankCoin& m) {
  if (Zone* z = item_action(s, "bank in wrong state")) z->bank_coin(s.entity, m.deposit, m.amount, *this);
}

void GameServer::handle(Session& s, const proto::TradeRequest& m) {
  Zone* z = world_action(s, "trade in wrong state");
  if (!z) return;
  // Someone ignoring you does not hear your trade offers either.
  if (const PlayerEntity* t = z->find(m.target_id))
    if (Session* ts = find_session(t->session); ts && ts->social.ignores(s.char_name))
      return system_to(s, t->name + " is not accepting trades from you.");
  z->trade_request(s.entity, m.target_id, *this);
}

void GameServer::handle(Session& s, const proto::TradeRespond& m) {
  if (Zone* z = world_action(s, "trade in wrong state")) z->trade_respond(s.entity, m.from_id, m.accept, *this);
}

void GameServer::handle(Session& s, const proto::TradeSetItem& m) {
  if (Zone* z = item_action(s, "trade in wrong state")) z->trade_set_item(s.entity, m.index, m.slot, *this);
}

void GameServer::handle(Session& s, const proto::TradeSetCoin& m) {
  if (Zone* z = item_action(s, "trade in wrong state")) z->trade_set_coin(s.entity, m.coin, *this);
}

void GameServer::handle(Session& s, const proto::TradeAccept& m) {
  if (Zone* z = world_action(s, "trade in wrong state")) z->trade_accept(s.entity, m.accept, *this, m.trade_id, m.revision);
}

void GameServer::handle(Session& s, const proto::TradeCancel& /*m*/) {
  // Never rate-limited: backing out of a trade must always work.
  if (s.state != State::InWorld) return;
  if (Zone* z = zone_for(s)) z->trade_cancel(s.entity, "", *this);
}

void GameServer::handle(Session& s, const proto::LootRollChoice& m) {
  if (m.choice > static_cast<std::uint8_t>(RollChoice::Need)) return violation(s, "unknown roll choice");
  if (Zone* z = world_action(s, "loot roll in wrong state")) z->loot_roll_choice(s.entity, m.roll_id, static_cast<RollChoice>(m.choice), *this);
}

void GameServer::handle(Session& s, const proto::SetLootRule& m) {
  if (m.rule > static_cast<std::uint8_t>(LootRule::RoundRobin)) return violation(s, "unknown loot rule");
  Zone* z = world_action(s, "loot rule in wrong state");
  if (!z) return;
  // In a group the leader sets the rule for everyone; on your own it is your own setting.
  if (groups_.group_of(s.char_id)) return group_loot_rule(s, static_cast<LootRule>(m.rule));
  z->set_loot_rule(s.entity, static_cast<LootRule>(m.rule), *this);
}

void GameServer::handle(Session& s, const proto::NpcTalk& m) {
  if (Zone* z = world_action(s, "npc talk in wrong state")) z->npc_talk(s.entity, m.npc_id, *this);
}

void GameServer::handle(Session& s, const proto::NpcChoose& m) {
  if (Zone* z = world_action(s, "npc choice in wrong state")) z->npc_choose(s.entity, m.npc_id, m.key, *this);
}

void GameServer::handle(Session& s, const proto::QuestAbandon& m) {
  if (Zone* z = world_action(s, "quest abandon in wrong state")) z->quest_abandon(s.entity, m.quest_id, *this);
}

void GameServer::handle(Session& s, const proto::TravelOpen& m) {
  if (Zone* z = world_action(s, "travel in wrong state")) z->travel_open(s.entity, m.npc_id, *this);
}

void GameServer::handle(Session& s, const proto::TravelChoose& m) {
  if (Zone* z = world_action(s, "travel in wrong state")) z->travel_choose(s.entity, m.npc_id, m.dest, *this);
}

// --------------------------------------------------------------------------- auctions

std::optional<db::CharacterRecord> GameServer::snapshot_in_world(const Session& s) {
  if (s.state != State::InWorld) return std::nullopt;
  Zone* z = zone_for(s);
  const PlayerEntity* p = z ? z->find(s.entity) : nullptr;
  if (!p) return std::nullopt;
  return snapshot(s, *p);
}

GameServer::Session* GameServer::session_of_char(std::int64_t char_id) {
  for (auto& [peer, s] : sessions_)
    if (s.state == State::InWorld && s.char_id == char_id) return &s;
  return nullptr;
}

void GameServer::send_auction_page(Session& s, const AuctionQuery& q) {
  const AuctionPage page = auctions_.search(q, content_, content_.rules.economy.auction_page_size);
  proto::AuctionResults r;
  r.page = static_cast<std::uint16_t>(std::clamp(q.page, 0, 65535));
  r.total = static_cast<std::uint32_t>(std::min<std::size_t>(page.total, 0xFFFFFFFFu));
  r.mine = q.mine != 0;
  const std::int64_t now = unix_now();
  for (const AuctionListing* l : page.rows) {
    const std::int64_t left = std::max<std::int64_t>(0, l->expires_at - now);
    r.entries.push_back({static_cast<std::uint64_t>(l->id), l->item_id, static_cast<std::uint16_t>(std::clamp(l->count, 0, 65535)), l->price,
                         l->seller_name, static_cast<std::uint32_t>(std::min<std::int64_t>(left * 1000, 0xFFFFFFFFLL)),
                         static_cast<std::uint8_t>(l->state)});
  }
  send(s.peer, r);
}

void GameServer::collect_auctions(Session& s, Zone& z) {
  std::vector<std::int64_t> done;
  std::uint64_t coin = 0;
  int returned = 0;
  int waiting = 0;
  for (AuctionListing* l : auctions_.collectable(s.char_id)) {
    if (l->state == ListingState::Sold) {
      coin += l->price;
      done.push_back(l->id);
    } else if (z.give_from_auction(s.entity, {l->item_id, l->count, 0}, 0, *this)) {
      ++returned;
      done.push_back(l->id);
    } else {
      ++waiting;
    }
  }
  if (done.empty()) return;
  if (coin > 0) z.add_coin(s.entity, coin, *this);
  for (std::int64_t id : done) auctions_.remove(id);
  std::string text = "The auctioneer hands you";
  if (coin > 0) text += " " + format_coin(coin) + " from your sales";
  if (returned > 0) text += std::string(coin > 0 ? " and" : "") + " " + std::to_string(returned) + " unsold item" + (returned == 1 ? "" : "s");
  text += '.';
  if (waiting > 0) text += " " + std::to_string(waiting) + " more must wait until you have room.";
  z.tell(s.entity, text, *this);
  pending_auctions_.insert(done.begin(), done.end());
  mark_dirty(s);
  flush_item_saves();
}

void GameServer::handle(Session& s, const proto::AuctionOpen& m) {
  Zone* z = world_action(s, "auction in wrong state");
  if (!z || !z->at_auctioneer(s.entity, m.npc_id, *this)) return;
  if (!auctions_loaded_) return z->tell(s.entity, "The auction board is still being set up. Try again in a moment.", *this);
  const auto& E = content_.rules.economy;
  std::string name = "Auctioneer";
  if (const NpcEntity* n = z->find_npc(m.npc_id)) name = n->def->name;
  send(s.peer, proto::AuctionWindow{m.npc_id, name, E.auction_fee_pct, static_cast<std::uint32_t>(std::max(0, E.auction_min_fee)), E.auction_hours,
                                    static_cast<std::uint16_t>(std::clamp(E.auction_max_listings, 0, 65535))});
  collect_auctions(s, *z);
  send_auction_page(s, AuctionQuery{});
}

void GameServer::handle(Session& s, const proto::AuctionSearch& m) {
  Zone* z = world_action(s, "auction in wrong state");
  if (!z || !z->at_auctioneer(s.entity, m.npc_id, *this)) return;
  AuctionQuery q;
  q.text = auth::sanitize_chat(m.text);
  q.kind = m.kind;
  q.min_level = m.min_level;
  q.max_level = m.max_level == 0 ? 255 : m.max_level;
  q.page = m.page;
  q.mine = m.mine ? s.char_id : 0;
  if (m.mine) collect_auctions(s, *z);
  send_auction_page(s, q);
}

void GameServer::handle(Session& s, const proto::AuctionPost& m) {
  Zone* z = world_action(s, "auction in wrong state");
  if (!z || !z->at_auctioneer(s.entity, m.npc_id, *this)) return;
  const auto& E = content_.rules.economy;
  if (!auctions_loaded_) return z->tell(s.entity, "The auction board is still being set up. Try again in a moment.", *this);
  if (m.price < 1 || m.price > 100000000000ULL) return z->tell(s.entity, "Set a price between 1 copper and 100,000,000 platinum.", *this);
  if (auctions_.count_for(s.char_id) >= E.auction_max_listings)
    return z->tell(s.entity, "You already have " + std::to_string(E.auction_max_listings) + " listings on the board (collect sold and expired ones).", *this);
  const std::uint64_t fee = auction_fee(m.price, E);
  ItemStackState stack;
  if (!z->take_for_auction(s.entity, m.slot, m.item_id, fee, stack, *this)) return;
  AuctionListing l;
  do {
    l.id = random_id();
  } while (auctions_.find(l.id));
  l.seller_char = s.char_id;
  l.seller_name = s.char_name;
  l.item_id = stack.item_id;
  l.count = stack.count;
  l.price = m.price;
  l.listed_at = unix_now();
  l.expires_at = l.listed_at + static_cast<std::int64_t>(std::lround(E.auction_hours * 3600.0f));
  auctions_.add(l);
  const ItemDef* d = content_.find_item(l.item_id);
  z->tell(s.entity, "You list " + (l.count > 1 ? std::to_string(l.count) + " x " : std::string()) + (d ? d->name : l.item_id) + " for " +
                        format_coin(l.price) + " (fee " + format_coin(fee) + ").", *this);
  pending_auctions_.insert(l.id);
  mark_dirty(s);
  flush_item_saves();
  AuctionQuery mine;
  mine.mine = s.char_id;
  send_auction_page(s, mine);
}

void GameServer::handle(Session& s, const proto::AuctionBuy& m) {
  Zone* z = world_action(s, "auction in wrong state");
  if (!z || !z->at_auctioneer(s.entity, m.npc_id, *this)) return;
  AuctionListing* l = auctions_.find(static_cast<std::int64_t>(m.listing_id & 0x7FFFFFFFFFFFFFFFULL));
  if (!l || l->state != ListingState::Active || unix_now() >= l->expires_at) return z->tell(s.entity, "That listing is no longer available.", *this);
  if (l->seller_char == s.char_id) return z->tell(s.entity, "You cannot buy your own listing; cancel it instead.", *this);
  if (l->price != m.price) return z->tell(s.entity, "The price of that listing changed; search again.", *this);
  if (!z->give_from_auction(s.entity, {l->item_id, l->count, 0}, l->price, *this)) return;
  l->state = ListingState::Sold;
  l->buyer = s.char_name;
  const ItemDef* d = content_.find_item(l->item_id);
  const std::string what = (l->count > 1 ? std::to_string(l->count) + " x " : std::string()) + (d ? d->name : l->item_id);
  z->tell(s.entity, "You buy " + what + " for " + format_coin(l->price) + ".", *this);
  if (Session* seller = session_of_char(l->seller_char))
    if (Zone* sz = zone_for(*seller)) sz->tell(seller->entity, "Your " + what + " sold for " + format_coin(l->price) + ". Collect it at an auctioneer.", *this);
  pending_auctions_.insert(l->id);
  mark_dirty(s);
  flush_item_saves();
}

void GameServer::handle(Session& s, const proto::AuctionCancel& m) {
  Zone* z = world_action(s, "auction in wrong state");
  if (!z || !z->at_auctioneer(s.entity, m.npc_id, *this)) return;
  AuctionListing* l = auctions_.find(static_cast<std::int64_t>(m.listing_id & 0x7FFFFFFFFFFFFFFFULL));
  if (!l || l->seller_char != s.char_id || l->state != ListingState::Active) return z->tell(s.entity, "You have no open listing like that.", *this);
  if (!z->give_from_auction(s.entity, {l->item_id, l->count, 0}, 0, *this)) return;
  const std::int64_t id = l->id;
  auctions_.remove(id);
  z->tell(s.entity, "You take your listing back.", *this);
  pending_auctions_.insert(id);
  mark_dirty(s);
  flush_item_saves();
  AuctionQuery mine;
  mine.mine = s.char_id;
  send_auction_page(s, mine);
}

void GameServer::expire_auctions() {
  if (!auctions_loaded_) return;
  const auto expired = auctions_.expire(unix_now());
  if (expired.empty()) return;
  for (AuctionListing* l : expired) pending_auctions_.insert(l->id);
  flush_item_saves();
}

void GameServer::flush_item_saves(bool all) {
  std::vector<db::CharacterRecord> batch;
  for (auto& [peer, s] : sessions_) {
    if (s.state != State::InWorld) continue;
    Zone* z = zone_for(s);
    PlayerEntity* p = z ? z->find(s.entity) : nullptr;
    if (!p || !(p->save_dirty || all)) continue;
    p->save_dirty = false;
    batch.push_back(snapshot(s, *p));
  }
  // Listings are written as they are now (or deleted if gone), whatever changed them in between.
  std::vector<db::AuctionRecord> up;
  std::vector<std::int64_t> del;
  for (std::int64_t id : pending_auctions_) {
    if (const AuctionListing* l = auctions_.find(id)) up.push_back(to_record(*l));
    else del.push_back(id);
  }
  pending_auctions_.clear();
  if (batch.empty() && up.empty() && del.empty() && !retry_saves_) return;
  retry_saves_ = false;
  db_.post([this, batch = std::move(batch), up = std::move(up), del = std::move(del)](db::Repository& repo, db::DbWorker& w) mutable {
    if (commit_with_carry(repo, std::move(batch), std::move(up), del)) return;
    w.complete([this] {
      spdlog::error("saving characters and auction listings failed three times; they are kept and retried with the next save");
      retry_saves_ = true;
    });
  });
}

bool GameServer::commit_with_carry(db::Repository& repo, std::vector<db::CharacterRecord> chars, std::vector<db::AuctionRecord> up,
                                   const std::vector<std::int64_t>& del) {
  // What failed before goes out with this batch; anything newer in this batch replaces it.
  SaveCarry merged = std::move(carry_);
  carry_ = {};
  for (auto& c : chars) merged.chars.insert_or_assign(c.id, std::move(c));
  for (auto& a : up) {
    merged.del.erase(a.id);
    merged.up.insert_or_assign(a.id, std::move(a));
  }
  for (std::int64_t id : del) {
    merged.up.erase(id);
    merged.del.insert(id);
  }
  if (merged.empty()) return true;
  std::vector<db::CharacterRecord> all_chars;
  all_chars.reserve(merged.chars.size());
  for (const auto& [id, c] : merged.chars) all_chars.push_back(c);
  std::vector<db::AuctionRecord> all_up;
  all_up.reserve(merged.up.size());
  for (const auto& [id, a] : merged.up) all_up.push_back(a);
  std::vector<std::int64_t> all_del(merged.del.begin(), merged.del.end());
  db::DbStatus st = db::DbStatus::Error;
  for (int attempt = 0; attempt < 3 && st != db::DbStatus::Ok; ++attempt) {
    if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));
    st = all_up.empty() && all_del.empty() ? repo.save_characters(all_chars) : repo.apply_auction(all_up, all_del, all_chars, true);
  }
  if (st == db::DbStatus::Ok) return true;
  carry_ = std::move(merged);
  return false;
}

// --------------------------------------------------------------------------- tick / lifecycle

void GameServer::tick() {
  ++tick_;
  db_.drain();
  for (auto& [id, zone] : zones_) zone->tick(tick_, *this);
  process_transfers();
  if (tick_ % std::max<std::uint32_t>(1, cfg_.tick_rate * 5) == 0) unload_idle_zones();
  process_closes();
  if (tick_ % std::max<std::uint32_t>(1, cfg_.tick_rate) == 0) {
    expire_lingers();
    expire_logins();
    flush_item_saves();
  }
  if (!auctions_loaded_ && !auctions_loading_ && auction_retry_tick_ != 0 && tick_ >= auction_retry_tick_) load_auctions();
  if (tick_ % std::max<std::uint32_t>(1, cfg_.tick_rate * 60) == 0) expire_auctions();
  if (!guilds_loaded_ && !guilds_loading_ && guild_retry_tick_ != 0 && tick_ >= guild_retry_tick_) load_guilds();
  if (tick_ >= next_social_tick_) {
    next_social_tick_ = tick_ + std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(content_.social.group_status_seconds *
                                                                                                   static_cast<float>(cfg_.tick_rate))));
    social_tick();
  }
  if (tick_ % autosave_every_ticks_ == 0) autosave();
}

void GameServer::autosave() {
  spdlog::debug("autosave: {} characters in the world", players_in_world());
  flush_item_saves(true);
}

void GameServer::shutdown(const std::string& reason) {
  autosave();
  for (auto& [peer, s] : sessions_) kick(s, reason);
  process_closes();
}

void GameServer::violation(Session& s, const char* reason) {
  ++s.violations;
  spdlog::warn("peer {} ({}) violation #{}: {}", s.peer, s.address, s.violations, reason);
  if (s.violations >= cfg_.max_violations) kick(s, "Disconnected: too many protocol errors.");
}

void GameServer::kick(Session& s, const std::string& reason) {
  if (s.closing) return;
  spdlog::info("kicking peer {} ({}): {}", s.peer, s.address, reason);
  send(s.peer, proto::Kick{reason});
  close_silently(s);
}

void GameServer::close_silently(Session& s) {
  if (s.closing) return;
  s.closing = true;
  pending_close_.push_back(s.peer);
}

void GameServer::process_closes() {
  for (net::PeerId peer : pending_close_) {
    Session* s = find_session(peer);
    if (!s) continue;
    if (s->lingering) {  // no connection left to close: leave the world and forget the session now
      end_linger(peer);
      continue;
    }
    if (s->state == State::InWorld) leave_world(*s);
    if (s->account_id != 0) {
      auto it = online_accounts_.find(s->account_id);
      if (it != online_accounts_.end() && it->second == peer) online_accounts_.erase(it);
    }
    transport_.disconnect_later(peer);
  }
  pending_close_.clear();
}

// --------------------------------------------------------------------------- ZoneSink

void GameServer::zone_send(const PlayerEntity& to, std::uint8_t channel, std::span<const std::uint8_t> packet) {
  transport_.send(to.session, channel, packet, channel == proto::kChannelReliable);
}

void GameServer::zone_player_died(const PlayerEntity& p, const CorpseEntity& corpse) {
  flush_item_saves();  // anything pending (a trade partner, a listing) is stored before this one character
  std::string zone_id;
  Zone* z = zone_of_player(p, &zone_id);
  Session* s = find_session(p.session);
  if (!z || !s) return;
  spdlog::info("'{}' ({}) died at {:.0f},{:.0f} in {}; corpse holds {} and {} stack(s), {} XP lost", p.name, s->username,
               p.pos.x, p.pos.z, zone_id, format_coin(corpse.coin), corpse.items.size(), corpse.xp_lost);
  auto rec = corpse_record(zone_id, corpse, *z);
  auto owner = snapshot(*s, p);
  db_.post([this, rec = std::move(rec), owner = std::move(owner)](db::Repository& repo, db::DbWorker&) {
    // Saves still waiting (perhaps both sides of a trade) go out first, together; this write then lands on top.
    // While they keep failing, this write waits too, so the stored state never splits a transfer.
    if (!carry_.empty() && !commit_with_carry(repo, {}, {}, {})) {
      spdlog::error("a corpse write was skipped: earlier saves are still failing");
      return;
    }
    repo.create_corpse(rec, owner);
  });
}

void GameServer::zone_corpse_looted(const CorpseEntity& corpse, const PlayerEntity& looter) {
  flush_item_saves();
  std::string zone_id;
  Zone* z = zone_of_player(looter, &zone_id);
  Session* s = find_session(looter.session);
  if (!z || !s) return;
  auto who = snapshot(*s, looter);
  if (!corpse.player_corpse) {
    mark_dirty(*s);
    flush_item_saves();
    return;
  }
  auto rec = corpse_record(zone_id, corpse, *z);
  db_.post([this, rec = std::move(rec), who = std::move(who)](db::Repository& repo, db::DbWorker&) {
    // Saves still waiting (perhaps both sides of a trade) go out first, together; this write then lands on top.
    // While they keep failing, this write waits too, so the stored state never splits a transfer.
    if (!carry_.empty() && !commit_with_carry(repo, {}, {}, {})) {
      spdlog::error("a corpse write was skipped: earlier saves are still failing");
      return;
    }
    repo.update_corpse(rec, who);
  });
}

void GameServer::zone_corpse_removed(const CorpseEntity& corpse) {
  if (!corpse.player_corpse || corpse.db_id == 0) return;
  spdlog::info("corpse of '{}' decayed", corpse.owner_name);
  const std::int64_t id = corpse.db_id;
  db_.post([id](db::Repository& repo, db::DbWorker&) { repo.delete_corpse(id); });
}

void GameServer::zone_character_changed(const PlayerEntity& p) { save_soon(p); }

void GameServer::zone_items_awarded(const PlayerEntity& p) { save_soon(p); }

void GameServer::zone_trade_completed(const PlayerEntity& a, const PlayerEntity& b) {
  Session* sa = find_session(a.session);
  Session* sb = find_session(b.session);
  if (!sa || !sb || sa->state != State::InWorld || sb->state != State::InWorld) return;
  spdlog::info("trade completed between '{}' and '{}'", a.name, b.name);
  // Both sides go into the same transaction (and stay together if it has to be retried).
  mark_dirty(*sa);
  mark_dirty(*sb);
  flush_item_saves();
}

void GameServer::zone_corpse_resurrected(const CorpseEntity& corpse, const PlayerEntity& owner) {
  flush_item_saves();
  std::string zone_id;
  Zone* z = zone_of_player(owner, &zone_id);
  Session* s = find_session(owner.session);
  if (!z || !s) return;
  spdlog::info("'{}' was resurrected at their corpse in {}", owner.name, zone_id);
  auto rec = corpse_record(zone_id, corpse, *z);
  auto who = snapshot(*s, owner);
  db_.post([this, rec = std::move(rec), who = std::move(who)](db::Repository& repo, db::DbWorker&) {
    // Saves still waiting (perhaps both sides of a trade) go out first, together; this write then lands on top.
    // While they keep failing, this write waits too, so the stored state never splits a transfer.
    if (!carry_.empty() && !commit_with_carry(repo, {}, {}, {})) {
      spdlog::error("a corpse write was skipped: earlier saves are still failing");
      return;
    }
    repo.update_corpse(rec, who);
  });
}

void GameServer::zone_correction(const PlayerEntity& to, const char* reason) {
  spdlog::debug("correcting '{}' ({})", to.name, reason);
  send(to.session, proto::MoveCorrection{to.last_applied_seq, to.pos.x, to.pos.y, to.pos.z, reason});
}

void GameServer::zone_violation(const PlayerEntity& who, const char* reason) {
  if (Session* s = find_session(who.session)) violation(*s, reason);
}

// --------------------------------------------------------------------------- Phase 8: reconnects

TickPhases GameServer::tick_phases() const {
  TickPhases out = unloaded_phases_;
  for (const auto& [id, z] : zones_)
    for (std::size_t i = 0; i < kTickPhaseCount; ++i) out[i] += z->tick_phases()[i];
  return out;
}

void GameServer::send_server_status(const Session& s) {
  proto::ServerStatus st;
  st.motd = cfg_.motd;
  st.players_online = static_cast<std::uint16_t>(std::min<std::size_t>(players_in_world(), 65535));
  st.max_players = static_cast<std::uint16_t>(std::min<std::uint32_t>(cfg_.max_players, 65535));
  st.server_version = EF_VERSION;
  send(s.peer, st);
}

bool GameServer::start_linger(Session& s) {
  if (cfg_.linger_seconds == 0 || s.closing || s.lingering) return false;
  Zone* z = zone_for(s);
  if (!z || !z->player_in_combat(s.entity)) return false;
  z->trade_cancel(s.entity, s.char_name + " is no longer here; the trade is cancelled.", *this);
  s.lingering = true;
  s.linger_until = tick_ + cfg_.linger_seconds * cfg_.tick_rate;
  lingering_.insert(s.peer);
  spdlog::info("'{}' ({}) dropped mid-fight; {} stays in the world for {} s", s.username, s.address, s.char_name, cfg_.linger_seconds);
  return true;
}

GameServer::Session* GameServer::lingering_for_account(std::int64_t account_id) {
  for (net::PeerId peer : lingering_)
    if (Session* s = find_session(peer); s && s->account_id == account_id) return s;
  return nullptr;
}

void GameServer::end_linger(net::PeerId peer) {
  lingering_.erase(peer);
  Session* s = find_session(peer);
  if (!s) return;
  spdlog::info("'{}': {} leaves the world after a dropped connection", s->username, s->char_name);
  if (s->state == State::InWorld) leave_world(*s);
  if (auto it = online_accounts_.find(s->account_id); it != online_accounts_.end() && it->second == peer) online_accounts_.erase(it);
  sessions_.erase(peer);
}

void GameServer::expire_logins() {
  for (auto& [peer, s] : sessions_)
    if ((s.state == State::AwaitHello || s.state == State::AwaitAuth) && !s.closing && !s.lingering &&
        now_ - s.connected_at > static_cast<double>(cfg_.login_timeout_seconds))
      kick(s, "Login timed out. Please try again.");
}

void GameServer::expire_lingers() {
  std::vector<net::PeerId> done;
  for (net::PeerId peer : lingering_) {
    const Session* s = find_session(peer);
    if (!s || tick_ >= s->linger_until) done.push_back(peer);
  }
  for (net::PeerId peer : done) end_linger(peer);
}

void GameServer::reclaim(Session& s, Session& old) {
  const net::PeerId old_peer = old.peer;
  Zone* z = zone_for(old);
  PlayerEntity* p = z ? z->find(old.entity) : nullptr;
  if (!p) {  // nothing to pick up after all: let it go and enter normally next time
    end_linger(old_peer);
    proto::EnterWorldResult r;
    r.ok = false;
    r.message = "Your character was still leaving the world. Please try again.";
    return send(s.peer, r);
  }
  s.state = State::InWorld;
  s.entity = old.entity;
  s.char_id = old.char_id;
  s.char_name = old.char_name;
  s.city = old.city;
  s.race = old.race;
  s.zone_id = old.zone_id;
  s.social = old.social;
  s.reply_to = old.reply_to;
  s.group_hash = 0;
  p->session = s.peer;
  z->reset_interest(p->id);
  z->reset_input_seq(p->id, false, 0);  // the new connection counts its inputs from 1
  online_chars_[s.char_id] = s.peer;
  lingering_.erase(old_peer);
  sessions_.erase(old_peer);
  spdlog::info("'{}' reclaimed {} in {} after a dropped connection", s.username, s.char_name, s.zone_id);
  send_enter_result(s, *z, *p, false);
  send_guild_info(s);
  send_friends(s);
  send_group_state(s, true);
  system_to(s, "You are back with your character.");
}

}  // namespace ef::game
