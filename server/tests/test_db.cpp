#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include <sqlite3.h>
#include <unistd.h>

#include "db/db_worker.hpp"
#include "db/sqlite_repository.hpp"

using namespace ef::db;

namespace {
CharacterRecord make_char(std::int64_t account, const std::string& name) {
  CharacterRecord r;
  r.account_id = account;
  r.name = name;
  r.race = "highlander";
  r.class_id = "warrior";
  r.city = "emberhold";
  r.zone = "proving_flats";
  return r;
}
}  // namespace

TEST_CASE("accounts are unique case-insensitively", "[db]") {
  SqliteRepository repo(":memory:");
  std::int64_t id = 0;
  REQUIRE(repo.create_account("alice", "hash", id) == DbStatus::Ok);
  CHECK(id > 0);
  std::int64_t id2 = 0;
  CHECK(repo.create_account("ALICE", "hash", id2) == DbStatus::Duplicate);
  auto a = repo.find_account("Alice");
  REQUIRE(a.has_value());
  CHECK(a->id == id);
  CHECK_FALSE(repo.find_account("bob").has_value());
}

TEST_CASE("character limit, uniqueness, ownership", "[db]") {
  SqliteRepository repo(":memory:");
  std::int64_t a = 0, b = 0;
  REQUIRE(repo.create_account("alice", "h", a) == DbStatus::Ok);
  REQUIRE(repo.create_account("bob", "h", b) == DbStatus::Ok);

  const char* names[] = {"Aone", "Atwo", "Athree", "Afour"};
  for (const char* n : names) {
    auto c = make_char(a, n);
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    CHECK(c.id > 0);
  }
  auto fifth = make_char(a, "Afive");
  CHECK(repo.create_character(fifth, 4) == DbStatus::LimitReached);
  auto dup = make_char(b, "AONE");
  CHECK(repo.create_character(dup, 4) == DbStatus::Duplicate);

  auto list = repo.list_characters(a);
  REQUIRE(list.size() == 4);
  CHECK(repo.list_characters(b).empty());

  // Bob cannot load or delete Alice's character.
  CHECK_FALSE(repo.load_character(b, list[0].id).has_value());
  CHECK(repo.delete_character(b, list[0].id) == DbStatus::NotFound);
  CHECK(repo.delete_character(a, list[0].id) == DbStatus::Ok);
  CHECK(repo.list_characters(a).size() == 3);
}

