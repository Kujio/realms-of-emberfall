// Server-side navigation: a walkability grid rasterized from the zone's collision map at load, and a
// bounded A* over it. Mobs walk straight when the way is clear and follow a path when it is not.
// Pure (no zone or network state) and unit-tested.
#pragma once

#include <cstdint>
#include <vector>

#include "game/math.hpp"
#include "game/movement.hpp"
#include "game/world_geom.hpp"

namespace ef::game {

class NavGrid {
 public:
  static constexpr int kWindow = 160;  // A* searches a window this many cells across

  NavGrid() = default;
  // `clearance`: a body this wide must fit at a cell's centre for it to count as walkable.
  void build(const Bounds& b, float cell, const CollisionMap& map, float clearance);
  bool empty() const { return w_ == 0; }
  int width() const { return w_; }
  int height() const { return h_; }
  float cell() const { return cell_; }
  bool walkable_cell(int ix, int iz) const;
  bool walkable(float x, float z) const;
  // True if every cell the segment crosses is walkable.
  bool line_walkable(const Vec3& a, const Vec3& b) const;
  // Fills `out` with waypoints from `from` toward `to` (smoothed; the last one is `to` itself or the
  // reachable cell nearest it inside the search window). False when no step can be made.
  bool find_path(const Vec3& from, const Vec3& to, std::vector<Vec3>& out, int max_expansions = 4000) const;
  std::size_t blocked_count() const;

 private:
  int ix(float x) const { return static_cast<int>(std::floor((x - x0_) / cell_)); }
  int iz(float z) const { return static_cast<int>(std::floor((z - z0_) / cell_)); }
  Vec3 center(int cx, int cz) const {
    return {x0_ + (static_cast<float>(cx) + 0.5f) * cell_, 0.0f, z0_ + (static_cast<float>(cz) + 0.5f) * cell_};
  }

  float x0_ = 0.0f;
  float z0_ = 0.0f;
  float cell_ = 2.0f;
  int w_ = 0;
  int h_ = 0;
  std::vector<std::uint8_t> blocked_;
  // A* scratch over the window, reused between searches (single-threaded).
  mutable std::vector<float> g_;
  mutable std::vector<std::int32_t> parent_;
  mutable std::vector<std::uint32_t> seen_;
  mutable std::uint32_t gen_ = 0;
  mutable std::vector<std::pair<float, std::int32_t>> heap_;
  mutable std::vector<std::int32_t> cells_;
};

}  // namespace ef::game
