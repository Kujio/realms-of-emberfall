#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

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

TEST_CASE("wait_idle returns once every job posted before it has run, with its completions ready", "[db]") {
  DbWorker w(std::make_unique<SqliteRepository>(":memory:"), DbWorker::Mode::Threaded);
  int done = 0;
  for (int i = 0; i < 5; ++i)
    w.post([&done](Repository&, DbWorker& self) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      self.complete([&done] { ++done; });
    });
  w.wait_idle();
  CHECK(w.queued_jobs() == 0);
  CHECK(w.drain() == 5);
  CHECK(done == 5);
  // The worker still runs jobs afterwards; once stopped, waiting returns at once.
  w.post([](Repository&, DbWorker& self) { self.complete([] {}); });
  w.wait_idle();
  CHECK(w.drain() == 1);
  w.stop_and_flush();
  w.wait_idle();
}

TEST_CASE("settle runs jobs that completions post, until nothing more arrives", "[db]") {
  for (const auto mode : {DbWorker::Mode::Threaded, DbWorker::Mode::Inline}) {
    DbWorker w(std::make_unique<SqliteRepository>(":memory:"), mode);
    // A chain like the groups' load starting the raids' load: each completion posts the next job.
    int last = 0;
    std::function<void(int)> step = [&](int n) {
      w.post([&, n](Repository&, DbWorker& self) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        self.complete([&, n] {
          last = n;
          if (n < 4) step(n + 1);
        });
      });
    };
    step(1);
    CHECK(w.settle());
    CHECK(last == 4);
    CHECK(w.queued_jobs() == 0);
    CHECK(w.drain() == 0);
    // A job that posts another job itself (from the worker): settle waits for that one too.
    bool child = false;
    w.post([&](Repository&, DbWorker& self) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      self.post([&](Repository&, DbWorker& again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        again.complete([&] { child = true; });
      });
    });
    CHECK(w.settle());
    CHECK(child);
    // Work that never stops arriving: settle gives up after its rounds.
    std::function<void()> forever = [&] { w.post([&](Repository&, DbWorker& self) { self.complete([&] { forever(); }); }); };
    forever();
    CHECK_FALSE(w.settle(3));
    w.stop_and_flush();
    w.drain();
  }
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
    SqliteRepository repo(path.string());  // creates the latest schema; roll the new columns back to look like v2
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
      ALTER TABLE characters DROP COLUMN gender;
      UPDATE characters SET xp = 777, coin = 55;
      ALTER TABLE characters DROP COLUMN factions;
      ALTER TABLE characters DROP COLUMN quests;
      ALTER TABLE accounts DROP COLUMN banned_until;
      ALTER TABLE accounts DROP COLUMN ban_reason;
      ALTER TABLE accounts DROP COLUMN muted_until;
      ALTER TABLE characters DROP COLUMN social;
      ALTER TABLE characters DROP COLUMN lockouts;
      ALTER TABLE raids DROP COLUMN main_assist;
      ALTER TABLE characters DROP COLUMN books;
      DROP TABLE guild_members;
      DROP TABLE guilds;
      DROP TABLE audit_log;
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

TEST_CASE("gender is stored at creation and read back", "[db][models]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("gen", "h", acct) == DbStatus::Ok);
  auto f = make_char(acct, "Fenna");
  auto m = make_char(acct, "Mordo");
  m.gender = 1;
  REQUIRE(repo.create_character(f, 4) == DbStatus::Ok);
  REQUIRE(repo.create_character(m, 4) == DbStatus::Ok);
  auto list = repo.list_characters(acct);
  REQUIRE(list.size() == 2);
  CHECK(list[0].gender == 0);
  CHECK(list[1].gender == 1);
  auto back = repo.load_character(acct, m.id);
  REQUIRE(back.has_value());
  CHECK(back->gender == 1);
}

