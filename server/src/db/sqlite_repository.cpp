#include "db/sqlite_repository.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>

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
  return r;
}

constexpr const char* kCharColumns = "id, account_id, name, race, class_id, city, level, zone, x, y, z, yaw";

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
  while (s.step() == SQLITE_ROW) out.push_back(read_character(s));
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
             "INSERT INTO characters (account_id, name, race, class_id, city, level, zone, x, y, z, yaw, created_at, last_saved)"
             " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?12);");
    ins.bind(1, rec.account_id).bind(2, rec.name).bind(3, rec.race).bind(4, rec.class_id).bind(5, rec.city);
    ins.bind(6, rec.level).bind(7, rec.zone).bind(8, rec.x).bind(9, rec.y).bind(10, rec.z).bind(11, rec.yaw);
    ins.bind(12, unix_now());
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
  return read_character(s);
}

DbStatus SqliteRepository::save_positions(const std::vector<CharacterPosition>& positions) {
  if (positions.empty()) return DbStatus::Ok;
  exec("BEGIN;");
  try {
    Stmt s(db_, "UPDATE characters SET zone = ?1, x = ?2, y = ?3, z = ?4, yaw = ?5, last_saved = ?6 WHERE id = ?7;");
    const auto now = unix_now();
    for (const auto& p : positions) {
      s.bind(1, p.zone).bind(2, p.x).bind(3, p.y).bind(4, p.z).bind(5, p.yaw).bind(6, now).bind(7, p.char_id);
      if (s.step() != SQLITE_DONE) throw std::runtime_error(s.error());
      s.reset();
    }
    exec("COMMIT;");
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("save_positions failed: {}", e.what());
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return DbStatus::Error;
  }
}

}  // namespace ef::db
