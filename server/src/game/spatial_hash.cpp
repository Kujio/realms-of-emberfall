#include "game/spatial_hash.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ef::game {

SpatialHash::SpatialHash(float cell_size) : inv_cell_(1.0f / (cell_size > 0.0f ? cell_size : 1.0f)) {}

std::int32_t SpatialHash::cell_coord(float v) const {
  const float c = std::floor(v * inv_cell_);
  constexpr float kLimit = 1.0e9f;
  return static_cast<std::int32_t>(std::clamp(c, -kLimit, kLimit));
}

std::int64_t SpatialHash::key(std::int32_t cx, std::int32_t cz) {
  return static_cast<std::int64_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                                   static_cast<std::uint32_t>(cz));
}

void SpatialHash::erase_from_cell(std::int64_t cell, EntityId id) {
  auto it = cells_.find(cell);
  if (it == cells_.end()) return;
  auto& v = it->second;
  auto pos = std::find(v.begin(), v.end(), id);
  if (pos != v.end()) {
    *pos = v.back();
    v.pop_back();
  }
  if (v.empty()) cells_.erase(it);
}

void SpatialHash::insert(EntityId id, float x, float z) {
  remove(id);
  const auto cell = key(cell_coord(x), cell_coord(z));
  cells_[cell].push_back(id);
  entries_.emplace(id, Entry{cell, x, z});
}

void SpatialHash::update(EntityId id, float x, float z) {
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    insert(id, x, z);
    return;
  }
  const auto cell = key(cell_coord(x), cell_coord(z));
  if (cell != it->second.cell) {
    erase_from_cell(it->second.cell, id);
    cells_[cell].push_back(id);
    it->second.cell = cell;
  }
  it->second.x = x;
  it->second.z = z;
}

void SpatialHash::remove(EntityId id) {
  auto it = entries_.find(id);
  if (it == entries_.end()) return;
  erase_from_cell(it->second.cell, id);
  entries_.erase(it);
}

void SpatialHash::query(float x, float z, float radius, std::vector<EntityId>& out) const {
  if (!(radius >= 0.0f)) return;
  const std::int32_t cx0 = cell_coord(x - radius);
  const std::int32_t cx1 = cell_coord(x + radius);
  const std::int32_t cz0 = cell_coord(z - radius);
  const std::int32_t cz1 = cell_coord(z + radius);
  const float r2 = radius * radius;
  for (std::int32_t cx = cx0; cx <= cx1; ++cx) {
    for (std::int32_t cz = cz0; cz <= cz1; ++cz) {
      auto it = cells_.find(key(cx, cz));
      if (it == cells_.end()) continue;
      for (EntityId id : it->second) {
        const auto& e = entries_.at(id);
        const float dx = e.x - x;
        const float dz = e.z - z;
        if (dx * dx + dz * dz <= r2) out.push_back(id);
      }
    }
  }
}

}  // namespace ef::game
