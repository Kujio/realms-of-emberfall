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
  // Phase 7 (schema v8): unix seconds; 0 = none, -1 = forever (auth/bans.hpp).
  std::int64_t banned_until = 0;
  std::string ban_reason;
  std::int64_t muted_until = 0;
};

struct StoredItem {
  int slot = 0;  // slot code (game/items.hpp): (container << 8) | index; corpses: where it was worn or packed
  std::string item_id;
  int count = 0;
  float seconds_left = 0.0f;  // summoned items: time until they fade (0 = never)
};

// A lasting beneficial effect or a recast timer, with the time it had left at save.
struct StoredBuff {
  std::string ability_id;
  int caster_level = 1;
  std::string caster_name;
  float seconds_left = 0.0f;
  float next_tick_seconds = 0.0f;  // periodic effects: time until the next tick
};

struct CharacterRecord {
  std::int64_t id = 0;
  std::int64_t account_id = 0;
  std::string name;
  std::string race;
  std::string class_id;
  std::string city;
  int gender = 0;  // 0 = female, 1 = male (fixed at creation)
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
  std::uint64_t bank_coin = 0;  // Phase 5
  std::string bind_zone;  // empty = the starting city's spawn
  float bind_x = 0.0f;
  float bind_y = 0.0f;
  float bind_z = 0.0f;
  float bind_yaw = 0.0f;
  std::vector<std::pair<std::string, int>> skills;
  std::vector<StoredItem> items;  // loaded/saved by load_character / save_characters; create_character stores a new kit
  // Phase 3 state.
  std::vector<std::string> spellbook;
  std::vector<std::string> gems;  // "" = empty gem
  std::vector<StoredBuff> buffs;
  std::vector<std::pair<std::string, float>> timers;  // ability/spell id -> seconds until ready
  // Phase 6 state (schema v7).
  std::vector<std::pair<std::string, int>> factions;  // faction id -> standing (only the ones that moved)
  std::string quests = "{}";                           // quest progress, as the game layer serializes it (JSON)
  // Phase 7 state (schema v8).
  std::string social = "{}";  // friends, ignore list and muted channels, as the game layer serializes it (JSON)
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

// A player auction listing (game/auction.hpp mirrors it in memory).
struct AuctionRecord {
  std::int64_t id = 0;  // chosen by the game server (random); unique across the table
  std::int64_t seller_char = 0;
  std::string seller_name;
  std::string item_id;
  int count = 1;
  std::uint64_t price = 0;
  std::int64_t listed_at = 0;
  std::int64_t expires_at = 0;
  int state = 0;  // 0 open, 1 sold, 2 expired
  std::string buyer;
};

// A guild and its roster (Phase 7). Member name, level and class come from the characters table.
struct GuildMemberRecord {
  std::int64_t char_id = 0;
  std::string name;
  int rank = 0;
  int level = 1;
  std::string class_id;
};
struct GuildRecord {
  std::int64_t id = 0;
  std::string name;
  std::string motd;
  std::vector<GuildMemberRecord> members;
};

// A group (Phase 12): its leader, loot rule and members in joining order. Name, level and class come
// from the characters table.
struct GroupMemberRecord {
  std::int64_t char_id = 0;
  std::string name;
  int level = 1;
  std::string class_id;
};
struct GroupRecord {
  std::int64_t id = 0;
  std::int64_t leader = 0;
  int loot_rule = 0;
  std::vector<GroupMemberRecord> members;
};

// One game master action (Phase 7), kept for review.
struct AuditRecord {
  std::int64_t at = 0;  // unix seconds
  std::string account;
  std::string character;
  std::string command;  // "ban", "setlevel", ...
  std::string target;
  std::string detail;
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

  // Auctions (Phase 5). Listing changes are always written together with the characters whose items or
  // coin moved, in one transaction, so an item is either on the board or in a pack, never both.
  // nullopt when the read did not complete (a partial board must never look like the whole board).
  virtual std::optional<std::vector<AuctionRecord>> load_auctions() = 0;
  // missing_ok: a delete of a listing that is not there is skipped instead of failing the whole change
  // (the game server's retries re-send its current state, which may already be partly stored).
  virtual DbStatus apply_auction(const std::vector<AuctionRecord>& upserts, const std::vector<std::int64_t>& deletes,
                                 const std::vector<CharacterRecord>& chars, bool missing_ok) = 0;

  // Moderation (Phase 7).
  // The account that owns a character (case-insensitive name), for bans and mutes by character name.
  virtual std::optional<AccountRecord> find_account_by_character(std::string_view char_name) = 0;
  virtual DbStatus set_account_ban(std::int64_t account_id, std::int64_t until, std::string_view reason) = 0;
  virtual DbStatus set_account_mute(std::int64_t account_id, std::int64_t until) = 0;
  // The operator's switch for game master rights (emberfall-server --grant-admin / --revoke-admin).
  virtual DbStatus set_account_admin(std::string_view username, bool admin) = 0;
  virtual DbStatus append_audit(const AuditRecord& entry) = 0;
  // Newest first.
  virtual std::vector<AuditRecord> recent_audit(int limit) = 0;

  // Guilds (Phase 7). nullopt when the read did not complete (a partial roster must not look whole).
  virtual std::optional<std::vector<GuildRecord>> load_guilds() = 0;
  // Creates the guild with `leader_char` as its leader (rank 0). Duplicate if the name is taken (any case).
  virtual DbStatus create_guild(std::string_view name, std::int64_t leader_char, std::int64_t& out_id) = 0;
  virtual DbStatus delete_guild(std::int64_t guild_id) = 0;
  virtual DbStatus set_guild_motd(std::int64_t guild_id, std::string_view motd) = 0;
  // Adds the character to the guild, or changes their rank there. Duplicate if they are in another guild.
  virtual DbStatus set_guild_member(std::int64_t guild_id, std::int64_t char_id, int rank) = 0;
  virtual DbStatus remove_guild_member(std::int64_t char_id) = 0;
  // Several rank changes in one guild (a leadership handover is two), all or none.
  virtual DbStatus set_guild_ranks(std::int64_t guild_id, const std::vector<std::pair<std::int64_t, int>>& ranks) = 0;

  // Groups (Phase 12). nullopt when the read did not complete.
  virtual std::optional<std::vector<GroupRecord>> load_groups() = 0;
  // Writes the group as it is now: its row, and exactly these members (taking each out of any other group).
  virtual DbStatus save_group(const GroupRecord& group) = 0;
  virtual DbStatus delete_group(std::int64_t group_id) = 0;
};

}  // namespace ef::db
