#include "db/sqlite_repository.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sqlite3.h>

namespace ef::db {

namespace {

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// RAII prepared statement.
class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db) + " | " + sql);
    }
  }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind(int idx, std::int64_t v) {
    sqlite3_bind_int64(stmt_, idx, v);
    return *this;
  }
  Stmt& bind(int idx, int v) {
    sqlite3_bind_int(stmt_, idx, v);
    return *this;
  }
  Stmt& bind(int idx, float v) {
    sqlite3_bind_double(stmt_, idx, static_cast<double>(v));
    return *this;
  }
  Stmt& bind(int idx, std::string_view v) {
    sqlite3_bind_text(stmt_, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  // Returns SQLITE_ROW, SQLITE_DONE, or an error code.
  int step() { return sqlite3_step(stmt_); }
  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

  std::int64_t col_i64(int c) const { return sqlite3_column_int64(stmt_, c); }
  int col_int(int c) const { return sqlite3_column_int(stmt_, c); }
  float col_float(int c) const { return static_cast<float>(sqlite3_column_double(stmt_, c)); }
  std::string col_text(int c) const {
    const auto* p = sqlite3_column_text(stmt_, c);
    const int n = sqlite3_column_bytes(stmt_, c);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n)) : std::string();
  }
  const char* error() const { return sqlite3_errmsg(db_); }

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

std::int64_t to_i64(std::uint64_t v) { return static_cast<std::int64_t>(std::min<std::uint64_t>(v, INT64_MAX)); }
std::uint64_t to_u64(std::int64_t v) { return v < 0 ? 0 : static_cast<std::uint64_t>(v); }

std::string skills_json(const std::vector<std::pair<std::string, int>>& skills) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, v] : skills) j[id] = v;
  return j.dump();
}

std::vector<std::pair<std::string, int>> parse_skills(const std::string& text) {
  std::vector<std::pair<std::string, int>> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return out;
  for (const auto& [k, v] : j.items())
    if (v.is_number_integer()) out.emplace_back(k, std::clamp(v.get<int>(), 0, 10000));
  return out;
}

std::string strings_json(const std::vector<std::string>& v) { return nlohmann::json(v).dump(); }

std::vector<std::string> parse_strings(const std::string& text, std::size_t max) {
  std::vector<std::string> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_array()) return out;
  for (const auto& e : j) {
    if (out.size() >= max) break;
    out.push_back(e.is_string() ? e.get<std::string>() : std::string());
  }
  return out;
}

std::string buffs_json(const std::vector<StoredBuff>& buffs) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto& b : buffs)
    j.push_back({{"id", b.ability_id}, {"level", b.caster_level}, {"caster", b.caster_name}, {"left", b.seconds_left}, {"next", b.next_tick_seconds}});
  return j.dump();
}

std::vector<StoredBuff> parse_buffs(const std::string& text) {
  std::vector<StoredBuff> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_array()) return out;
  for (const auto& e : j) {
    if (!e.is_object() || !e.contains("id") || !e["id"].is_string() || !e.contains("left") || !e["left"].is_number()) continue;
    StoredBuff b;
    b.ability_id = e["id"].get<std::string>();
    b.caster_level = e.contains("level") && e["level"].is_number_integer() ? std::clamp(e["level"].get<int>(), 1, 255) : 1;
    b.caster_name = e.contains("caster") && e["caster"].is_string() ? e["caster"].get<std::string>() : std::string();
    b.seconds_left = std::clamp(e["left"].get<float>(), 0.0f, 86400.0f);
    b.next_tick_seconds = e.contains("next") && e["next"].is_number() ? std::clamp(e["next"].get<float>(), 0.0f, 3600.0f) : 0.0f;
    out.push_back(std::move(b));
    if (out.size() >= 64) break;
  }
  return out;
}

std::string timers_json(const std::vector<std::pair<std::string, float>>& timers) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, left] : timers) j[id] = left;
  return j.dump();
}

std::vector<std::pair<std::string, float>> parse_timers(const std::string& text) {
  std::vector<std::pair<std::string, float>> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return out;
  for (const auto& [k, v] : j.items())
    if (v.is_number() && out.size() < 256) out.emplace_back(k, std::clamp(v.get<float>(), 0.0f, 86400.0f));
  return out;
}

std::string items_json(const std::vector<StoredItem>& items) {
  nlohmann::json j = nlohmann::json::array();
  for (const auto& it : items) j.push_back({{"slot", it.slot}, {"item", it.item_id}, {"count", it.count}});
  return j.dump();
}

