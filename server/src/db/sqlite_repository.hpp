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
  DbStatus save_characters(const std::vector<CharacterRecord>& chars) override;
  DbStatus create_corpse(const CorpseRecord& corpse, const CharacterRecord& owner_after) override;
  DbStatus update_corpse(const CorpseRecord& corpse, const CharacterRecord& looter) override;
  DbStatus delete_corpse(std::int64_t corpse_id) override;
  std::vector<CorpseRecord> load_corpses(std::int64_t now_unix) override;
  std::optional<std::vector<AuctionRecord>> load_auctions() override;
  DbStatus apply_auction(const std::vector<AuctionRecord>& upserts, const std::vector<std::int64_t>& deletes,
                         const std::vector<CharacterRecord>& chars, bool missing_ok) override;
  std::optional<AccountRecord> find_account_by_character(std::string_view char_name) override;
  DbStatus set_account_ban(std::int64_t account_id, std::int64_t until, std::string_view reason) override;
  DbStatus set_account_mute(std::int64_t account_id, std::int64_t until) override;
  DbStatus set_account_admin(std::string_view username, bool admin) override;
  DbStatus append_audit(const AuditRecord& entry) override;
  std::vector<AuditRecord> recent_audit(int limit) override;
  std::optional<std::vector<GuildRecord>> load_guilds() override;
  DbStatus create_guild(std::string_view name, std::int64_t leader_char, std::int64_t& out_id) override;
  DbStatus delete_guild(std::int64_t guild_id) override;
  DbStatus set_guild_motd(std::int64_t guild_id, std::string_view motd) override;
  DbStatus set_guild_member(std::int64_t guild_id, std::int64_t char_id, int rank) override;
  DbStatus remove_guild_member(std::int64_t char_id) override;
  DbStatus set_guild_ranks(std::int64_t guild_id, const std::vector<std::pair<std::int64_t, int>>& ranks) override;

 private:
  void exec(const char* sql);
  void migrate();
  void save_one(const CharacterRecord& c);  // inside a transaction; throws on failure
  void write_corpse(const CorpseRecord& c);  // insert or replace; throws on failure
  sqlite3* db_ = nullptr;
};

}  // namespace ef::db
