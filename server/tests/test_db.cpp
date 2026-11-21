#include <catch2/catch_test_macros.hpp>

#include <filesystem>

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

TEST_CASE("positions persist", "[db]") {
  const auto path = std::filesystem::temp_directory_path() / "emberfall_test_positions.db";
  std::filesystem::remove(path);
  std::int64_t char_id = 0;
  std::int64_t acct = 0;
  {
    SqliteRepository repo(path.string());
    REQUIRE(repo.create_account("carol", "h", acct) == DbStatus::Ok);
    auto c = make_char(acct, "Carol");
    REQUIRE(repo.create_character(c, 4) == DbStatus::Ok);
    char_id = c.id;
    REQUIRE(repo.save_positions({{char_id, "proving_flats", 12.5f, 0.0f, -3.25f, 1.0f}}) == DbStatus::Ok);
  }
  {
    SqliteRepository repo(path.string());  // reopen: migrations are idempotent
    auto c = repo.load_character(acct, char_id);
    REQUIRE(c.has_value());
    CHECK(c->x == 12.5f);
    CHECK(c->z == -3.25f);
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
