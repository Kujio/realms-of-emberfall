#include "db/sqlite_repository.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

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

// Faction standings: id -> integer, kept within a sane range (they may be negative).
std::vector<std::pair<std::string, int>> parse_factions(const std::string& text) {
  std::vector<std::pair<std::string, int>> out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return out;
  for (const auto& [k, v] : j.items())
    if (v.is_number_integer() && k.size() <= 32 && out.size() < 256) out.emplace_back(k, std::clamp(v.get<int>(), -5000, 5000));
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
  r.gender = std::clamp(s.col_int(27), 0, 1);
  r.bank_coin = to_u64(s.col_i64(28));
  r.factions = parse_factions(s.col_text(29));
  r.quests = s.col_text(30);
  if (r.quests.empty()) r.quests = "{}";
  r.social = s.col_text(31);
  if (r.social.empty()) r.social = "{}";
  r.lockouts = s.col_text(32);
  if (r.lockouts.empty()) r.lockouts = "{}";
  r.books = parse_strings(s.col_text(33), 256);
  return r;
}

constexpr const char* kCharColumns =
    "id, account_id, name, race, class_id, city, level, zone, x, y, z, yaw, "
    "xp, hp, mana, endurance, coin, bind_zone, bind_x, bind_y, bind_z, bind_yaw, skills, "
    "spellbook, gems, buffs, timers, gender, bank_coin, factions, quests, social, lockouts, books";

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

namespace {

// SQLite result codes that mean the file itself is damaged or is not a database.
bool is_corruption(int code) {
  const int primary = code & 0xFF;
  return primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB;
}

}  // namespace

SqliteRepository::SqliteRepository(const std::string& path, DbCheck check, OpenMode mode) {
  const bool memory = path == ":memory:";
  const bool server = mode == OpenMode::Server;
  if (!memory && server) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
  }
  if (!memory && !server && !std::filesystem::exists(path)) throw std::runtime_error("no database at " + path);
  // Operator tools never create a database (a typo must not "check" or back up an empty file).
  const int flags = SQLITE_OPEN_READWRITE | (server ? SQLITE_OPEN_CREATE : 0) | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("cannot open database " + path + ": " + msg);
  }
  sqlite3_busy_timeout(db_, 5000);
  auto fail_open = [&](const std::string& msg, bool corrupt) {
    sqlite3_close(db_);
    db_ = nullptr;
    if (corrupt) throw DbCorruptError(msg);
    throw std::runtime_error(msg);
  };
  if (!memory && check != DbCheck::Off) {
    // Before anything writes to it: a damaged file is reported, never "repaired" by a migration. A check
    // that cannot run (the file is locked, say) is an ordinary error, not corruption.
    std::vector<std::string> problems;
    try {
      problems = integrity_problems(check == DbCheck::Full);
    } catch (const std::exception& e) {
      fail_open("cannot check database " + path + ": " + e.what(), false);
    }
    if (!problems.empty()) {
      std::string msg = "database " + path + " failed its integrity check:";
      for (std::size_t i = 0; i < problems.size() && i < 10; ++i) msg += "\n  " + problems[i];
      fail_open(msg, true);
    }
  }
  if (!memory && server) {
    char* err = nullptr;
    if (sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err) != SQLITE_OK) {
      const int code = sqlite3_extended_errcode(db_);
      const std::string why = err ? err : "unknown error";
      sqlite3_free(err);
      fail_open("database " + path + " cannot be opened (" + why + ")", is_corruption(code));
    }
  }
  if (!server) return;  // operator tools leave the schema exactly as they found it
  exec("PRAGMA synchronous=NORMAL;");
  exec("PRAGMA foreign_keys=ON;");
  migrate();
}

SqliteRepository::~SqliteRepository() { sqlite3_close(db_); }

std::vector<std::string> SqliteRepository::integrity_problems(bool full) {
  std::vector<std::string> out;
  sqlite3_stmt* st = nullptr;
  const char* sql = full ? "PRAGMA integrity_check(100);" : "PRAGMA quick_check(100);";
  auto failed = [&]() {
    const int code = sqlite3_extended_errcode(db_);
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!is_corruption(code)) throw std::runtime_error(msg);  // busy, locked, out of memory: not a verdict
    out.push_back(msg);
    return out;
  };
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return failed();
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const unsigned char* t = sqlite3_column_text(st, 0);
    const std::string line = t ? reinterpret_cast<const char*>(t) : "";
    if (line != "ok") out.push_back(line);
  }
  if (rc != SQLITE_DONE) return failed();
  sqlite3_finalize(st);
  return out;
}

