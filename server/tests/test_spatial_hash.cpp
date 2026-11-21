#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "game/spatial_hash.hpp"

using namespace ef::game;

namespace {
std::vector<EntityId> sorted_query(const SpatialHash& h, float x, float z, float r) {
  std::vector<EntityId> out;
  h.query(x, z, r, out);
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

TEST_CASE("spatial hash finds entities within radius across cells", "[spatial]") {
  SpatialHash h(10.0f);
  h.insert(1, 0.0f, 0.0f);
  h.insert(2, 9.0f, 0.0f);
  h.insert(3, -12.0f, 3.0f);
  h.insert(4, 50.0f, 50.0f);
  CHECK(sorted_query(h, 0, 0, 15) == std::vector<EntityId>{1, 2, 3});
  CHECK(sorted_query(h, 0, 0, 9) == std::vector<EntityId>{1, 2});
  CHECK(sorted_query(h, 50, 50, 1) == std::vector<EntityId>{4});
}

TEST_CASE("spatial hash update and remove", "[spatial]") {
  SpatialHash h(10.0f);
  h.insert(1, 0, 0);
  h.update(1, 100, 100);
  CHECK(sorted_query(h, 0, 0, 5).empty());
  CHECK(sorted_query(h, 100, 100, 5) == std::vector<EntityId>{1});
  h.update(2, -5, -5);  // update inserts when missing
  CHECK(h.size() == 2);
  h.remove(1);
  h.remove(1);  // idempotent
  CHECK(h.size() == 1);
  CHECK(sorted_query(h, 100, 100, 5).empty());
}

TEST_CASE("spatial hash handles negative coordinates and huge values", "[spatial]") {
  SpatialHash h(8.0f);
  h.insert(7, -0.5f, -0.5f);
  CHECK(sorted_query(h, 0.5f, 0.5f, 2.0f) == std::vector<EntityId>{7});
  h.insert(8, 1.0e12f, -1.0e12f);  // clamped cell coords must not overflow
  CHECK(h.contains(8));
  CHECK(sorted_query(h, 0, 0, -1.0f).empty());
}
