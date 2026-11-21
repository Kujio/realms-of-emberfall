// Uniform-grid spatial hash on the XZ plane for interest management and proximity queries.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ef::game {

using EntityId = std::uint32_t;

class SpatialHash {
 public:
  explicit SpatialHash(float cell_size);

  void insert(EntityId id, float x, float z);  // replaces if present
  void update(EntityId id, float x, float z);  // inserts if absent
  void remove(EntityId id);
  bool contains(EntityId id) const { return entries_.contains(id); }
  std::size_t size() const { return entries_.size(); }

  // Appends every entity within `radius` (inclusive) of (x, z) to `out`.
  void query(float x, float z, float radius, std::vector<EntityId>& out) const;

 private:
  struct Entry {
    std::int64_t cell;
    float x;
    float z;
  };
  std::int32_t cell_coord(float v) const;
  static std::int64_t key(std::int32_t cx, std::int32_t cz);
  void erase_from_cell(std::int64_t cell, EntityId id);

  float inv_cell_;
  std::unordered_map<std::int64_t, std::vector<EntityId>> cells_;
  std::unordered_map<EntityId, Entry> entries_;
};

}  // namespace ef::game