void SqliteRepository::backup_to(const std::string& dest) {
  if (dest.empty()) throw std::runtime_error("no backup destination given");
  // A backup always goes to a new file: replacing an existing one could pair it with a stale journal.
  if (std::filesystem::exists(dest) || std::filesystem::exists(dest + "-wal") || std::filesystem::exists(dest + "-journal"))
    throw std::runtime_error("backup destination already exists: " + dest + " (choose a new name)");
  if (const auto parent = std::filesystem::path(dest).parent_path(); !parent.empty()) std::filesystem::create_directories(parent);
  // This invocation's own staging file, created exclusively and private (it holds password hashes).
  std::string tmp;
  for (int attempt = 0; attempt < 16 && tmp.empty(); ++attempt) {
    std::uint64_t r = 0;
    sqlite3_randomness(sizeof r, &r);
    char suffix[40];
    std::snprintf(suffix, sizeof suffix, ".partial-%016llx", static_cast<unsigned long long>(r));
    const std::string candidate = dest + suffix;
    const int fd = ::open(candidate.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd >= 0) {
      ::close(fd);
      tmp = candidate;
    }
  }
  if (tmp.empty()) throw std::runtime_error("cannot create a staging file next to " + dest);
  {
    // Never write over the live database (or its journal files) under any name for it.
    const char* src_name = sqlite3_db_filename(db_, "main");
    const std::string src = src_name ? src_name : "";
    std::error_code ec;
    auto canon = [&](const std::string& p) { return std::filesystem::weakly_canonical(std::filesystem::absolute(p, ec), ec); };
    if (!src.empty())
      for (const char* ext : {"", "-wal", "-shm", "-journal"})
        if (canon(dest) == canon(src + ext)) {
          std::filesystem::remove(tmp, ec);
          throw std::runtime_error("refusing to back up over the live database: " + dest);
        }
  }
  // Whatever happens from here on, the staging file goes unless it was published.
  struct Staging {
    std::string path;
    bool keep = false;
    ~Staging() {
      std::error_code ec;
      if (!keep)
        for (const char* ext : {"", "-wal", "-shm", "-journal"}) std::filesystem::remove(path + ext, ec);
    }
  } staging{tmp};

  sqlite3* out = nullptr;
  if (sqlite3_open_v2(tmp.c_str(), &out, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    const std::string msg = out ? sqlite3_errmsg(out) : "out of memory";
    sqlite3_close(out);
    throw std::runtime_error("cannot open the staging file " + tmp + ": " + msg);
  }
  sqlite3_backup* b = sqlite3_backup_init(out, "main", db_, "main");
  if (!b) {
    const std::string msg = sqlite3_errmsg(out);
    sqlite3_close(out);
    throw std::runtime_error("cannot start backup: " + msg);
  }
  int rc = SQLITE_OK;
  // A few hundred pages at a time, so a busy server is never locked out for long; give up after ten
  // minutes of contention or restarts (the server writing faster than the copy can follow).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  bool timed_out = false;
  while ((rc = sqlite3_backup_step(b, 256)) == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
    if (std::chrono::steady_clock::now() > deadline) {
      timed_out = true;
      break;
    }
    sqlite3_sleep(5);
  }
  sqlite3_backup_finish(b);
  if (rc != SQLITE_DONE) {
    const std::string msg = timed_out ? std::string("timed out after ten minutes") : std::string(sqlite3_errmsg(out));
    sqlite3_close(out);
    throw std::runtime_error("backup failed: " + msg);
  }
  sqlite3_close(out);
  {
    SqliteRepository check(tmp, DbCheck::Off, OpenMode::Operator);
    const auto problems = check.integrity_problems(true);
    if (!problems.empty()) throw std::runtime_error("the backup copy failed its integrity check: " + problems.front());
  }
  // link() refuses an existing name, so two backups racing for one destination cannot replace each other.
  if (::link(tmp.c_str(), dest.c_str()) != 0) {
    const std::error_code ec(errno, std::generic_category());
    throw std::runtime_error("cannot publish the backup as " + dest + ": " + ec.message());
  }
  // The staging name goes; the published name keeps the checked copy.
}

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
  if (version < 4) {
    // Phase 3.5: a character's gender picks its model (0 = female, 1 = male). Older characters become female.
    exec("BEGIN;");
    exec(R"sql(
      ALTER TABLE characters ADD COLUMN gender INTEGER NOT NULL DEFAULT 0;
      PRAGMA user_version = 4;
    )sql");
    exec("COMMIT;");
    version = 4;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 5) {
    // Phase 4: summoned items keep the time they have left.
    // Databases built by tests from the current DDL may already have the column.
    bool has_column = false;
    {
      Stmt cols(db_, "PRAGMA table_info(character_items);");
      while (cols.step() == SQLITE_ROW)
        if (cols.col_text(1) == "seconds_left") has_column = true;
    }
    exec("BEGIN;");
    if (!has_column) exec("ALTER TABLE character_items ADD COLUMN seconds_left REAL NOT NULL DEFAULT 0;");
    exec("PRAGMA user_version = 5;");
    exec("COMMIT;");
    version = 5;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 6) {
    // Phase 5: slot codes. The old 16-slot pack becomes the 8-slot pack (0-7) and the first 8 bank
    // slots (8-15); bank coin; the auction board. Old corpse items lose their (meaningless) slot index.
    // Databases built by tests from the current DDL (then rolled back) may already have the column and table.
    bool has_bank_coin = false;
    {
      Stmt cols(db_, "PRAGMA table_info(characters);");
      while (cols.step() == SQLITE_ROW)
        if (cols.col_text(1) == "bank_coin") has_bank_coin = true;
    }
    exec("BEGIN;");
    if (!has_bank_coin) exec("ALTER TABLE characters ADD COLUMN bank_coin INTEGER NOT NULL DEFAULT 0;");
    exec(R"sql(
      UPDATE character_items SET slot = CASE WHEN slot < 8 THEN 256 + slot ELSE 512 + slot - 8 END WHERE slot < 16;
      CREATE TABLE IF NOT EXISTS auctions (
        id          INTEGER PRIMARY KEY,
        seller_char INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
        seller_name TEXT NOT NULL,
        item_id     TEXT NOT NULL,
        count       INTEGER NOT NULL,
        price       INTEGER NOT NULL,
        listed_at   INTEGER NOT NULL,
        expires_at  INTEGER NOT NULL,
        state       INTEGER NOT NULL,
        buyer       TEXT NOT NULL DEFAULT ''
      );
      CREATE INDEX IF NOT EXISTS auctions_seller ON auctions(seller_char);
    )sql");
    {
      std::vector<std::pair<std::int64_t, std::string>> rewrite;
      Stmt c(db_, "SELECT id, items FROM corpses;");
      int rc = SQLITE_ROW;
      while ((rc = c.step()) == SQLITE_ROW) {
        auto items = parse_items(c.col_text(1));
        for (auto& it : items) it.slot = 0xFFFF;
        rewrite.emplace_back(c.col_i64(0), items_json(items));
      }
      if (rc != SQLITE_DONE) {
        // A partial scan would commit v6 with some corpses not rewritten: give up and leave the database at v5.
        const std::string why = c.error();
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        throw std::runtime_error("schema v6 migration: reading corpses failed: " + why);
      }
      Stmt u(db_, "UPDATE corpses SET items = ?1 WHERE id = ?2;");
      for (const auto& [id, text] : rewrite) {
        u.bind(1, text).bind(2, id);
        if (u.step() != SQLITE_DONE) throw std::runtime_error(u.error());
        u.reset();
      }
    }
    exec("PRAGMA user_version = 6;");
    exec("COMMIT;");
    version = 6;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 7) {
    // Phase 6: faction standing and quest progress.
    exec(R"sql(
      BEGIN;
      ALTER TABLE characters ADD COLUMN factions TEXT NOT NULL DEFAULT '{}';
      ALTER TABLE characters ADD COLUMN quests TEXT NOT NULL DEFAULT '{}';
      PRAGMA user_version = 7;
      COMMIT;
    )sql");
    version = 7;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 8) {
    // Phase 7: bans and mutes, social lists, guilds, and the game master audit log.
    exec(R"sql(
      BEGIN;
      ALTER TABLE accounts ADD COLUMN banned_until INTEGER NOT NULL DEFAULT 0;
      ALTER TABLE accounts ADD COLUMN ban_reason TEXT NOT NULL DEFAULT '';
      ALTER TABLE accounts ADD COLUMN muted_until INTEGER NOT NULL DEFAULT 0;
      ALTER TABLE characters ADD COLUMN social TEXT NOT NULL DEFAULT '{}';
      CREATE TABLE guilds (
        id          INTEGER PRIMARY KEY AUTOINCREMENT,
        name        TEXT NOT NULL UNIQUE COLLATE NOCASE,
        motd        TEXT NOT NULL DEFAULT '',
        created_at  INTEGER NOT NULL
      );
      CREATE TABLE guild_members (
        char_id   INTEGER PRIMARY KEY REFERENCES characters(id) ON DELETE CASCADE,
        guild_id  INTEGER NOT NULL REFERENCES guilds(id) ON DELETE CASCADE,
        rank      INTEGER NOT NULL
      );
      CREATE INDEX guild_members_guild ON guild_members(guild_id);
      CREATE TABLE audit_log (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        at         INTEGER NOT NULL,
        account    TEXT NOT NULL,
        character  TEXT NOT NULL,
        command    TEXT NOT NULL,
        target     TEXT NOT NULL,
        detail     TEXT NOT NULL
      );
      PRAGMA user_version = 8;
      COMMIT;
    )sql");
    version = 8;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 9) {
    // Phase 12: groups survive logouts and restarts.
    exec(R"sql(
      BEGIN;
      CREATE TABLE IF NOT EXISTS groups (
        id         INTEGER PRIMARY KEY,
        leader     INTEGER NOT NULL,
        loot_rule  INTEGER NOT NULL DEFAULT 0
      );
      CREATE TABLE IF NOT EXISTS group_members (
        char_id   INTEGER PRIMARY KEY REFERENCES characters(id) ON DELETE CASCADE,
        group_id  INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
        slot      INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS group_members_group ON group_members(group_id);
      PRAGMA user_version = 9;
      COMMIT;
    )sql");
    version = 9;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 10) {
    // Phase 14: raids (whole groups joined together) survive restarts.
    exec(R"sql(
      BEGIN;
      CREATE TABLE IF NOT EXISTS raids (
        id         INTEGER PRIMARY KEY,
        leader     INTEGER NOT NULL,
        loot_rule  INTEGER NOT NULL DEFAULT 0
      );
      CREATE TABLE IF NOT EXISTS raid_groups (
        group_id  INTEGER PRIMARY KEY,
        raid_id   INTEGER NOT NULL REFERENCES raids(id) ON DELETE CASCADE,
        slot      INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS raid_groups_raid ON raid_groups(raid_id);
      PRAGMA user_version = 10;
      COMMIT;
    )sql");
    version = 10;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 11) {
    // Phase 15: raid lockouts on characters, and a raid's main assist.
    exec(R"sql(
      BEGIN;
      ALTER TABLE characters ADD COLUMN lockouts TEXT NOT NULL DEFAULT '{}';
      ALTER TABLE raids ADD COLUMN main_assist INTEGER NOT NULL DEFAULT 0;
      PRAGMA user_version = 11;
      COMMIT;
    )sql");
    version = 11;
    spdlog::info("database migrated to schema v{}", version);
  }
  if (version < 12) {
    // Phase 16: the recipe books each character has read.
    exec(R"sql(
      BEGIN;
      ALTER TABLE characters ADD COLUMN books TEXT NOT NULL DEFAULT '[]';
      PRAGMA user_version = 12;
      COMMIT;
    )sql");
    version = 12;
    spdlog::info("database migrated to schema v{}", version);
  }
}

std::optional<AccountRecord> SqliteRepository::find_account(std::string_view username) {
  Stmt s(db_, "SELECT id, username, password_hash, is_admin, banned_until, ban_reason, muted_until FROM accounts WHERE username = ?1;");
  s.bind(1, username);
  if (s.step() != SQLITE_ROW) return std::nullopt;
  return AccountRecord{s.col_i64(0), s.col_text(1), s.col_text(2), s.col_int(3) != 0, s.col_i64(4), s.col_text(5), s.col_i64(6)};
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
             " bind_zone, bind_x, bind_y, bind_z, bind_yaw, coin, gender)"
             " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19);");
    ins.bind(1, rec.account_id).bind(2, rec.name).bind(3, rec.race).bind(4, rec.class_id).bind(5, rec.city);
    ins.bind(6, rec.level).bind(7, rec.zone).bind(8, rec.x).bind(9, rec.y).bind(10, rec.z).bind(11, rec.yaw);
    ins.bind(12, unix_now());
    ins.bind(13, rec.bind_zone).bind(14, rec.bind_x).bind(15, rec.bind_y).bind(16, rec.bind_z).bind(17, rec.bind_yaw);
    ins.bind(18, to_i64(rec.coin)).bind(19, rec.gender);
    const int rc = ins.step();
    if (rc == SQLITE_CONSTRAINT) {
      exec("ROLLBACK;");
      return DbStatus::Duplicate;
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(ins.error());
    rec.id = sqlite3_last_insert_rowid(db_);
    // The starting kit goes in with the character.
    Stmt kit(db_, "INSERT INTO character_items (char_id, slot, item_id, count, seconds_left) VALUES (?1, ?2, ?3, ?4, 0);");
    for (const auto& it : rec.items) {
      kit.bind(1, rec.id).bind(2, it.slot).bind(3, it.item_id).bind(4, it.count);
      if (kit.step() != SQLITE_DONE) throw std::runtime_error(kit.error());
      kit.reset();
    }
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
  Stmt items(db_, "SELECT slot, item_id, count, seconds_left FROM character_items WHERE char_id = ?1 ORDER BY slot;");
  items.bind(1, char_id);
  int rc = SQLITE_ROW;
  while ((rc = items.step()) == SQLITE_ROW)
    rec.items.push_back({items.col_int(0), items.col_text(1), items.col_int(2), items.col_float(3)});
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
         " skills = ?17, last_saved = ?18, spellbook = ?20, gems = ?21, buffs = ?22, timers = ?23, bank_coin = ?24,"
         " factions = ?25, quests = ?26, social = ?27, lockouts = ?28, books = ?29 WHERE id = ?19;");
  s.bind(1, c.level).bind(2, c.zone).bind(3, c.x).bind(4, c.y).bind(5, c.z).bind(6, c.yaw);
  s.bind(7, to_i64(c.xp)).bind(8, c.hp).bind(9, c.mana).bind(10, c.endurance).bind(11, to_i64(c.coin));
  s.bind(12, c.bind_zone).bind(13, c.bind_x).bind(14, c.bind_y).bind(15, c.bind_z).bind(16, c.bind_yaw);
  s.bind(17, skills_json(c.skills)).bind(18, unix_now()).bind(19, c.id);
  s.bind(20, strings_json(c.spellbook)).bind(21, strings_json(c.gems)).bind(22, buffs_json(c.buffs)).bind(23, timers_json(c.timers));
  s.bind(24, to_i64(c.bank_coin));
  s.bind(25, skills_json(c.factions)).bind(26, c.quests.empty() ? std::string("{}") : c.quests);
  s.bind(27, c.social.empty() ? std::string("{}") : c.social);
  s.bind(28, c.lockouts.empty() ? std::string("{}") : c.lockouts);
  s.bind(29, strings_json(c.books));
  if (s.step() != SQLITE_DONE) throw std::runtime_error(s.error());
  // A missing row must fail the whole transaction (e.g. a loot transfer would otherwise commit
  // the corpse side alone).
  if (sqlite3_changes(db_) == 0) throw std::runtime_error("character " + std::to_string(c.id) + " does not exist");
  Stmt del(db_, "DELETE FROM character_items WHERE char_id = ?1;");
  del.bind(1, c.id);
  if (del.step() != SQLITE_DONE) throw std::runtime_error(del.error());
  Stmt ins(db_, "INSERT INTO character_items (char_id, slot, item_id, count, seconds_left) VALUES (?1, ?2, ?3, ?4, ?5);");
  for (const auto& it : c.items) {
    ins.bind(1, c.id).bind(2, it.slot).bind(3, it.item_id).bind(4, it.count).bind(5, std::isfinite(it.seconds_left) ? it.seconds_left : 0.0f);
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

std::optional<std::vector<AuctionRecord>> SqliteRepository::load_auctions() {
  std::vector<AuctionRecord> out;
  Stmt s(db_, "SELECT id, seller_char, seller_name, item_id, count, price, listed_at, expires_at, state, buyer FROM auctions ORDER BY id;");
  int rc = SQLITE_ROW;
  while ((rc = s.step()) == SQLITE_ROW) {
    AuctionRecord a;
    a.id = s.col_i64(0);
    a.seller_char = s.col_i64(1);
    a.seller_name = s.col_text(2);
    a.item_id = s.col_text(3);
    a.count = std::max(1, s.col_int(4));
    a.price = to_u64(s.col_i64(5));
    a.listed_at = s.col_i64(6);
    a.expires_at = s.col_i64(7);
    a.state = std::clamp(s.col_int(8), 0, 2);
    a.buyer = s.col_text(9);
    out.push_back(std::move(a));
  }
  if (rc != SQLITE_DONE) {
    spdlog::error("load_auctions: query failed after {} listing(s): {}", out.size(), s.error());
    return std::nullopt;
  }
  return out;
}

DbStatus SqliteRepository::apply_auction(const std::vector<AuctionRecord>& upserts, const std::vector<std::int64_t>& deletes,
                                         const std::vector<CharacterRecord>& chars, bool missing_ok) {
  try {
    Tx tx(db_);
    Stmt up(db_,
            "INSERT OR REPLACE INTO auctions (id, seller_char, seller_name, item_id, count, price, listed_at, expires_at, state, buyer)"
            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10);");
    for (const auto& a : upserts) {
      up.bind(1, a.id).bind(2, a.seller_char).bind(3, a.seller_name).bind(4, a.item_id).bind(5, a.count).bind(6, to_i64(a.price));
      up.bind(7, a.listed_at).bind(8, a.expires_at).bind(9, a.state).bind(10, a.buyer);
      if (up.step() != SQLITE_DONE) throw std::runtime_error(up.error());
      up.reset();
    }
    Stmt del(db_, "DELETE FROM auctions WHERE id = ?1;");
    for (std::int64_t id : deletes) {
      del.bind(1, id);
      if (del.step() != SQLITE_DONE) throw std::runtime_error(del.error());
      // A listing that is already gone means the in-memory board and the table disagree: refuse the
      // whole change rather than hand out an item twice.
      if (sqlite3_changes(db_) == 0 && !missing_ok) throw std::runtime_error("auction " + std::to_string(id) + " does not exist");
      del.reset();
    }
    for (const auto& c : chars) save_one(c);
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("apply_auction failed: {}", e.what());
    return DbStatus::Error;
  }
}

// ---------------------------------------------------------------- moderation (Phase 7)

namespace {

DbStatus changed_one(sqlite3* db, int rc, const std::string& error, const char* what) {
  if (rc != SQLITE_DONE) {
    spdlog::error("{} failed: {}", what, error);
    return DbStatus::Error;
  }
  return sqlite3_changes(db) > 0 ? DbStatus::Ok : DbStatus::NotFound;
}

}  // namespace

std::optional<AccountRecord> SqliteRepository::find_account_by_character(std::string_view char_name) {
  Stmt s(db_,
         "SELECT a.id, a.username, a.password_hash, a.is_admin, a.banned_until, a.ban_reason, a.muted_until FROM accounts a"
         " JOIN characters c ON c.account_id = a.id WHERE c.name = ?1 COLLATE NOCASE;");
  s.bind(1, char_name);
  if (s.step() != SQLITE_ROW) return std::nullopt;
  return AccountRecord{s.col_i64(0), s.col_text(1), s.col_text(2), s.col_int(3) != 0, s.col_i64(4), s.col_text(5), s.col_i64(6)};
}

DbStatus SqliteRepository::set_account_ban(std::int64_t account_id, std::int64_t until, std::string_view reason) {
  Stmt s(db_, "UPDATE accounts SET banned_until = ?1, ban_reason = ?2 WHERE id = ?3;");
  s.bind(1, until).bind(2, reason).bind(3, account_id);
  const int rc = s.step();
  return changed_one(db_, rc, s.error(), "set_account_ban");
}

DbStatus SqliteRepository::set_account_mute(std::int64_t account_id, std::int64_t until) {
  Stmt s(db_, "UPDATE accounts SET muted_until = ?1 WHERE id = ?2;");
  s.bind(1, until).bind(2, account_id);
  const int rc = s.step();
  return changed_one(db_, rc, s.error(), "set_account_mute");
}

DbStatus SqliteRepository::set_account_admin(std::string_view username, bool admin) {
  Stmt s(db_, "UPDATE accounts SET is_admin = ?1 WHERE username = ?2;");
  s.bind(1, admin ? 1 : 0).bind(2, username);
  const int rc = s.step();
  return changed_one(db_, rc, s.error(), "set_account_admin");
}

DbStatus SqliteRepository::append_audit(const AuditRecord& e) {
  Stmt s(db_, "INSERT INTO audit_log (at, account, character, command, target, detail) VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
  s.bind(1, e.at).bind(2, e.account).bind(3, e.character).bind(4, e.command).bind(5, e.target).bind(6, e.detail);
  if (s.step() != SQLITE_DONE) {
    spdlog::error("append_audit failed: {}", s.error());
    return DbStatus::Error;
  }
  return DbStatus::Ok;
}

std::vector<AuditRecord> SqliteRepository::recent_audit(int limit) {
  std::vector<AuditRecord> out;
  Stmt s(db_, "SELECT at, account, character, command, target, detail FROM audit_log ORDER BY id DESC LIMIT ?1;");
  s.bind(1, std::clamp(limit, 1, 1000));
  int rc = SQLITE_ROW;
  while ((rc = s.step()) == SQLITE_ROW) out.push_back({s.col_i64(0), s.col_text(1), s.col_text(2), s.col_text(3), s.col_text(4), s.col_text(5)});
  if (rc != SQLITE_DONE) spdlog::error("recent_audit failed: {}", s.error());
  return out;
}

// ---------------------------------------------------------------- guilds (Phase 7)

std::optional<std::vector<GuildRecord>> SqliteRepository::load_guilds() {
  std::vector<GuildRecord> out;
  {
    Stmt g(db_, "SELECT id, name, motd FROM guilds ORDER BY id;");
    int rc = SQLITE_ROW;
    while ((rc = g.step()) == SQLITE_ROW) out.push_back({g.col_i64(0), g.col_text(1), g.col_text(2), {}});
    if (rc != SQLITE_DONE) {
      spdlog::error("load_guilds: reading guilds failed: {}", g.error());
      return std::nullopt;
    }
  }
  Stmt m(db_,
         "SELECT gm.guild_id, gm.char_id, c.name, gm.rank, c.level, c.class_id FROM guild_members gm"
         " JOIN characters c ON c.id = gm.char_id ORDER BY gm.guild_id, gm.rank, c.name;");
  int rc = SQLITE_ROW;
  while ((rc = m.step()) == SQLITE_ROW) {
    const std::int64_t gid = m.col_i64(0);
    auto it = std::find_if(out.begin(), out.end(), [&](const GuildRecord& r) { return r.id == gid; });
    if (it == out.end()) continue;
    it->members.push_back({m.col_i64(1), m.col_text(2), std::max(0, m.col_int(3)), std::max(1, m.col_int(4)), m.col_text(5)});
  }
  if (rc != SQLITE_DONE) {
    spdlog::error("load_guilds: reading rosters failed: {}", m.error());
    return std::nullopt;
  }
  return out;
}

DbStatus SqliteRepository::create_guild(std::string_view name, std::int64_t leader_char, std::int64_t& out_id) {
  try {
    Tx tx(db_);
    Stmt g(db_, "INSERT INTO guilds (name, motd, created_at) VALUES (?1, '', ?2);");
    g.bind(1, name).bind(2, unix_now());
    const int rc = g.step();
    if (rc == SQLITE_CONSTRAINT) return DbStatus::Duplicate;
    if (rc != SQLITE_DONE) throw std::runtime_error(g.error());
    const std::int64_t id = sqlite3_last_insert_rowid(db_);
    Stmt m(db_, "INSERT INTO guild_members (char_id, guild_id, rank) VALUES (?1, ?2, 0);");
    m.bind(1, leader_char).bind(2, id);
    if (m.step() != SQLITE_DONE) throw std::runtime_error(m.error());
    tx.commit();
    out_id = id;
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("create_guild '{}' failed: {}", name, e.what());
    return DbStatus::Error;
  }
}

std::optional<std::vector<GroupRecord>> SqliteRepository::load_groups() {
  std::vector<GroupRecord> out;
  {
    Stmt g(db_, "SELECT id, leader, loot_rule FROM groups ORDER BY id;");
    int rc = SQLITE_ROW;
    while ((rc = g.step()) == SQLITE_ROW) out.push_back({g.col_i64(0), g.col_i64(1), g.col_int(2), {}});
    if (rc != SQLITE_DONE) {
      spdlog::error("load_groups: reading groups failed: {}", g.error());
      return std::nullopt;
    }
  }
  Stmt m(db_,
         "SELECT gm.group_id, gm.char_id, c.name, c.level, c.class_id FROM group_members gm"
         " JOIN characters c ON c.id = gm.char_id ORDER BY gm.group_id, gm.slot;");
  int rc = SQLITE_ROW;
  while ((rc = m.step()) == SQLITE_ROW) {
    const std::int64_t gid = m.col_i64(0);
    auto it = std::find_if(out.begin(), out.end(), [&](const GroupRecord& r) { return r.id == gid; });
    if (it == out.end()) continue;
    it->members.push_back({m.col_i64(1), m.col_text(2), std::max(1, m.col_int(3)), m.col_text(4)});
  }
  if (rc != SQLITE_DONE) {
    spdlog::error("load_groups: reading members failed: {}", m.error());
    return std::nullopt;
  }
  return out;
}

DbStatus SqliteRepository::save_group(const GroupRecord& group) {
  try {
    Tx tx(db_);
    Stmt g(db_, "INSERT INTO groups (id, leader, loot_rule) VALUES (?1, ?2, ?3)"
                " ON CONFLICT(id) DO UPDATE SET leader = excluded.leader, loot_rule = excluded.loot_rule;");
    g.bind(1, group.id).bind(2, group.leader).bind(3, group.loot_rule);
    if (g.step() != SQLITE_DONE) throw std::runtime_error(g.error());
    Stmt clear(db_, "DELETE FROM group_members WHERE group_id = ?1;");
    clear.bind(1, group.id);
    if (clear.step() != SQLITE_DONE) throw std::runtime_error(clear.error());
    for (std::size_t i = 0; i < group.members.size(); ++i) {
      // A member's row in another group (one that disbanded while a write was pending) is replaced.
      Stmt mm(db_, "INSERT OR REPLACE INTO group_members (char_id, group_id, slot) VALUES (?1, ?2, ?3);");
      mm.bind(1, group.members[i].char_id).bind(2, group.id).bind(3, static_cast<std::int64_t>(i));
      const int rc = mm.step();
      if (rc == SQLITE_CONSTRAINT) continue;  // a deleted character: nothing to keep
      if (rc != SQLITE_DONE) throw std::runtime_error(mm.error());
    }
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("save_group {} failed: {}", group.id, e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::delete_group(std::int64_t group_id) {
  try {
    Tx tx(db_);
    Stmt m(db_, "DELETE FROM group_members WHERE group_id = ?1;");
    m.bind(1, group_id);
    if (m.step() != SQLITE_DONE) throw std::runtime_error(m.error());
    Stmt g(db_, "DELETE FROM groups WHERE id = ?1;");
    g.bind(1, group_id);
    if (g.step() != SQLITE_DONE) throw std::runtime_error(g.error());
    const bool found = sqlite3_changes(db_) > 0;
    tx.commit();
    return found ? DbStatus::Ok : DbStatus::NotFound;
  } catch (const std::exception& e) {
    spdlog::error("delete_group {} failed: {}", group_id, e.what());
    return DbStatus::Error;
  }
}

std::optional<std::vector<RaidRecord>> SqliteRepository::load_raids() {
  std::vector<RaidRecord> out;
  {
    Stmt r(db_, "SELECT id, leader, loot_rule, main_assist FROM raids ORDER BY id;");
    int rc = SQLITE_ROW;
    while ((rc = r.step()) == SQLITE_ROW) out.push_back({r.col_i64(0), r.col_i64(1), r.col_int(2), {}, r.col_i64(3)});
    if (rc != SQLITE_DONE) {
      spdlog::error("load_raids: reading raids failed: {}", r.error());
      return std::nullopt;
    }
  }
  Stmt g(db_, "SELECT raid_id, group_id FROM raid_groups ORDER BY raid_id, slot;");
  int rc = SQLITE_ROW;
  while ((rc = g.step()) == SQLITE_ROW) {
    const std::int64_t rid = g.col_i64(0);
    auto it = std::find_if(out.begin(), out.end(), [&](const RaidRecord& r) { return r.id == rid; });
    if (it != out.end()) it->groups.push_back(g.col_i64(1));
  }
  if (rc != SQLITE_DONE) {
    spdlog::error("load_raids: reading groups failed: {}", g.error());
    return std::nullopt;
  }
  return out;
}

DbStatus SqliteRepository::save_raid(const RaidRecord& raid) {
  try {
    Tx tx(db_);
    Stmt r(db_, "INSERT INTO raids (id, leader, loot_rule, main_assist) VALUES (?1, ?2, ?3, ?4)"
                " ON CONFLICT(id) DO UPDATE SET leader = excluded.leader, loot_rule = excluded.loot_rule,"
                " main_assist = excluded.main_assist;");
    r.bind(1, raid.id).bind(2, raid.leader).bind(3, raid.loot_rule).bind(4, raid.main_assist);
    if (r.step() != SQLITE_DONE) throw std::runtime_error(r.error());
    Stmt clear(db_, "DELETE FROM raid_groups WHERE raid_id = ?1;");
    clear.bind(1, raid.id);
    if (clear.step() != SQLITE_DONE) throw std::runtime_error(clear.error());
    for (std::size_t i = 0; i < raid.groups.size(); ++i) {
      // A group's row in another raid (one that dissolved while a write was pending) is replaced.
      Stmt g(db_, "INSERT OR REPLACE INTO raid_groups (group_id, raid_id, slot) VALUES (?1, ?2, ?3);");
      g.bind(1, raid.groups[i]).bind(2, raid.id).bind(3, static_cast<std::int64_t>(i));
      if (g.step() != SQLITE_DONE) throw std::runtime_error(g.error());
    }
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("save_raid {} failed: {}", raid.id, e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::delete_raid(std::int64_t raid_id) {
  try {
    Tx tx(db_);
    Stmt g(db_, "DELETE FROM raid_groups WHERE raid_id = ?1;");
    g.bind(1, raid_id);
    if (g.step() != SQLITE_DONE) throw std::runtime_error(g.error());
    Stmt r(db_, "DELETE FROM raids WHERE id = ?1;");
    r.bind(1, raid_id);
    if (r.step() != SQLITE_DONE) throw std::runtime_error(r.error());
    const bool found = sqlite3_changes(db_) > 0;
    tx.commit();
    return found ? DbStatus::Ok : DbStatus::NotFound;
  } catch (const std::exception& e) {
    spdlog::error("delete_raid {} failed: {}", raid_id, e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::delete_guild(std::int64_t guild_id) {
  try {
    Tx tx(db_);
    Stmt m(db_, "DELETE FROM guild_members WHERE guild_id = ?1;");
    m.bind(1, guild_id);
    if (m.step() != SQLITE_DONE) throw std::runtime_error(m.error());
    Stmt g(db_, "DELETE FROM guilds WHERE id = ?1;");
    g.bind(1, guild_id);
    if (g.step() != SQLITE_DONE) throw std::runtime_error(g.error());
    const bool found = sqlite3_changes(db_) > 0;
    tx.commit();
    return found ? DbStatus::Ok : DbStatus::NotFound;
  } catch (const std::exception& e) {
    spdlog::error("delete_guild {} failed: {}", guild_id, e.what());
    return DbStatus::Error;
  }
}

DbStatus SqliteRepository::set_guild_motd(std::int64_t guild_id, std::string_view motd) {
  Stmt s(db_, "UPDATE guilds SET motd = ?1 WHERE id = ?2;");
  s.bind(1, motd).bind(2, guild_id);
  const int rc = s.step();
  return changed_one(db_, rc, s.error(), "set_guild_motd");
}

DbStatus SqliteRepository::set_guild_member(std::int64_t guild_id, std::int64_t char_id, int rank) {
  Stmt s(db_,
         "INSERT INTO guild_members (char_id, guild_id, rank) VALUES (?1, ?2, ?3)"
         " ON CONFLICT(char_id) DO UPDATE SET rank = excluded.rank WHERE guild_members.guild_id = excluded.guild_id;");
  s.bind(1, char_id).bind(2, guild_id).bind(3, rank);
  const int rc = s.step();
  if (rc == SQLITE_CONSTRAINT) return DbStatus::NotFound;  // no such guild or character
  if (rc != SQLITE_DONE) {
    spdlog::error("set_guild_member failed: {}", s.error());
    return DbStatus::Error;
  }
  return sqlite3_changes(db_) > 0 ? DbStatus::Ok : DbStatus::Duplicate;  // they are in another guild
}

DbStatus SqliteRepository::remove_guild_member(std::int64_t char_id) {
  Stmt s(db_, "DELETE FROM guild_members WHERE char_id = ?1;");
  s.bind(1, char_id);
  const int rc = s.step();
  return changed_one(db_, rc, s.error(), "remove_guild_member");
}

DbStatus SqliteRepository::set_guild_ranks(std::int64_t guild_id, const std::vector<std::pair<std::int64_t, int>>& ranks) {
  try {
    Tx tx(db_);
    Stmt u(db_, "UPDATE guild_members SET rank = ?1 WHERE char_id = ?2 AND guild_id = ?3;");
    for (const auto& [char_id, rank] : ranks) {
      u.bind(1, rank).bind(2, char_id).bind(3, guild_id);
      if (u.step() != SQLITE_DONE) throw std::runtime_error(u.error());
      if (sqlite3_changes(db_) == 0) return DbStatus::NotFound;  // rolled back: not all of them are members
      u.reset();
    }
    tx.commit();
    return DbStatus::Ok;
  } catch (const std::exception& e) {
    spdlog::error("set_guild_ranks for guild {} failed: {}", guild_id, e.what());
    return DbStatus::Error;
  }
}

}  // namespace ef::db
