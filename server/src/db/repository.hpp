// Persistence interface. The SQLite implementation lives in sqlite_repository.*; a Postgres
// implementation can be swapped in later without touching game code.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ef::db {

struct AccountRecord {
  std::int64_t id = 0;
  std::string username;
  std::string password_hash;
  bool is_admin = false;
};

struct StoredItem {
  int slot = 0;
  std::string item_id;
  int count = 0;
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
  // Phase 2 state. hp/mana/endurance of -1 mean "full".
  std::uint64_t xp = 0;
  int hp = -1;
  int mana = -1;
  int endurance = -1;
  std::uint64_t coin = 0;
  std::string bind_zone;  // empty = the starting city's spawn
  float bind_x = 0.0f;
  float bind_y = 0.0f;
  float bind_z = 0.0f;
  float bind_yaw = 0.0f;
  std::vector<std::pair<std::string, int>> skills;
  std::vector<StoredItem> items;  // only loaded/saved by load_character / save_characters
};

struct CorpseRecord {
  std::int64_t id = 0;  // chosen by the game server (random); unique across the table
  std::int64_t char_id = 0;
  std::string owner_name;
  int level = 1;
  std::string zone;
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float yaw = 0.0f;
  std::uint64_t coin = 0;
  std::uint64_t xp_lost = 0;
  std::int64_t expires_at = 0;  // unix seconds
  std::vector<StoredItem> items;
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

  // Summaries for character select (no inventory).
  virtual std::vector<CharacterRecord> list_characters(std::int64_t account_id) = 0;
  // Atomically enforces max_per_account and name uniqueness. On success sets rec.id.
  virtual DbStatus create_character(CharacterRecord& rec, int max_per_account) = 0;
  virtual DbStatus delete_character(std::int64_t account_id, std::int64_t char_id) = 0;
  // Full record including inventory.
  virtual std::optional<CharacterRecord> load_character(std::int64_t account_id, std::int64_t char_id) = 0;
  // Saves position, progress, vitals, coin, skills, and inventory for each character in one transaction.
  virtual DbStatus save_characters(const std::vector<CharacterRecord>& chars) = 0;

  // Death: stores the corpse and the stripped character together, so a crash cannot duplicate
  // or lose what moved onto the corpse.
  virtual DbStatus create_corpse(const CorpseRecord& corpse, const CharacterRecord& owner_after) = 0;
  // Looting: the corpse's remaining contents and the looter's new state, in one transaction.
  virtual DbStatus update_corpse(const CorpseRecord& corpse, const CharacterRecord& looter) = 0;
  virtual DbStatus delete_corpse(std::int64_t corpse_id) = 0;
  // Deletes corpses that expired before `now_unix`, then returns the rest.
  virtual std::vector<CorpseRecord> load_corpses(std::int64_t now_unix) = 0;
};

}  // namespace ef::db
