#include "game/game_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include <spdlog/spdlog.h>

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

std::uint64_t seed_for(const Config& cfg, std::size_t zone_index) {
  if (cfg.rng_seed != 0) return cfg.rng_seed + zone_index * 0x9E3779B97F4A7C15ULL;
  std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ zone_index;
}

std::vector<db::StoredItem> stored_items(const std::vector<ItemStackState>& items) {
  std::vector<db::StoredItem> out;
  for (std::size_t i = 0; i < items.size(); ++i)
    if (!items[i].empty()) out.push_back({static_cast<int>(i), items[i].item_id, items[i].count});
  return out;
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
      dt_(1.0f / static_cast<float>(cfg.tick_rate)) {
  for (std::size_t i = 0; i < content_.zones.size(); ++i) {
    const auto& z = content_.zones[i];
    zones_.emplace(z.id, std::make_unique<Zone>(z, content_, dt_, seed_for(cfg_, i)));
  }
  load_corpses();

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

void GameServer::load_corpses() {
  const std::int64_t now = unix_now();
  db_.post([this, now](db::Repository& repo, db::DbWorker& w) {
    auto corpses = repo.load_corpses(now);
    w.complete([this, now, corpses = std::move(corpses)] {
      std::size_t placed = 0;
      for (const auto& c : corpses) {
        Zone* z = zone(c.zone);
        if (!z) {
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
        for (const auto& it : c.items) init.items.push_back({it.item_id, it.count});
        init.xp_lost = c.xp_lost;
        init.seconds_left = static_cast<float>(std::max<std::int64_t>(1, c.expires_at - now));
        z->add_corpse(init);
        ++placed;
      }
      if (placed > 0) spdlog::info("restored {} player corpse(s)", placed);
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
  r.bind_zone = p.bind_zone;
  r.bind_x = p.bind_pos.x;
  r.bind_y = p.bind_pos.y;
  r.bind_z = p.bind_pos.z;
  r.bind_yaw = p.bind_yaw;
  for (const auto& sk : p.skills) r.skills.emplace_back(sk.id, sk.value);
  r.items = stored_items(std::vector<ItemStackState>(p.inventory.begin(), p.inventory.end()));
  r.spellbook = p.spellbook;
  r.gems.assign(p.gems.begin(), p.gems.end());
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
  std::vector<db::CharacterRecord> batch{snapshot(*s, p)};
  db_.post([batch = std::move(batch)](db::Repository& repo, db::DbWorker&) { repo.save_characters(batch); });
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
  sessions_.emplace(peer, Session(peer, address, cfg_));
  spdlog::debug("peer {} connected from {}", peer, address);
}

void GameServer::on_disconnect(net::PeerId peer) {
  Session* s = find_session(peer);
  if (!s) return;
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
    close_silently(s);
    return;
  }
  s.state = State::AwaitAuth;
  s.nonce = auth::random_hex_token(16);
  send(s.peer, proto::HelloResult{true, proto::kProtocolVersion, "Welcome to Realms of Emberfall.", login_port_, cert_pem_, s.nonce});
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
  if (auto it = online_accounts_.find(account.id); it != online_accounts_.end() && it->second != s.peer) {
    if (Session* old = find_session(it->second)) kick(*old, "Your account was logged in from another location.");
  }
  online_accounts_[account.id] = s.peer;
  s.account_id = account.id;
  s.username = account.username;
  s.is_admin = account.is_admin;
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
  rec.zone = city->zone;
  rec.x = city->spawn.x;
  rec.y = city->spawn.y;
  rec.z = city->spawn.z;
  rec.yaw = city->spawn.yaw;
  rec.bind_zone = city->zone;  // new characters are bound at their starting city
  rec.bind_x = city->spawn.x;
  rec.bind_y = city->spawn.y;
  rec.bind_z = city->spawn.z;
  rec.bind_yaw = city->spawn.yaw;
  rec.coin = static_cast<std::uint64_t>(std::max(0, content_.rules.starting_coin));

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
  ++s.inflight;
  const net::PeerId peer = s.peer;
  const std::int64_t account = s.account_id;
  const auto char_id = static_cast<std::int64_t>(m.char_id);
  db_.post([this, peer, account, char_id](db::Repository& repo, db::DbWorker& w) {
    const auto st = repo.delete_character(account, char_id);
    w.complete([this, peer, char_id, st] {
      Session* sp = find_session(peer);
      if (!sp) return;
      if (sp->inflight > 0) --sp->inflight;
      if (sp->closing) return;
      const bool ok = st == db::DbStatus::Ok;
      send(peer, proto::CharDeleteResult{ok, static_cast<std::uint64_t>(char_id), ok ? "Character deleted." : "Character not found."});
      if (ok) spdlog::info("account '{}' deleted character {}", sp->username, char_id);
      request_char_list(*sp);
    });
  });
}

// --------------------------------------------------------------------------- world

void GameServer::handle(Session& s, const proto::EnterWorld& m) {
  if (s.state != State::CharSelect) return violation(s, "enter world in wrong state");
  if (s.inflight > 0) return;
  s.state = State::EnteringWorld;
  ++s.inflight;
  const net::PeerId peer = s.peer;
  const std::int64_t account = s.account_id;
  const auto char_id = static_cast<std::int64_t>(m.char_id);
  db_.post([this, peer, account, char_id](db::Repository& repo, db::DbWorker& w) {
    auto rec = repo.load_character(account, char_id);
    w.complete([this, peer, rec] { on_enter_world(peer, rec); });
  });
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

  auto zit = zones_.find(rec->zone);
  Vec3 pos{rec->x, rec->y, rec->z};
  float yaw = rec->yaw;
  if (zit == zones_.end()) {
    spdlog::warn("character '{}' was in unknown zone '{}'; moving to default zone", rec->name, rec->zone);
    zit = zones_.find(content_.zones.front().id);
    const auto& sp = zit->second->def().safe_spawn;
    pos = {sp.x, sp.y, sp.z};
    yaw = sp.yaw;
  }
  Zone& zone = *zit->second;
  const ZoneDef& zd = zone.def();
  if (pos.x < zd.min_x || pos.x > zd.max_x || pos.z < zd.min_z || pos.z > zd.max_z) {
    pos = {zd.safe_spawn.x, zd.safe_spawn.y, zd.safe_spawn.z};
    yaw = zd.safe_spawn.yaw;
  }

  ZonePlayerInit init;
  init.session = peer;
  init.char_id = rec->id;
  init.name = rec->name;
  init.race = rec->race;
  init.class_id = rec->class_id;
  init.gender = rec->gender;
  init.level = std::clamp(rec->level, 1, content_.rules.max_level);
  init.pos = pos;
  init.yaw = yaw;
  init.xp = rec->xp;
  init.hp = rec->hp;
  init.mana = rec->mana;
  init.endurance = rec->endurance;
  init.coin = rec->coin;
  for (const auto& it : rec->items) init.items.emplace_back(it.slot, ItemStackState{it.item_id, it.count});
  for (const auto& [id, v] : rec->skills) init.skills.push_back({id, v});
  init.bind_zone = rec->bind_zone;
  init.bind_pos = {rec->bind_x, rec->bind_y, rec->bind_z};
  init.bind_yaw = rec->bind_yaw;
  init.spellbook = rec->spellbook;
  init.gems = rec->gems;
  for (const auto& b : rec->buffs) init.buffs.push_back({b.ability_id, b.caster_level, b.caster_name, b.seconds_left, b.next_tick_seconds});
  for (const auto& [id, left] : rec->timers) init.timers.push_back({id, left});
  if (init.bind_zone.empty()) {  // characters from before binds existed: their starting city
    if (const CityDef* city = content_.find_city(rec->city)) {
      init.bind_zone = city->zone;
      init.bind_pos = {city->spawn.x, city->spawn.y, city->spawn.z};
      init.bind_yaw = city->spawn.yaw;
    }
  }
  const EntityId eid = zone.add_player(init);
  const PlayerEntity* p = zone.find(eid);

  s->state = State::InWorld;
  s->entity = eid;
  s->char_id = rec->id;
  s->char_name = rec->name;
  s->city = rec->city;
  s->zone_id = zd.id;

  proto::EnterWorldResult r;
  r.ok = true;
  r.entity_id = eid;
  r.zone_id = zd.id;
  r.zone_name = zd.name;
  r.min_x = zd.min_x;
  r.min_z = zd.min_z;
  r.max_x = zd.max_x;
  r.max_z = zd.max_z;
  r.x = p->pos.x;
  r.y = p->pos.y;
  r.z = p->pos.z;
  r.yaw = p->yaw;
  r.run_speed = content_.rules.movement.run_speed;
  r.tick_rate = static_cast<std::uint8_t>(cfg_.tick_rate);
  r.server_tick = tick_;
  send(peer, r);
  zone.send_initial_state(eid, *this);
  send(peer, proto::ChatMessage{kChatSystem, "", "You have entered " + zd.name + "."});
  spdlog::info("'{}' entered {} as {} (entity {}, level {})", s->username, zd.id, rec->name, eid, p->level);
}

void GameServer::leave_world(Session& s) {
  if (s.state != State::InWorld) return;
  if (Zone* zone = zone_for(s)) {
    // A dead character camps out at their bind point; the corpse stays where it fell.
    if (const PlayerEntity* p = zone->find(s.entity); p && p->dead) zone->respawn(s.entity, *this);
    if (const PlayerEntity* p = zone->find(s.entity)) {
      std::vector<db::CharacterRecord> save{snapshot(s, *p)};
      db_.post([save = std::move(save)](db::Repository& repo, db::DbWorker&) { repo.save_characters(save); });
    }
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
  if (!s.chat_limiter.consume(1.0, now_)) {
    send(s.peer, proto::ChatMessage{kChatSystem, "", "You are sending messages too quickly."});
    return;
  }
  const std::string text = auth::sanitize_chat(m.text);
  if (text.empty()) return;
  Zone* zone = zone_for(s);
  if (!zone) return;
  const PlayerEntity* me = zone->find(s.entity);
  if (!me) return;
  std::vector<EntityId> near;
  zone->players_near(me->pos, zone->def().say_radius, near);
  const proto::ChatMessage msg{kChatSay, s.char_name, text};
  for (EntityId id : near) {
    if (const PlayerEntity* p = zone->find(id)) send(p->session, msg);
  }
  spdlog::info("[say] {}: {}", s.char_name, text);
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

// --------------------------------------------------------------------------- tick / lifecycle

void GameServer::tick() {
  ++tick_;
  db_.drain();
  for (auto& [id, zone] : zones_) zone->tick(tick_, *this);
  process_closes();
  if (tick_ % autosave_every_ticks_ == 0) autosave();
}

void GameServer::autosave() {
  std::vector<db::CharacterRecord> batch;
  for (auto& [peer, s] : sessions_) {
    if (s.state != State::InWorld) continue;
    Zone* zone = zone_for(s);
    const PlayerEntity* p = zone ? zone->find(s.entity) : nullptr;
    if (p) batch.push_back(snapshot(s, *p));
  }
  if (batch.empty()) return;
  spdlog::debug("autosave: {} characters", batch.size());
  db_.post([batch = std::move(batch)](db::Repository& repo, db::DbWorker&) { repo.save_characters(batch); });
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
  std::string zone_id;
  Zone* z = zone_of_player(p, &zone_id);
  Session* s = find_session(p.session);
  if (!z || !s) return;
  spdlog::info("'{}' ({}) died at {:.0f},{:.0f} in {}; corpse holds {} and {} stack(s), {} XP lost", p.name, s->username,
               p.pos.x, p.pos.z, zone_id, format_coin(corpse.coin), corpse.items.size(), corpse.xp_lost);
  auto rec = corpse_record(zone_id, corpse, *z);
  auto owner = snapshot(*s, p);
  db_.post([rec = std::move(rec), owner = std::move(owner)](db::Repository& repo, db::DbWorker&) { repo.create_corpse(rec, owner); });
}

void GameServer::zone_corpse_looted(const CorpseEntity& corpse, const PlayerEntity& looter) {
  std::string zone_id;
  Zone* z = zone_of_player(looter, &zone_id);
  Session* s = find_session(looter.session);
  if (!z || !s) return;
  auto who = snapshot(*s, looter);
  if (!corpse.player_corpse) {
    std::vector<db::CharacterRecord> batch{std::move(who)};
    db_.post([batch = std::move(batch)](db::Repository& repo, db::DbWorker&) { repo.save_characters(batch); });
    return;
  }
  auto rec = corpse_record(zone_id, corpse, *z);
  db_.post([rec = std::move(rec), who = std::move(who)](db::Repository& repo, db::DbWorker&) { repo.update_corpse(rec, who); });
}

void GameServer::zone_corpse_removed(const CorpseEntity& corpse) {
  if (!corpse.player_corpse || corpse.db_id == 0) return;
  spdlog::info("corpse of '{}' decayed", corpse.owner_name);
  const std::int64_t id = corpse.db_id;
  db_.post([id](db::Repository& repo, db::DbWorker&) { repo.delete_corpse(id); });
}

void GameServer::zone_character_changed(const PlayerEntity& p) { save_soon(p); }

void GameServer::zone_corpse_resurrected(const CorpseEntity& corpse, const PlayerEntity& owner) {
  std::string zone_id;
  Zone* z = zone_of_player(owner, &zone_id);
  Session* s = find_session(owner.session);
  if (!z || !s) return;
  spdlog::info("'{}' was resurrected at their corpse in {}", owner.name, zone_id);
  auto rec = corpse_record(zone_id, corpse, *z);
  auto who = snapshot(*s, owner);
  db_.post([rec = std::move(rec), who = std::move(who)](db::Repository& repo, db::DbWorker&) { repo.update_corpse(rec, who); });
}

void GameServer::zone_correction(const PlayerEntity& to, const char* reason) {
  spdlog::debug("correcting '{}' ({})", to.name, reason);
  send(to.session, proto::MoveCorrection{to.last_applied_seq, to.pos.x, to.pos.y, to.pos.z, reason});
}

void GameServer::zone_violation(const PlayerEntity& who, const char* reason) {
  if (Session* s = find_session(who.session)) violation(*s, reason);
}

}  // namespace ef::game