std::vector<StoredItem> parse_items(const std::string& text) {
  std::vector<StoredItem> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_array()) return out;
  for (const auto& e : j) {
    if (!e.is_object() || !e.contains("item") || !e["item"].is_string() || !e.contains("count") || !e["count"].is_number_integer()) continue;
    const int slot = e.contains("slot") && e["slot"].is_number_integer() ? e["slot"].get<int>() : 0;
    out.push_back({slot, e["item"].get<std::string>(), e["count"].get<int>()});
  }
  return out;
}

CharacterRecord read_character(const Stmt& s) {
  CharacterRecord r;
  r.id = s.col_i64(0);
  r.account_id = s.col_i64(1);
  r.name = s.col_text(2);
  r.race = s.col_text(3);
  r.class_id = s.col_text(4);
  r.city = s.col_text(5);
  r.level = s.col_int(6);
  r.zone = s.col_text(7);
  r.x = s.col_float(8);
  r.y = s.col_float(9);
  r.z = s.col_float(10);
  r.yaw = s.col_float(11);
  r.xp = to_u64(s.col_i64(12));
  r.hp = s.col_int(13);
  r.mana = s.col_int(14);
  r.endurance = s.col_int(15);
  r.coin = to_u64(s.col_i64(16));
  r.bind_zone = s.col_text(17);
  r.bind_x = s.col_float(18);
  r.bind_y = s.col_float(19);
  r.bind_z = s.col_float(20);
  r.bind_yaw = s.col_float(21);
  r.skills = parse_skills(s.col_text(22));
  r.spellbook = parse_strings(s.col_text(23), 256);
  r.gems = parse_strings(s.col_text(24), 8);
  r.buffs = parse_buffs(s.col_text(25));
  r.timers = parse_timers(s.col_text(26));
  return r;
}

constexpr const char* kCharColumns =
    "id, account_id, name, race, class_id, city, level, zone, x, y, z, yaw, "
    "xp, hp, mana, endurance, coin, bind_zone, bind_x, bind_y, bind_z, bind_yaw, skills, "
    "spellbook, gems, buffs, timers";

// BEGIN IMMEDIATE ... COMMIT, rolled back if commit() is never reached.
class Tx {
 public:
  explicit Tx(sqlite3* db) : db_(db) {
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("begin failed: ") + sqlite3_errmsg(db_));
  }
  ~Tx() {
    if (!done_) sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  }
  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;
  void commit() {
    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("commit failed: ") + sqlite3_errmsg(db_));
    done_ = true;
  }

 private:
  sqlite3* db_;
  bool done_ = false;
};

}  // namespace

SqliteRepository::SqliteRepository(const std::string& path) {
  const bool memory = path == ":memory:";
  if (!memory) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
  }
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("cannot open database " + path + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 5000);
  if (!memory) exec("PRAGMA journal_mode=WAL;");
  exec("PRAGMA synchronous=NORMAL;");
  exec("PRAGMA foreign_keys=ON;");
  migrate();
}

SqliteRepository::~SqliteRepository() { sqlite3_close(db_); }

void SqliteRepository::exec(const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "unknown error";
    sqlite3_free(err);
    throw std::runtime_error("sqlite exec failed: " + msg + " | " + sql);
  }
}

