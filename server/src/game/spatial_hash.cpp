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

void SpatialHash::erase_from_cell(const Entry& e, EntityId id) {
  Bucket& v = *e.bucket;
  auto pos = std::find_if(v.begin(), v.end(), [id](const Hit& h) { return h.id == id; });
  if (pos != v.end()) {
    *pos = v.back();
    v.pop_back();
  }
  if (v.empty()) cells_.erase(e.cell);
}

void SpatialHash::insert(EntityId id, float x, float z) {
  remove(id);
  const auto cell = key(cell_coord(x), cell_coord(z));
  Bucket& b = cells_[cell];
  b.push_back({id, x, z});
  entries_.emplace(id, Entry{cell, &b});
}

void SpatialHash::update(EntityId id, float x, float z) {
  auto it = entries_.find(id);
  if (it == entries_.end()) {
    insert(id, x, z);
    return;
  }
  const auto cell = key(cell_coord(x), cell_coord(z));
  if (cell != it->second.cell) {
    erase_from_cell(it->second, id);
    Bucket& b = cells_[cell];
    b.push_back({id, x, z});
    it->second = Entry{cell, &b};
    return;
  }
  for (Hit& h : *it->second.bucket)
    if (h.id == id) {
      h.x = x;
      h.z = z;
      return;
    }
}

void SpatialHash::remove(EntityId id) {
  auto it = entries_.find(id);
  if (it == entries_.end()) return;
  erase_from_cell(it->second, id);
  entries_.erase(it);
}

template <class Fn>
void SpatialHash::for_each_in(float x, float z, float radius, Fn&& fn) const {
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
      for (const Hit& h : it->second) {
        const float dx = h.x - x;
        const float dz = h.z - z;
        if (dx * dx + dz * dz <= r2) fn(h);
      }
    }
  }
}

void SpatialHash::query(float x, float z, float radius, std::vector<EntityId>& out) const {
  for_each_in(x, z, radius, [&out](const Hit& h) { out.push_back(h.id); });
}

void SpatialHash::query(float x, float z, float radius, std::vector<Hit>& out) const {
  for_each_in(x, z, radius, [&out](const Hit& h) { out.push_back(h); });
}

}  // namespace ef::game
