// Pure, unit-tested world geometry shared by the server and (mirrored line for line) the client:
// terrain height, 2D collision against circles, rotated boxes and wall tiles, and line of sight.
// The client's copy lives in client/scripts/world/zone_geom.gd; prediction replays inputs through
// it, so the two must stay identical (same shapes, same order, same push-out rule).
#pragma once

#include <cstdint>
#include <vector>

#include "game/math.hpp"

namespace ef::game {

// A round hill added to the terrain: height * (1 - (d/radius)^2)^2 inside the radius.
struct Hill {
  float x = 0.0f;
  float z = 0.0f;
  float radius = 1.0f;
  float height = 0.0f;  // negative = a hollow
};

struct Terrain {
  float base = 0.0f;
  std::vector<Hill> hills;
  float height(float x, float z) const;
};

// A round blocker (tree, pillar, rock).
struct Obstacle {
  float x = 0.0f;
  float z = 0.0f;
  float radius = 1.0f;
};

// A box blocker (wall, building) with half extents, rotated by yaw about its centre. Yaw follows
// Godot: local +X points to (cos yaw, -sin yaw) and local +Z to (sin yaw, cos yaw) in (x, z).
struct WallBox {
  float x = 0.0f;
  float z = 0.0f;
  float hx = 0.5f;
  float hz = 0.5f;
  float yaw = 0.0f;
};

// A grid of square cells, row-major by z; 1 = solid wall. Cell (0,0) starts at (origin_x, origin_z).
struct TileGrid {
  float origin_x = 0.0f;
  float origin_z = 0.0f;
  float cell = 4.0f;
  int w = 0;
  int h = 0;
  std::vector<std::uint8_t> solid;
  bool empty() const { return w <= 0 || h <= 0; }
  // Cells outside the grid count as solid.
  bool solid_cell(int ix, int iz) const;
  int cell_x(float x) const;
  int cell_z(float z) const;
};

class CollisionMap {
 public:
  CollisionMap() = default;
  CollisionMap(std::vector<Obstacle> circles, std::vector<WallBox> boxes, TileGrid tiles);

  bool empty() const { return circles_.empty() && boxes_.empty() && tiles_.empty(); }
  const std::vector<Obstacle>& circles() const { return circles_; }
  const std::vector<WallBox>& boxes() const { return boxes_; }
  const TileGrid& tiles() const { return tiles_; }

  // Pushes a body of `radius` at `p` out of everything it overlaps: circles, then boxes, then tiles,
  // twice over. y is left alone.
  Vec3 resolve(Vec3 p, float radius) const;
  // True if a body of `radius` at (x, z) touches anything.
  bool overlaps(float x, float z, float radius) const;
  // Line of sight: false if the segment passes through a box or a wall tile, or through a circle
  // neither end stands inside (standing against a pillar does not blind you).
  bool segment_clear(const Vec3& a, const Vec3& b) const;

 private:
  void candidates(float x, float z, float reach) const;

  std::vector<Obstacle> circles_;
  std::vector<WallBox> boxes_;
  TileGrid tiles_;
  // Broadphase: shapes (circles first, then boxes offset by circles_.size()) per bucket.
  float bucket_ = 16.0f;
  float bx0_ = 0.0f;
  float bz0_ = 0.0f;
  int bw_ = 0;
  int bh_ = 0;
  std::vector<std::vector<std::uint32_t>> buckets_;
  std::vector<std::uint32_t> unbucketed_;  // shapes too big for the grid: always candidates
  mutable std::vector<std::uint32_t> scratch_;
  mutable std::vector<std::uint32_t> stamp_;
  mutable std::uint32_t stamp_gen_ = 0;
};

// Pushes a point out of one circle / box (helpers shared by resolve and the nav grid). Return true
// if they moved the point.
bool push_out_circle(float& x, float& z, float radius, const Obstacle& o);
bool push_out_box(float& x, float& z, float radius, const WallBox& b);

// FNV-1a over 32-bit words: the layout hash the client checks its own zone data against.
class LayoutHash {
 public:
  void add(std::int64_t v);
  // Quantized: content values carry at most 2 decimals (positions, sizes) or 4 (yaw), so v * scale is
  // always within float error of an integer and rounds the same everywhere.
  void add_q(float v, double scale) { add(static_cast<std::int64_t>(std::llround(static_cast<double>(v) * scale))); }
  void add_cm(float v) { add_q(v, 100.0); }
  std::uint32_t value() const { return h_; }

 private:
  std::uint32_t h_ = 2166136261u;
};

}  // namespace ef::game
