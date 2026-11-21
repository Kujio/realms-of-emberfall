#pragma once

#include <memory>
#include <string>

#include "db/repository.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace ef::db {

// SQLite (WAL) implementation. Not thread-safe: use from the DB worker thread only.
class SqliteRepository final : public Repository {
 public:
  // path ":memory:" opens a private in-memory database (tests). Throws std::runtime_error on failure.
  explicit SqliteRepository(const std::string& path);
  ~SqliteRepository() override;

  std::optional<AccountRecord> find_account(std::string_view username) override;
  DbStatus create_account(std::string_view username, std::string_view password_hash, std::int64_t& out_id) override;
  std::vector<CharacterRecord> list_characters(std::int64_t account_id) override;
  DbStatus create_character(CharacterRecord& rec, int max_per_account) override;
  DbStatus delete_character(std::int64_t account_id, std::int64_t char_id) override;
  std::optional<CharacterRecord> load_character(std::int64_t account_id, std::int64_t char_id) override;
  DbStatus save_positions(const std::vector<CharacterPosition>& positions) override;

 private:
  void exec(const char* sql);
  void migrate();
  sqlite3* db_ = nullptr;
};

}  // namespace ef::db