TEST_CASE("a Phase 3 (schema v3) database upgrades to v4 in place", "[db][models]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v3_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  std::int64_t acct = 0;
  std::int64_t id = 0;
  {
    SqliteRepository repo(path.string());
    REQUIRE(repo.create_account("elder", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Elder");
    c.gender = 1;
    c.spellbook = {"ember_dart"};
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    id = c.id;
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    const char* to_v3 = R"sql(
      ALTER TABLE characters DROP COLUMN gender;
      UPDATE characters SET xp = 4242, spellbook = '["ember_dart"]';
      ALTER TABLE characters DROP COLUMN factions;
      ALTER TABLE characters DROP COLUMN quests;
      ALTER TABLE accounts DROP COLUMN banned_until;
      ALTER TABLE accounts DROP COLUMN ban_reason;
      ALTER TABLE accounts DROP COLUMN muted_until;
      ALTER TABLE characters DROP COLUMN social;
      ALTER TABLE characters DROP COLUMN lockouts;
      ALTER TABLE raids DROP COLUMN main_assist;
      ALTER TABLE characters DROP COLUMN books;
      DROP TABLE guild_members;
      DROP TABLE guilds;
      DROP TABLE audit_log;
      PRAGMA user_version = 3;
    )sql";
    REQUIRE(sqlite3_exec(raw, to_v3, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto c = repo.load_character(acct, id);
    REQUIRE(c.has_value());
    CHECK(c->xp == 4242);
    CHECK(c->gender == 0);  // characters from before genders existed become female
    REQUIRE(c->spellbook.size() == 1);
    CHECK(c->spellbook[0] == "ember_dart");
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA user_version;", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    CHECK(sqlite3_column_int(st, 0) == 12);  // migrated all the way to the current schema
    sqlite3_finalize(st);
    sqlite3_close(raw);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("a Phase 3.5 (schema v4) database upgrades to v5, and summoned items keep their time", "[db][classes]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v4_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  std::int64_t acct = 0;
  std::int64_t id = 0;
  {
    SqliteRepository repo(path.string());
    REQUIRE(repo.create_account("conjurer", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Conjurer");
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    id = c.id;
    auto loaded = repo.load_character(acct, id);
    REQUIRE(loaded.has_value());
    CharacterRecord s = *loaded;
    s.items = {{0, "rat_whisker", 2, 0.0f}};
    REQUIRE(repo.save_characters({s}) == DbStatus::Ok);
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    const char* to_v4 = R"sql(
      ALTER TABLE character_items DROP COLUMN seconds_left;
      ALTER TABLE characters DROP COLUMN factions;
      ALTER TABLE characters DROP COLUMN quests;
      ALTER TABLE accounts DROP COLUMN banned_until;
      ALTER TABLE accounts DROP COLUMN ban_reason;
      ALTER TABLE accounts DROP COLUMN muted_until;
      ALTER TABLE characters DROP COLUMN social;
      ALTER TABLE characters DROP COLUMN lockouts;
      ALTER TABLE raids DROP COLUMN main_assist;
      ALTER TABLE characters DROP COLUMN books;
      DROP TABLE guild_members;
      DROP TABLE guilds;
      DROP TABLE audit_log;
      PRAGMA user_version = 4;
    )sql";
    REQUIRE(sqlite3_exec(raw, to_v4, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto c = repo.load_character(acct, id);
    REQUIRE(c.has_value());
    REQUIRE(c->items.size() == 1);
    CHECK(c->items[0].item_id == "rat_whisker");
    CHECK(c->items[0].seconds_left == 0.0f);  // old items never fade
    CharacterRecord s = *c;
    s.items = {{0, "rat_whisker", 2, 0.0f}, {3, "conjured_bread", 4, 1234.5f}};
    REQUIRE(repo.save_characters({s}) == DbStatus::Ok);
    auto back = repo.load_character(acct, id);
    REQUIRE(back.has_value());
    REQUIRE(back->items.size() == 2);
    CHECK(back->items[1].slot == 3);
    CHECK(back->items[1].seconds_left == 1234.5f);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("a Phase 4 (schema v5) database upgrades to v6: the 16-slot pack becomes pack and bank", "[db][items]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v5_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  std::int64_t acct = 0;
  std::int64_t id = 0;
  {
    SqliteRepository repo(path.string());
    REQUIRE(repo.create_account("hoarder", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Hoarder");
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    id = c.id;
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    std::string sql = "ALTER TABLE characters DROP COLUMN bank_coin; ALTER TABLE characters DROP COLUMN factions; "
                      "ALTER TABLE characters DROP COLUMN quests; DROP TABLE auctions; DELETE FROM character_items;"
                      "ALTER TABLE accounts DROP COLUMN banned_until; ALTER TABLE accounts DROP COLUMN ban_reason; "
                      "ALTER TABLE accounts DROP COLUMN muted_until; ALTER TABLE characters DROP COLUMN social; "
                      "ALTER TABLE characters DROP COLUMN lockouts; ALTER TABLE raids DROP COLUMN main_assist; "
                      "ALTER TABLE characters DROP COLUMN books; "
                      "DROP TABLE guild_members; DROP TABLE guilds; DROP TABLE audit_log;";
    sql += "INSERT INTO character_items (char_id, slot, item_id, count, seconds_left) VALUES (" + std::to_string(id) + ", 2, 'rat_whisker', 3, 0), (" +
           std::to_string(id) + ", 12, 'bone_chips', 5, 0);";
    sql += "PRAGMA user_version = 5;";
    REQUIRE(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto c = repo.load_character(acct, id);
    REQUIRE(c.has_value());
    REQUIRE(c->items.size() == 2);
    CHECK(c->items[0].slot == 0x0102);  // pack slot 2
    CHECK(c->items[1].slot == 0x0204);  // bank slot 4 (old slot 12)
    CHECK(c->bank_coin == 0);
    CharacterRecord s = *c;
    s.bank_coin = 777;
    REQUIRE(repo.save_characters({s}) == DbStatus::Ok);
    CHECK(repo.load_character(acct, id)->bank_coin == 777);
    REQUIRE(repo.load_auctions());
    CHECK(repo.load_auctions()->empty());
  }
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + "-wal");
  std::filesystem::remove(path.string() + "-shm");
}

TEST_CASE("auction changes and the characters they touch commit together or not at all", "[db][items][auction]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("seller", "h", acct) == DbStatus::Ok);
  auto c = make_char(acct, "Seller");
  c.items = {{0x0100, "longsword_t2", 1, 0.0f}};  // a creation kit is stored with the character
  REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
  auto loaded = repo.load_character(acct, c.id);
  REQUIRE(loaded);
  REQUIRE(loaded->items.size() == 1);
  // List it: the item leaves the pack and the listing appears, in one transaction.
  CharacterRecord after = *loaded;
  after.items.clear();
  AuctionRecord l{42, c.id, "Seller", "longsword_t2", 1, 500, 1, 1000, 0, ""};
  REQUIRE(repo.apply_auction({l}, {}, {after}, false) == DbStatus::Ok);
  REQUIRE(repo.load_auctions()->size() == 1);
  CHECK(repo.load_character(acct, c.id)->items.empty());
  // A change that deletes a listing that is not there fails whole: the character save rolls back too.
  CharacterRecord bogus = after;
  bogus.coin = 999999;
  CHECK(repo.apply_auction({}, {12345}, {bogus}, false) == DbStatus::Error);
  CHECK(repo.load_character(acct, c.id)->coin == after.coin);
  // A retry may re-send a delete that already happened: with missing_ok it is skipped, not fatal.
  CHECK(repo.apply_auction({}, {12345}, {after}, true) == DbStatus::Ok);
  // Collect: listing gone, coin paid.
  CharacterRecord paid = after;
  paid.coin += 500;
  REQUIRE(repo.apply_auction({}, {42}, {paid}, false) == DbStatus::Ok);
  CHECK(repo.load_auctions()->empty());
  CHECK(repo.load_character(acct, c.id)->coin == after.coin + 500);
  // Deleting the seller removes their listings.
  REQUIRE(repo.apply_auction({AuctionRecord{43, c.id, "Seller", "longsword_t2", 1, 5, 1, 1000, 0, ""}}, {}, {}, false) == DbStatus::Ok);
  REQUIRE(repo.delete_character(acct, c.id) == DbStatus::Ok);
  CHECK(repo.load_auctions()->empty());
}

// ---------------------------------------------------------------- Phase 7: guilds, social lists, bans, audit

TEST_CASE("guilds: create, roster, ranks, message of the day, delete", "[db][social]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("founder", "h", acct) == DbStatus::Ok);
  auto a = make_char(acct, "Founder");
  auto b = make_char(acct, "Second");
  a.level = 20;
  REQUIRE(repo.create_character(a, 4) == DbStatus::Ok);
  REQUIRE(repo.create_character(b, 4) == DbStatus::Ok);
  a.level = 20;
  REQUIRE(repo.save_characters({a}) == DbStatus::Ok);
  std::int64_t gid = 0;
  REQUIRE(repo.create_guild("The Iron Vow", a.id, gid) == DbStatus::Ok);
  std::int64_t dup = 0;
  CHECK(repo.create_guild("the iron vow", b.id, dup) == DbStatus::Duplicate);  // names are unique in any case
  CHECK(repo.create_guild("Second Guild", a.id, dup) == DbStatus::Error);      // a leader belongs to one guild
  REQUIRE(repo.set_guild_member(gid, b.id, 3) == DbStatus::Ok);
  REQUIRE(repo.set_guild_member(gid, b.id, 2) == DbStatus::Ok);  // a rank change
  REQUIRE(repo.set_guild_motd(gid, "Raid at dusk") == DbStatus::Ok);
  CHECK(repo.set_guild_member(999, b.id, 1) != DbStatus::Ok);  // already in a guild (or no such guild)
  // A handover writes both ranks or neither.
  CHECK(repo.set_guild_ranks(gid, {{b.id, 0}, {a.id + 1000, 1}}) == DbStatus::NotFound);
  CHECK(repo.load_guilds()->front().members[1].rank == 2);  // b's change rolled back with the failed one
  REQUIRE(repo.set_guild_ranks(gid, {{b.id, 0}, {a.id, 1}}) == DbStatus::Ok);
  REQUIRE(repo.set_guild_ranks(gid, {{b.id, 2}, {a.id, 0}}) == DbStatus::Ok);
  auto all = repo.load_guilds();
  REQUIRE(all);
  REQUIRE(all->size() == 1);
  const GuildRecord& g = all->front();
  CHECK(g.name == "The Iron Vow");
  CHECK(g.motd == "Raid at dusk");
  REQUIRE(g.members.size() == 2);
  CHECK(g.members[0].name == "Founder");
  CHECK(g.members[0].rank == 0);
  CHECK(g.members[0].level == 20);
  CHECK(g.members[1].rank == 2);
  // Deleting a character takes them off the roster; deleting the guild clears the rest.
  REQUIRE(repo.delete_character(acct, b.id) == DbStatus::Ok);
  CHECK(repo.load_guilds()->front().members.size() == 1);
  CHECK(repo.remove_guild_member(b.id) == DbStatus::NotFound);
  REQUIRE(repo.delete_guild(gid) == DbStatus::Ok);
  CHECK(repo.load_guilds()->empty());
  CHECK(repo.delete_guild(gid) == DbStatus::NotFound);
}

TEST_CASE("social lists save with the character", "[db][social]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("pal", "h", acct) == DbStatus::Ok);
  auto c = make_char(acct, "Friendly");
  REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
  CHECK(repo.load_character(acct, c.id)->social == "{}");
  c.social = R"({"friends":["Aldric"],"ignore":["Spammer"],"muted":16})";
  REQUIRE(repo.save_characters({c}) == DbStatus::Ok);
  CHECK(repo.load_character(acct, c.id)->social == c.social);
}

TEST_CASE("bans, mutes, admin rights and the audit log", "[db][admin]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("rowdy", "h", acct) == DbStatus::Ok);
  auto c = make_char(acct, "Rowdy");
  REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
  auto by_char = repo.find_account_by_character("rOWDY");
  REQUIRE(by_char);
  CHECK(by_char->username == "rowdy");
  CHECK_FALSE(repo.find_account_by_character("Nobody"));
  REQUIRE(repo.set_account_ban(acct, 1999999999, "spamming") == DbStatus::Ok);
  REQUIRE(repo.set_account_mute(acct, -1) == DbStatus::Ok);
  auto a = repo.find_account("rowdy");
  REQUIRE(a);
  CHECK(a->banned_until == 1999999999);
  CHECK(a->ban_reason == "spamming");
  CHECK(a->muted_until == -1);
  CHECK(repo.set_account_ban(999, 1, "") == DbStatus::NotFound);
  CHECK_FALSE(a->is_admin);
  REQUIRE(repo.set_account_admin("rowdy", true) == DbStatus::Ok);
  CHECK(repo.find_account("rowdy")->is_admin);
  CHECK(repo.set_account_admin("ghost", true) == DbStatus::NotFound);
  REQUIRE(repo.append_audit({100, "gm", "Keeper", "ban", "Rowdy", "7 days"}) == DbStatus::Ok);
  REQUIRE(repo.append_audit({101, "gm", "Keeper", "unban", "Rowdy", ""}) == DbStatus::Ok);
  auto log = repo.recent_audit(10);
  REQUIRE(log.size() == 2);
  CHECK(log[0].command == "unban");  // newest first
  CHECK(log[1].detail == "7 days");
}

TEST_CASE("a Phase 6 (schema v7) database upgrades to v8 in place", "[db][social]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v7_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  std::int64_t acct = 0;
  std::int64_t id = 0;
  {
    SqliteRepository repo(path.string());
    REQUIRE(repo.create_account("elder", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Elder");
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    id = c.id;
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    const char* to_v7 = R"sql(
      ALTER TABLE accounts DROP COLUMN banned_until;
      ALTER TABLE accounts DROP COLUMN ban_reason;
      ALTER TABLE accounts DROP COLUMN muted_until;
      ALTER TABLE characters DROP COLUMN social;
      ALTER TABLE characters DROP COLUMN lockouts;
      ALTER TABLE raids DROP COLUMN main_assist;
      ALTER TABLE characters DROP COLUMN books;
      DROP TABLE guild_members;
      DROP TABLE guilds;
      DROP TABLE audit_log;
      PRAGMA user_version = 7;
    )sql";
    REQUIRE(sqlite3_exec(raw, to_v7, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto c = repo.load_character(acct, id);
    REQUIRE(c);
    CHECK(c->social == "{}");
    auto a = repo.find_account("elder");
    REQUIRE(a);
    CHECK(a->banned_until == 0);
    CHECK(a->muted_until == 0);
    REQUIRE(repo.load_guilds());
    CHECK(repo.load_guilds()->empty());
    std::int64_t gid = 0;
    CHECK(repo.create_guild("Old Guard", id, gid) == DbStatus::Ok);
  }
  std::filesystem::remove(path);
}

TEST_CASE("groups are saved, replaced, loaded in joining order and forgotten (schema v9)", "[db][social]") {
  SqliteRepository repo(":memory:");
  std::int64_t acct = 0;
  REQUIRE(repo.create_account("party", "h", acct) == DbStatus::Ok);
  std::vector<std::int64_t> ids;
  for (const char* n : {"Ana", "Bram", "Cora"}) {
    auto c = make_char(acct, n);
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    ids.push_back(c.id);
  }
  GroupRecord g{7, ids[1], 1, {{ids[1], "", 1, ""}, {ids[0], "", 1, ""}}};
  REQUIRE(repo.save_group(g) == DbStatus::Ok);
  auto all = repo.load_groups();
  REQUIRE(all);
  REQUIRE(all->size() == 1);
  CHECK((*all)[0].leader == ids[1]);
  CHECK((*all)[0].loot_rule == 1);
  REQUIRE((*all)[0].members.size() == 2);
  CHECK((*all)[0].members[0].name == "Bram");  // joining order, names from the characters
  CHECK((*all)[0].members[1].name == "Ana");
  // Saving again replaces the roster; a member moving to another group leaves this one.
  g.members.push_back({ids[2], "", 1, ""});
  REQUIRE(repo.save_group(g) == DbStatus::Ok);
  GroupRecord other{8, ids[2], 0, {{ids[2], "", 1, ""}}};
  REQUIRE(repo.save_group(other) == DbStatus::Ok);
  all = repo.load_groups();
  REQUIRE(all->size() == 2);
  CHECK((*all)[0].members.size() == 2);
  CHECK((*all)[1].members.size() == 1);
  // Saving a smaller roster drops the member left out.
  g.members.pop_back();
  REQUIRE(repo.save_group(g) == DbStatus::Ok);
  all = repo.load_groups();
  REQUIRE(all->size() == 2);
  CHECK((*all)[0].members.size() == 2);
  // A deleted character drops out of its group (ON DELETE CASCADE).
  REQUIRE(repo.delete_character(acct, ids[0]) == DbStatus::Ok);
  all = repo.load_groups();
  CHECK((*all)[0].members.size() == 1);
  CHECK(repo.delete_group(7) == DbStatus::Ok);
  CHECK(repo.delete_group(7) == DbStatus::NotFound);
  CHECK(repo.load_groups()->size() == 1);
}

TEST_CASE("a Phase 11 (schema v8) database upgrades to v9 in place", "[db][social]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v8_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  {
    SqliteRepository repo(path.string());
    std::int64_t acct = 0;
    REQUIRE(repo.create_account("elder", "h", acct) == DbStatus::Ok);
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "DROP TABLE group_members; DROP TABLE groups; ALTER TABLE characters DROP COLUMN lockouts; "
                              "ALTER TABLE raids DROP COLUMN main_assist; ALTER TABLE characters DROP COLUMN books; "
                              "PRAGMA user_version = 8;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto g = repo.load_groups();
    REQUIRE(g);
    CHECK(g->empty());
    CHECK(repo.find_account("elder"));
  }
  std::filesystem::remove(path);
}

TEST_CASE("raids are saved, replaced, loaded in joining order and forgotten (schema v10)", "[db][social]") {
  SqliteRepository repo(":memory:");
  RaidRecord r{3, 42, 1, {7, 5, 9}};
  REQUIRE(repo.save_raid(r) == DbStatus::Ok);
  auto all = repo.load_raids();
  REQUIRE(all);
  REQUIRE(all->size() == 1);
  CHECK((*all)[0].leader == 42);
  CHECK((*all)[0].loot_rule == 1);
  CHECK((*all)[0].groups == std::vector<std::int64_t>{7, 5, 9});  // joining order
  // Saving again replaces the groups; a group moving to another raid leaves this one.
  r.groups = {7, 5};
  r.leader = 43;
  REQUIRE(repo.save_raid(r) == DbStatus::Ok);
  REQUIRE(repo.save_raid(RaidRecord{4, 50, 0, {9, 5}}) == DbStatus::Ok);
  all = repo.load_raids();
  REQUIRE(all->size() == 2);
  CHECK((*all)[0].leader == 43);
  CHECK((*all)[0].groups == std::vector<std::int64_t>{7});
  CHECK((*all)[1].groups == std::vector<std::int64_t>{9, 5});
  CHECK(repo.delete_raid(3) == DbStatus::Ok);
  CHECK(repo.delete_raid(3) == DbStatus::NotFound);
  all = repo.load_raids();
  REQUIRE(all->size() == 1);
  CHECK((*all)[0].id == 4);
}

TEST_CASE("a Phase 13 (schema v9) database upgrades to v10 in place", "[db][social]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_test_v9_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  {
    SqliteRepository repo(path.string());
    std::int64_t acct = 0;
    REQUIRE(repo.create_account("raider", "h", acct) == DbStatus::Ok);
  }
  {
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(path.string().c_str(), &raw) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, "DROP TABLE raid_groups; DROP TABLE raids; ALTER TABLE characters DROP COLUMN lockouts; "
                              "ALTER TABLE characters DROP COLUMN books; PRAGMA user_version = 9;", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
  }
  {
    SqliteRepository repo(path.string());
    auto r = repo.load_raids();
    REQUIRE(r);
    CHECK(r->empty());
    CHECK(repo.find_account("raider"));
  }
  std::filesystem::remove(path);
}

// ------------------------------------------------------------------ Phase 8: integrity and backups

namespace {

std::string temp_db(const char* tag) {
  const auto p = std::filesystem::temp_directory_path() / ("ef_" + std::string(tag) + "_" + std::to_string(::getpid()) + ".db");
  for (const char* ext : {"", "-wal", "-shm"}) std::filesystem::remove(p.string() + ext);
  return p.string();
}

void fill(const std::string& path, int accounts) {
  SqliteRepository repo(path);
  for (int i = 0; i < accounts; ++i) {
    std::int64_t id = 0;
    REQUIRE(repo.create_account("user" + std::to_string(i), std::string(200, 'h'), id) == DbStatus::Ok);
  }
}

}  // namespace

TEST_CASE("a healthy database passes its checks and backs up to a checked, complete copy", "[db][backup]") {
  const auto path = temp_db("healthy");
  const auto copy = path + ".bak";
  std::filesystem::remove(copy);
  fill(path, 300);
  {
    SqliteRepository repo(path, DbCheck::Full);
    CHECK(repo.integrity_problems(false).empty());
    CHECK(repo.integrity_problems(true).empty());
    repo.backup_to(copy);  // while it is open, as on a running server
  }
  REQUIRE(std::filesystem::exists(copy));
  CHECK_FALSE(std::filesystem::exists(copy + ".partial"));
  SqliteRepository restored(copy, DbCheck::Full);
  CHECK(restored.find_account("user0"));
  CHECK(restored.find_account("user299"));
  for (const auto& p : {path, copy})
    for (const char* ext : {"", "-wal", "-shm"}) std::filesystem::remove(p + ext);
}

TEST_CASE("a damaged database is refused at open, before anything writes to it", "[db][backup]") {
  const auto path = temp_db("damaged");
  fill(path, 300);
  const auto size = std::filesystem::file_size(path);
  REQUIRE(size > 4096 * 4);
  {
    // Scribble over the middle pages, as a failing disk or a half-copied file would.
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    REQUIRE(f);
    std::fseek(f, 4096 * 2, SEEK_SET);
    const std::string junk(4096 * 2, '\x5A');
    std::fwrite(junk.data(), 1, junk.size(), f);
    std::fclose(f);
  }
  CHECK_THROWS_AS(SqliteRepository(path, DbCheck::Quick), DbCorruptError);
  CHECK(std::filesystem::file_size(path) == size);  // untouched: no migration ran on it
  // A file that is not a database at all is refused too, even with checks off.
  const auto junk_path = temp_db("notadb");
  {
    std::FILE* f = std::fopen(junk_path.c_str(), "wb");
    REQUIRE(f);
    const std::string junk(8192, 'x');
    std::fwrite(junk.data(), 1, junk.size(), f);
    std::fclose(f);
  }
  CHECK_THROWS_AS(SqliteRepository(junk_path, DbCheck::Off), DbCorruptError);
  for (const auto& p : {path, junk_path})
    for (const char* ext : {"", "-wal", "-shm"}) std::filesystem::remove(p + ext);
}
