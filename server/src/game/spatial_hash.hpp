// Uniform-grid spatial hash on the XZ plane for interest management and proximity queries.
// Each cell keeps its entities' positions next to their ids, so a query reads one contiguous list per
// cell and never looks entities up (Phase 8: interest management was a top tick hotspot).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ef::game {

using EntityId = std::uint32_t;

class SpatialHash {
 public:
  struct Hit {
    EntityId id;
    float x;
    float z;
  };

  explicit SpatialHash(float cell_size);

  void insert(EntityId id, float x, float z);  // replaces if present
  void update(EntityId id, float x, float z);  // inserts if absent
  void remove(EntityId id);
  bool contains(EntityId id) const { return entries_.contains(id); }
  std::size_t size() const { return entries_.size(); }

  // Appends every entity within `radius` (inclusive) of (x, z) to `out`.
  void query(float x, float z, float radius, std::vector<EntityId>& out) const;
  // The same, with each entity's position as last inserted or updated.
  void query(float x, float z, float radius, std::vector<Hit>& out) const;

 private:
  using Bucket = std::vector<Hit>;
  struct Entry {
    std::int64_t cell;
    Bucket* bucket;  // cells_ is node-based: the pointer stays valid until the cell is erased (when empty)
  };
  std::int32_t cell_coord(float v) const;
  static std::int64_t key(std::int32_t cx, std::int32_t cz);
  void erase_from_cell(const Entry& e, EntityId id);
  template <class Fn>
  void for_each_in(float x, float z, float radius, Fn&& fn) const;

  float inv_cell_;
  std::unordered_map<std::int64_t, Bucket> cells_;
  std::unordered_map<EntityId, Entry> entries_;
};

}  // namespace ef::game