void SqliteRepository::migrate() {
  int version = 0;
  {
    Stmt v(db_, "PRAGMA user_version;");
    if (v.step() == SQLITE_ROW) version = v.col_int(0);
  }
  if (version < 1) {
    exec("BEGIN;");
    exec(R"sql(
      CREATE TABLE accounts (
        id            INTEGER PRIMARY KEY AUTOINCREMENT,
        username      TEXT NOT NULL UNIQUE COLLATE NOCASE,
        password_hash TEXT NOT NULL,
        is_admin      INTEGER NOT NULL DEFAULT 0,
        created_at    INTEGER NOT NULL
      );
      CREATE TABLE characters (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        name        TEXT NOT NULL UNIQUE COLLATE NOCASE,
        race        TEXT NOT NULL,
        class_id    TEXT NOT NULL,
        city        TEXT NOT NULL,
        level       INTEGER NOT NULL DEFAULT 1,
        zone        TEXT NOT NULL,
        x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL, yaw REAL NOT NULL,
        created_at  INTEGER NOT NULL,
        last_saved  INTEGER NOT NULL
      );
      CREATE INDEX characters_account ON characters(account_id);
      PRAGMA user_version = 1;
    )sql");
    exec("COMMIT;");
    version = 1;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 2) {
    exec("BEGIN;");
    exec(R"sql(
      ALTER TABLE characters ADD COLUMN xp INTEGER NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN hp INTEGER NOT NULL DEFAULT -1;
      ALTER TABLE characters ADD COLUMN mana INTEGER NOT NULL DEFAULT -1;
      ALTER TABLE characters ADD COLUMN endurance INTEGER NOT NULL DEFAULT -1;
      ALTER TABLE characters ADD COLUMN coin INTEGER NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN bind_zone TEXT NOT NULL DEFAULT '';
      ALTER TABLE characters ADD COLUMN bind_x REAL NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN bind_y REAL NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN bind_z REAL NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN bind_yaw REAL NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN skills TEXT NOT NULL DEFAULT '{}';
      CREATE TABLE character_items (
        char_id  INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
        slot     INTEGER NOT NULL,
        item_id  TEXT NOT NULL,
        count    INTEGER NOT NULL,
        PRIMARY KEY (char_id, slot)
      );
      CREATE TABLE corpses (
        id          INTEGER PRIMARY KEY,
        char_id     INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
        owner_name  TEXT NOT NULL,
        level       INTEGER NOT NULL,
        zone        TEXT NOT NULL,
        x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL, yaw REAL NOT NULL,
        coin        INTEGER NOT NULL,
        xp_lost     INTEGER NOT NULL,
        expires_at  INTEGER NOT NULL,
        items       TEXT NOT NULL
      );
      CREATE INDEX corpses_char ON corpses(char_id);
      PRAGMA user_version = 2;
    )sql");
    exec("COMMIT;");
    version = 2;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 3) {
    exec("BEGIN;");
    exec(R"sql(
      ALTER TABLE characters ADD COLUMN spellbook TEXT NOT NULL DEFAULT '[]';
      ALTER TABLE characters ADD COLUMN gems TEXT NOT NULL DEFAULT '[]';
      ALTER TABLE characters ADD COLUMN buffs TEXT NOT NULL DEFAULT '[]';
      ALTER TABLE characters ADD COLUMN timers TEXT NOT NULL DEFAULT '{}';
      PRAGMA user_version = 3;
    )sql");
    exec("COMMIT;");
    version = 3;
    spdlog::info("database migrated to schema v{}", version);
  }
}

std::optional<AccountRecord> SqliteRepository::find_account(std::string_view username) {
  Stmt s(db_, "SELECT id, username, password_hash, is_admin FROM accounts WHERE username = ?1;");
  s.bind(1, username);
  if (s.step() != SQLITE_ROW) return std::nullopt;
  return AccountRecord{s.col_i64(0), s.col_text(1), s.col_text(2), s.col_int(3) != 0};
}

DbStatus SqliteRepository::create_account(std::string_view username, std::string_view password_hash, std::int64_t& out_id) {
  Stmt s(db_, "INSERT INTO accounts (username, password_hash, created_at) VALUES (?1, ?2, ?3);");
  s.bind(1, username).bind(2, password_hash).bind(3, unix_now());
  const int rc = s.step();
  if (rc == SQLITE_CONSTRAINT) return DbStatus::Duplicate;
  if (rc != SQLITE_DONE) {
    spdlog::error("create_account failed: {}", s.error());
    return DbStatus::Error;
  }
  out_id = sqlite3_last_insert_rowid(db_);
  return DbStatus::Ok;
}

std::vector<CharacterRecord> SqliteRepository::list_characters(std::int64_t account_id) {
  std::vector<CharacterRecord> out;
  Stmt s(db_, (std::string("SELECT ") + kCharColumns + " FROM characters WHERE account_id = ?1 ORDER BY id;").c_str());
  s.bind(1, account_id);
  int rc = SQLITE_ROW;
  while ((rc = s.step()) == SQLITE_ROW) out.push_back(read_character(s));
  if (rc != SQLITE_DONE) spdlog::error("list_characters {}: query failed: {}", account_id, s.error());
  return out;
}