TEST_CASE("character state persists", "[db]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_state_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  std::int64_t char_id = 0;
  std::int64_t acct = 0;
  {
    SqliteRepository repo(path.string());
    REQUIRE(repo.create_account("carol", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Carol");
    c.bind_zone = "proving_flats";
    c.bind_x = 4.0f;
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    char_id = c.id;
    auto loaded = repo.load_character(acct, char_id);
    REQUIRE(loaded.has_value());
    CHECK(loaded->bind_zone == "proving_flats");
    CHECK(loaded->hp == -1);  // new characters start at full health
    CharacterRecord s = *loaded;
    s.x = 12.5f;
    s.z = -3.25f;
    s.level = 7;
    s.xp = 123456;
    s.hp = 40;
    s.endurance = 12;
    s.coin = 9876;
    s.skills = {{"offense", 31}, {"dodge", 4}};
    s.items = {{0, "rat_whisker", 3}, {5, "wolf_fang", 1}};
    REQUIRE(repo.save_characters({s}) == DbStatus::Ok);
    s.items = {{1, "wolf_fang", 2}};  // a later save replaces the pack, it does not append
    REQUIRE(repo.save_characters({s}) == DbStatus::Ok);
  }
  {
    SqliteRepository repo(path.string());  // reopen: migrations are idempotent
    auto c = repo.load_character(acct, char_id);
    REQUIRE(c.has_value());
    CHECK(c->x == 12.5f);
    CHECK(c->z == -3.25f);
    CHECK(c->level == 7);
    CHECK(c->xp == 123456);
    CHECK(c->hp == 40);
    CHECK(c->endurance == 12);
    CHECK(c->coin == 9876);
    CHECK(c->skills.size() == 2);
    REQUIRE(c->items.size() == 1);
    CHECK(c->items[0].slot == 1);
    CHECK(c->items[0].count == 2);
    CHECK(repo.list_characters(acct).at(0).level == 7);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("corpses persist, update atomically with the looter, and expire", "[db]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("dave", "h", acct) == DbStatus::Ok);
  auto c = make_char(acct, "Dave");
  REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
  auto loaded_dave = repo.load_character(acct, c.id);
  REQUIRE(loaded_dave.has_value());
  auto dave = *loaded_dave;

  CorpseRecord corpse;
  corpse.id = 424242;
  corpse.char_id = c.id;
  corpse.owner_name = "Dave";
  corpse.zone = "proving_flats";
  corpse.x = 10;
  corpse.coin = 55;
  corpse.xp_lost = 70;
  corpse.expires_at = 2000;
  corpse.items = {{0, "rat_whisker", 2}, {1, "wolf_fang", 1}};
  dave.coin = 0;
  dave.items.clear();
  REQUIRE(repo.create_corpse(corpse, dave) == DbStatus::Ok);
  CHECK(repo.create_corpse(corpse, dave) == DbStatus::Ok);  // replace, not duplicate

  auto loaded = repo.load_corpses(1000);
  REQUIRE(loaded.size() == 1);
  CHECK(loaded[0].owner_name == "Dave");
  CHECK(loaded[0].coin == 55);
  CHECK(loaded[0].items.size() == 2);

  // Owner loots the coin and one item.
  corpse.coin = 0;
  corpse.items = {{1, "wolf_fang", 1}};
  dave.coin = 55;
  dave.items = {{0, "rat_whisker", 2}};
  REQUIRE(repo.update_corpse(corpse, dave) == DbStatus::Ok);
  loaded = repo.load_corpses(1000);
  REQUIRE(loaded.size() == 1);
  CHECK(loaded[0].coin == 0);
  CHECK(loaded[0].items.size() == 1);
  auto after = repo.load_character(acct, c.id);
  REQUIRE(after.has_value());
  CHECK(after->coin == 55);
  CHECK(after->items.size() == 1);

  CHECK(repo.load_corpses(3000).empty());  // expired ones are deleted on load
  CHECK(repo.delete_corpse(424242) == DbStatus::NotFound);

  corpse.expires_at = 9000;
  REQUIRE(repo.create_corpse(corpse, dave) == DbStatus::Ok);
  CHECK(repo.delete_corpse(424242) == DbStatus::Ok);
  // Deleting the character removes its corpses too.
  REQUIRE(repo.create_corpse(corpse, dave) == DbStatus::Ok);
  REQUIRE(repo.delete_character(acct, c.id) == DbStatus::Ok);
  CHECK(repo.load_corpses(1000).empty());
}

TEST_CASE("a Phase 1 (schema v1) database upgrades in place", "[db]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v1_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    const char* v1 = R"sql(
      CREATE TABLE accounts (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL UNIQUE COLLATE NOCASE,
        password_hash TEXT NOT NULL, is_admin INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL);
      CREATE TABLE characters (id INTEGER PRIMARY KEY AUTOINCREMENT, account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
        name TEXT NOT NULL UNIQUE COLLATE NOCASE, race TEXT NOT NULL, class_id TEXT NOT NULL, city TEXT NOT NULL,
        level INTEGER NOT NULL DEFAULT 1, zone TEXT NOT NULL, x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL, yaw REAL NOT NULL,
        created_at INTEGER NOT NULL, last_saved INTEGER NOT NULL);
      CREATE INDEX characters_account ON characters(account_id);
      INSERT INTO accounts (username, password_hash, created_at) VALUES ('old', 'h', 0);
      INSERT INTO characters (account_id, name, race, class_id, city, level, zone, x, y, z, yaw, created_at, last_saved)
        VALUES (1, 'Oldtimer', 'highlander', 'warrior', 'emberhold', 1, 'proving_flats', 3, 0, 4, 0, 0, 0);
      PRAGMA user_version = 1;
    )sql";
    REQUIRE(sqlite3_exec(raw, v1, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto c = repo.load_character(1, 1);
    REQUIRE(c.has_value());
    CHECK(c->name == "Oldtimer");
    CHECK(c->x == 3.0f);
    CHECK(c->xp == 0);
    CHECK(c->hp == -1);
    CHECK(c->bind_zone.empty());  // the server falls back to the starting city
    CHECK(repo.load_corpses(0).empty());
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("threaded worker runs jobs off-thread and completions on drain", "[db]") {
  DbWorker w(std::make_unique<SqliteRepository>(":memory:"), DbWorker::Mode::Threaded);
  int done = 0;
  for (int i = 0; i < 20; ++i) {
    w.post([&done, i](Repository& repo, DbWorker& self) {
      std::int64_t id = 0;
      repo.create_account("user" + std::to_string(i), "h", id);
      self.complete([&done] { ++done; });
    });
  }
  w.stop_and_flush();
  w.drain();
  CHECK(done == 20);
}

TEST_CASE("saving a character that no longer exists fails the whole transfer", "[db]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("erin", "h", acct) == DbStatus::Ok);
  CharacterRecord ghost = make_char(acct, "Ghost");
  ghost.id = 9999;  // never created
  CorpseRecord corpse;
  corpse.id = 5;
  corpse.char_id = 9999;
  corpse.owner_name = "Ghost";
  corpse.zone = "proving_flats";
  corpse.expires_at = 100;
  CHECK(repo.save_characters({ghost}) == DbStatus::Error);
  CHECK(repo.update_corpse(corpse, ghost) == DbStatus::Error);
  CHECK(repo.load_corpses(0).empty());  // rolled back
}

TEST_CASE("spellbook, gems, buffs, and recast timers persist (schema v3)", "[db][spells]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("wiz", "h", acct) == DbStatus::Ok);
  auto c = make_char(acct, "Wizzy");
  REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
  auto fresh = repo.load_character(acct, c.id);
  REQUIRE(fresh.has_value());
  CHECK(fresh->spellbook.empty());
  CHECK(fresh->gems.empty());
  CHECK(fresh->buffs.empty());
  CHECK(fresh->timers.empty());
  CharacterRecord s = *fresh;
  s.spellbook = {"ember_dart", "ward_of_glass", "frost_needle"};
  s.gems = {"ember_dart", "", "frost_needle"};
  s.buffs = {{"ward_of_glass", 12, "Wizzy", 1234.5f}};
  s.timers = {{"gather_aether", 600.0f}};
  REQUIRE(repo.save_characters({s}) == DbStatus::Ok);
  auto back = repo.load_character(acct, c.id);
  REQUIRE(back.has_value());
  CHECK(back->spellbook == s.spellbook);
  CHECK(back->gems == s.gems);
  REQUIRE(back->buffs.size() == 1);
  CHECK(back->buffs[0].ability_id == "ward_of_glass");
  CHECK(back->buffs[0].caster_level == 12);
  CHECK(back->buffs[0].caster_name == "Wizzy");
  CHECK(back->buffs[0].seconds_left == 1234.5f);
  REQUIRE(back->timers.size() == 1);
  CHECK(back->timers[0].first == "gather_aether");
  CHECK(back->timers[0].second == 600.0f);
}

TEST_CASE("a Phase 2 (schema v2) database upgrades to v3 in place", "[db][spells]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v2_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  std::int64_t acct = 0;
  std::int64_t id = 0;
  {
    SqliteRepository repo(path.string());  // creates v3; roll the new columns back to look like v2
    REQUIRE(repo.create_account("veteran", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Veteran");
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    id = c.id;
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    const char* to_v2 = R"sql(
      ALTER TABLE characters DROP COLUMN spellbook;
      ALTER TABLE characters DROP COLUMN gems;
      ALTER TABLE characters DROP COLUMN buffs;
      ALTER TABLE characters DROP COLUMN timers;
      UPDATE characters SET xp = 777, coin = 55;
      PRAGMA user_version = 2;
    )sql";
    REQUIRE(sqlite3_exec(raw, to_v2, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto c = repo.load_character(acct, id);
    REQUIRE(c.has_value());
    CHECK(c->xp == 777);
    CHECK(c->coin == 55);
    CHECK(c->spellbook.empty());  // the server grants starter spells to casters with an empty book
    CHECK(c->gems.empty());
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}
