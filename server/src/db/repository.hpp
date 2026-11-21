// Persistence interface. The SQLite implementation lives in sqlite_repository.*; a Postgres
// implementation can be swapped in later without touching game code.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ef::db {

struct AccountRecord {
  std::int64_t id = 0;
  std::string username;
  std::string password_hash;
  bool is_admin = false;
};

struct CharacterRecord {
  std::int64_t id = 0;
  std::int64_t account_id = 0;
  std::string name;
  std::string race;
  std::string class_id;
  std::string city;
  int level = 1;
  std::string zone;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
};

struct CharacterPosition {
  std::int64_t char_id = 0;
  std::string zone;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
};

enum class DbStatus { Ok, Duplicate, NotFound, LimitReached, Error };

class Repository {
 public:
  virtual ~Repository() = default;
  Repository() = default;
  Repository(const Repository&) = delete;
  Repository& operator=(const Repository&) = delete;

  virtual std::optional<AccountRecord> find_account(std::string_view username) = 0;
  // On success sets out_id.
  virtual DbStatus create_account(std::string_view username, std::string_view password_hash, std::int64_t& out_id) = 0;

  virtual std::vector<CharacterRecord> list_characters(std::int64_t account_id) = 0;
  // Atomically enforces max_per_account and name uniqueness. On success sets rec.id.
  virtual DbStatus create_character(CharacterRecord& rec, int max_per_account) = 0;
  virtual DbStatus delete_character(std::int64_t account_id, std::int64_t char_id) = 0;
  virtual std::optional<CharacterRecord> load_character(std::int64_t account_id, std::int64_t char_id) = 0;
  // Saves all positions in one transaction.
  virtual DbStatus save_positions(const std::vector<CharacterPosition>& positions) = 0;
};

}  // namespace ef::db