DbStatus SqliteRepository::create_character(CharacterRecord& rec, int max_per_account) {
  exec("BEGIN IMMEDIATE;");
  try {
    Stmt count(db_, "SELECT COUNT(*) FROM characters WHERE account_id = ?1;");
    count.bind(1, rec.account_id);
    if (count.step() != SQLITE_ROW) throw std::runtime_error(count.error());
    const int existing = count.col_int(0);
    count.reset();
    if (existing >= max_per_account) {
      exec("ROLLBACK;");
      return DbStatus::LimitReached;
    }
    Stmt ins(db_,
             "INSERT INTO characters (account_id, name, race, class_id, city, level, zone, x, y, z, yaw, created_at, last_saved,"
             " bind_zone, bind_x, bind_y, bind_z, bind_yaw, coin)"
             " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?12, ?13, ?14, ?15, ?16, ?17, ?18);");
    ins.bind(1, rec.account_id).bind(2, rec.name).bind(3, rec.race).bind(4, rec.class_id).bind(5, rec.city);
    ins.bind(6, rec.level).bind(7, rec.zone).bind(8, rec.x).bind(9, rec.y).bind(10, rec.z).bind(11, rec.yaw);
    ins.bind(12, unix_now());
    ins.bind(13, rec.bind_zone).bind(14, rec.bind_x).bind(15, rec.bind_y).bind(16, rec.bind_z).bind(17, rec.bind_yaw);
    ins.bind(18, to_i64(rec.coin));
    const int rc = ins.step();
    if (rc == SQLITE_CONSTRAINT) {
      exec("ROLLBACK;");
      return DbStatus::Duplicate;
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(ins.error());
    rec.id = sqlite3_last_insert_rowid(db_);
    exec("COMMIT;");
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("create_character failed: {}", e.what());
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::delete_character(std::int64_t account_id, std::int64_t char_id) {
  Stmt s(db_, "DELETE FROM characters WHERE id = ?1 AND account_id = ?2;");
  s.bind(1, char_id).bind(2, account_id);
  if (s.step() != SQLITE_DONE) {
    spdlog::error("delete_character failed: {}", s.error());
    return DbStatus::Error;
  }
  return sqlite3_changes(db_) > 0 ? DbStatus::Ok : DbStatus::NotFound;
}

std::optional<CharacterRecord> SqliteRepository::load_character(std::int64_t account_id, std::int64_t char_id) {
  Stmt s(db_, (std::string("SELECT ") + kCharColumns + " FROM characters WHERE id = ?1 AND account_id = ?2;").c_str());
  s.bind(1, char_id).bind(2, account_id);
  if (s.step() != SQLITE_ROW) return std::nullopt;
  CharacterRecord rec = read_character(s);
  Stmt items(db_, "SELECT slot, item_id, count FROM character_items WHERE char_id = ?1 ORDER BY slot;");
  items.bind(1, char_id);
  int rc = SQLITE_ROW;
  while ((rc = items.step()) == SQLITE_ROW) rec.items.push_back({items.col_int(0), items.col_text(1), items.col_int(2)});
  if (rc != SQLITE_DONE) {
    // A partial pack must never look complete: the next save would erase the unread items.
    spdlog::error("load_character {}: reading the pack failed: {}", char_id, items.error());
    return std::nullopt;
  }
  return rec;
}

void SqliteRepository::save_one(const CharacterRecord& c) {
  Stmt s(db_,
         "UPDATE characters SET level = ?1, zone = ?2, x = ?3, y = ?4, z = ?5, yaw = ?6, xp = ?7, hp = ?8, mana = ?9,"
         " endurance = ?10, coin = ?11, bind_zone = ?12, bind_x = ?13, bind_y = ?14, bind_z = ?15, bind_yaw = ?16,"
         " skills = ?17, last_saved = ?18, spellbook = ?20, gems = ?21, buffs = ?22, timers = ?23 WHERE id = ?19;");
  s.bind(1, c.level).bind(2, c.zone).bind(3, c.x).bind(4, c.y).bind(5, c.z).bind(6, c.yaw);
  s.bind(7, to_i64(c.xp)).bind(8, c.hp).bind(9, c.mana).bind(10, c.endurance).bind(11, to_i64(c.coin));
  s.bind(12, c.bind_zone).bind(13, c.bind_x).bind(14, c.bind_y).bind(15, c.bind_z).bind(16, c.bind_yaw);
  s.bind(17, skills_json(c.skills)).bind(18, unix_now()).bind(19, c.id);
  s.bind(20, strings_json(c.spellbook)).bind(21, strings_json(c.gems)).bind(22, buffs_json(c.buffs)).bind(23, timers_json(c.timers));
  if (s.step() != SQLITE_DONE) throw std::runtime_error(s.error());
  // A missing row must fail the whole transaction (e.g. a loot transfer would otherwise commit
  // the corpse side alone).
  if (sqlite3_changes(db_) == 0) throw std::runtime_error("character " + std::to_string(c.id) + " does not exist");
  Stmt del(db_, "DELETE FROM character_items WHERE char_id = ?1;");
  del.bind(1, c.id);
  if (del.step() != SQLITE_DONE) throw std::runtime_error(del.error());
  Stmt ins(db_, "INSERT INTO character_items (char_id, slot, item_id, count) VALUES (?1, ?2, ?3, ?4);");
  for (const auto& it : c.items) {
    ins.bind(1, c.id).bind(2, it.slot).bind(3, it.item_id).bind(4, it.count);
    if (ins.step() != SQLITE_DONE) throw std::runtime_error(ins.error());
    ins.reset();
  }
}

void SqliteRepository::write_corpse(const CorpseRecord& c) {
  Stmt s(db_,
         "INSERT OR REPLACE INTO corpses (id, char_id, owner_name, level, zone, x, y, z, yaw, coin, xp_lost, expires_at, items)"
         " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13);");
  s.bind(1, c.id).bind(2, c.char_id).bind(3, c.owner_name).bind(4, c.level).bind(5, c.zone);
  s.bind(6, c.x).bind(7, c.y).bind(8, c.z).bind(9, c.yaw).bind(10, to_i64(c.coin)).bind(11, to_i64(c.xp_lost));
  s.bind(12, c.expires_at).bind(13, items_json(c.items));
  if (s.step() != SQLITE_DONE) throw std::runtime_error(s.error());
}

DbStatus SqliteRepository::save_characters(const std::vector<CharacterRecord>& chars) {
  if (chars.empty()) return DbStatus::Ok;
  try {
    Tx tx(db_);
    for (const auto& c : chars) save_one(c);
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("save_characters failed: {}", e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::create_corpse(const CorpseRecord& corpse, const CharacterRecord& owner_after) {
  try {
    Tx tx(db_);
    write_corpse(corpse);
    save_one(owner_after);
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("create_corpse failed: {}", e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::update_corpse(const CorpseRecord& corpse, const CharacterRecord& looter) {
  try {
    Tx tx(db_);
    write_corpse(corpse);
    save_one(looter);
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("update_corpse failed: {}", e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::delete_corpse(std::int64_t corpse_id) {
  Stmt s(db_, "DELETE FROM corpses WHERE id = ?1;");
  s.bind(1, corpse_id);
  if (s.step() != SQLITE_DONE) {
    spdlog::error("delete_corpse failed: {}", s.error());
    return DbStatus::Error;
  }
  return sqlite3_changes(db_) > 0 ? DbStatus::Ok : DbStatus::NotFound;
}

std::vector<CorpseRecord> SqliteRepository::load_corpses(std::int64_t now_unix) {
  std::vector<CorpseRecord> out;
  {
    Stmt del(db_, "DELETE FROM corpses WHERE expires_at <= ?1;");
    del.bind(1, now_unix);
    if (del.step() != SQLITE_DONE) {
      spdlog::error("expiring corpses failed: {}", del.error());
    } else if (const int n = sqlite3_changes(db_); n > 0) {
      spdlog::info("{} expired corpse(s) decayed while the server was down", n);
    }
  }
  // Filter here too: the cleanup DELETE above can fail (busy database) without the SELECT failing.
  Stmt s(db_, "SELECT id, char_id, owner_name, level, zone, x, y, z, yaw, coin, xp_lost, expires_at, items FROM corpses"
              " WHERE expires_at > ?1 ORDER BY id;");
  s.bind(1, now_unix);
  int rc = SQLITE_ROW;
  while ((rc = s.step()) == SQLITE_ROW) {
    CorpseRecord c;
    c.id = s.col_i64(0);
    c.char_id = s.col_i64(1);
    c.owner_name = s.col_text(2);
    c.level = s.col_int(3);
    c.zone = s.col_text(4);
    c.x = s.col_float(5);
    c.y = s.col_float(6);
    c.z = s.col_float(7);
    c.yaw = s.col_float(8);
    c.coin = to_u64(s.col_i64(9));
    c.xp_lost = to_u64(s.col_i64(10));
    c.expires_at = s.col_i64(11);
    c.items = parse_items(s.col_text(12));
    out.push_back(std::move(c));
  }
  // Corpses not read stay in the table and are restored on the next start; say so loudly.
  if (rc != SQLITE_DONE) spdlog::error("load_corpses: query failed after {} corpse(s): {}", out.size(), s.error());
  return out;
}

}  // namespace ef::db
