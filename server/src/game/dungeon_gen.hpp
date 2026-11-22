// Seeded procedural dungeons: rooms in a grid of sectors, joined by corridors into a tree. Integer
// math only (xorshift32), so the client's copy (client/scripts/world/dungeon_gen.gd) builds the
// identical layout from the same seed. Pure and unit-tested; a golden hash pins the output.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/math.hpp"
#include "game/world_geom.hpp"

namespace ef::game {

struct DungeonParams {
  std::uint32_t seed = 1;
  int rooms = 8;        // exact number of rooms (2..64)
  float cell = 4.0f;    // metres per tile
  int sector = 12;      // tiles per sector side (one room per sector)
  int min_room = 4;     // room side in tiles
  int max_room = 8;
  int corridor = 2;     // corridor width in tiles
};

// The validator and the server reject parameters outside these.
std::string check_dungeon_params(const DungeonParams& p);

struct DungeonRoom {
  int x = 0;  // first tile
  int z = 0;
  int w = 1;
  int h = 1;
  int depth = 0;   // corridor steps from the entrance
  int parent = -1; // the room it branches from (-1 = the entrance)
};

struct DungeonLayout {
  TileGrid tiles;
  std::vector<DungeonRoom> rooms;  // [0] is the entrance
  int boss = 0;                    // the deepest room (it is also made the largest)
  Vec3 room_center(int i) const;
  // Radius of a circle that fits inside the room with a tile of margin.
  float room_radius(int i) const;
  float min_x() const { return tiles.origin_x; }
  float min_z() const { return tiles.origin_z; }
  float max_x() const { return tiles.origin_x + static_cast<float>(tiles.w) * tiles.cell; }
  float max_z() const { return tiles.origin_z + static_cast<float>(tiles.h) * tiles.cell; }
};

// xorshift32: identical on every platform and in GDScript.
class XorShift32 {
 public:
  explicit XorShift32(std::uint32_t seed) : s_(seed == 0 ? 0x9E3779B9u : seed) {}
  std::uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  int below(int n) { return n <= 1 ? 0 : static_cast<int>(next() % static_cast<std::uint32_t>(n)); }

 private:
  std::uint32_t s_;
};

DungeonLayout generate_dungeon(const DungeonParams& p);
std::uint32_t dungeon_hash(const DungeonLayout& d);

}  // namespace ef::game
