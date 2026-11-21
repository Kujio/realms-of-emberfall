// Per-mob hate list. Pure data structure; the zone decides what generates threat.
//
// Rules: damage adds threat equal to the damage (abilities may add bonus threat); a successful
// taunt puts the taunter just above the current top; healing and buffing an ally add threat to
// every mob already fighting that ally (see heal_threat). The mob attacks whoever is on top.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "game/spatial_hash.hpp"

namespace ef::game {

class ThreatTable {
 public:
  struct Entry {
    EntityId id = 0;
    double threat = 0.0;
  };

  // Adds (or creates) threat. Negative amounts are clamped so threat never drops below 0.
  void add(EntityId id, double amount) {
    Entry& e = entry(id);
    e.threat = std::max(0.0, e.threat + amount);
  }

  // Moves `id` just above the current top (no change if it is already on top). Returns true if
  // the top changed.
  bool taunt(EntityId id) {
    const EntityId before = top();
    if (before == id) return false;
    const double top_threat = before != 0 ? threat_of(before) : 0.0;
    entry(id).threat = top_threat + 1.0;
    return top() != before;
  }

  void remove(EntityId id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.id == id; }), entries_.end());
  }
  void clear() { entries_.clear(); }

  bool empty() const { return entries_.empty(); }
  std::size_t size() const { return entries_.size(); }
  bool contains(EntityId id) const { return find(id) != nullptr; }
  double threat_of(EntityId id) const {
    const Entry* e = find(id);
    return e ? e->threat : 0.0;
  }

  // Highest threat; ties go to whoever got there first. 0 when empty.
  EntityId top() const {
    const Entry* best = nullptr;
    for (const auto& e : entries_)
      if (!best || e.threat > best->threat) best = &e;
    return best ? best->id : 0;
  }

  const std::vector<Entry>& entries() const { return entries_; }

 private:
  Entry& entry(EntityId id) {
    for (auto& e : entries_)
      if (e.id == id) return e;
    entries_.push_back({id, 0.0});
    return entries_.back();
  }
  const Entry* find(EntityId id) const {
    for (const auto& e : entries_)
      if (e.id == id) return &e;
    return nullptr;
  }

  std::vector<Entry> entries_;
};

// Threat a healer earns on each mob fighting the healed target: half the healing, split evenly.
inline double heal_threat(int healed, std::size_t mobs_engaged) {
  if (healed <= 0 || mobs_engaged == 0) return 0.0;
  return 0.5 * static_cast<double>(healed) / static_cast<double>(mobs_engaged);
}

}  // namespace ef::game
