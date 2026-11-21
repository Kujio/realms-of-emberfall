// Shared helpers for zone-level tests: the real content, a recording ZoneSink, and spawn helpers.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "game/zone.hpp"

namespace zt {

using namespace ef::game;
namespace proto = ef::proto;

inline const Content& content() {
  static const Content c = load_content(EF_CONTENT_DIR);
  return c;
}

// Decodes everything the zone sends and keeps it per recipient entity.
struct RecordingSink final : ZoneSink {
  std::multimap<EntityId, EntityId> spawns;    // to -> who
  std::multimap<EntityId, EntityId> despawns;  // to -> who
  std::map<EntityId, proto::Snapshot> last_snapshot;
  std::vector<std::pair<EntityId, std::string>> corrections;
  std::vector<std::pair<EntityId, std::string>> violations;
  std::map<EntityId, std::vector<std::vector<std::uint8_t>>> packets;  // reliable + unreliable, in order
  int deaths_persisted = 0;
  int loots_persisted = 0;
  int corpses_removed = 0;
  int saves = 0;

  void zone_send(const PlayerEntity& to, std::uint8_t /*channel*/, std::span<const std::uint8_t> bytes) override {
    std::vector<std::uint8_t> pkt(bytes.begin(), bytes.end());
    proto::EntitySpawn sp;
    proto::EntityDespawn de;
    proto::Snapshot sn;
    if (proto::decode_packet(pkt, sp)) spawns.emplace(to.id, sp.entity_id);
    else if (proto::decode_packet(pkt, de)) despawns.emplace(to.id, de.entity_id);
    else if (proto::decode_packet(pkt, sn)) last_snapshot[to.id] = sn;
    packets[to.id].push_back(std::move(pkt));
  }
  void zone_correction(const PlayerEntity& to, const char* r) override { corrections.emplace_back(to.id, r); }
  void zone_violation(const PlayerEntity& who, const char* r) override { violations.emplace_back(who.id, r); }
  void zone_player_died(const PlayerEntity&, const CorpseEntity&) override { ++deaths_persisted; }
  void zone_corpse_looted(const CorpseEntity&, const PlayerEntity&) override { ++loots_persisted; }
  void zone_corpse_removed(const CorpseEntity&) override { ++corpses_removed; }
  void zone_character_changed(const PlayerEntity&) override { ++saves; }

  bool spawned(EntityId to, EntityId who) const {
    auto [a, b] = spawns.equal_range(to);
    return std::any_of(a, b, [&](const auto& kv) { return kv.second == who; });
  }
  bool despawned(EntityId to, EntityId who) const {
    auto [a, b] = despawns.equal_range(to);
    return std::any_of(a, b, [&](const auto& kv) { return kv.second == who; });
  }

  // Every message of type M sent to `to`, oldest first.
  template <class M>
  std::vector<M> all(EntityId to) const {
    std::vector<M> out;
    auto it = packets.find(to);
    if (it == packets.end()) return out;
    for (const auto& p : it->second) {
      M m;
      if (proto::decode_packet(p, m)) out.push_back(std::move(m));
    }
    return out;
  }
  template <class M>
  std::optional<M> last(EntityId to) const {
    auto v = all<M>(to);
    if (v.empty()) return std::nullopt;
    return v.back();
  }
  // Chat lines (any channel) sent to `to` that contain `needle`.
  int chat_count(EntityId to, const std::string& needle) const {
    int n = 0;
    for (const auto& m : all<proto::ChatMessage>(to))
      if (m.text.find(needle) != std::string::npos) ++n;
    return n;
  }
  void clear() {
    packets.clear();
    spawns.clear();
    despawns.clear();
  }
};

inline ZoneDef test_zone() {
  ZoneDef z;
  z.id = "test";
  z.name = "Test";
  z.min_x = -500;
  z.min_z = -500;
  z.max_x = 500;
  z.max_z = 500;
  z.interest_radius = 50;
  z.say_radius = 30;
  return z;
}

inline ZonePlayerInit at(float x, float z, const char* name, int level = 1) {
  static std::int64_t next_char = 1000;
  ZonePlayerInit i;
  i.name = name;
  i.race = "highlander";
  i.class_id = "warrior";
  i.char_id = ++next_char;
  i.pos = {x, 0, z};
  i.level = level;
  i.bind_zone = "test";
  i.bind_pos = {0, 0, 0};
  return i;
}

// A player of another class (spellbook empty = the class starter spells).
inline ZonePlayerInit as_class(ZonePlayerInit i, const char* class_id) {
  i.class_id = class_id;
  return i;
}

// Every spell of the player's class up to its level, with `gems` memorized in order.
inline ZonePlayerInit with_spells(ZonePlayerInit i, std::vector<std::string> gems = {}) {
  for (const AbilityDef* a : content().spells_for(i.class_id))
    if (a->level <= i.level) i.spellbook.push_back(a->id);
  i.gems = std::move(gems);
  return i;
}

inline const AbilityDef& spell(const char* id) {
  const AbilityDef* a = content().find_spell(id);
  if (!a) throw std::runtime_error(std::string("no spell ") + id);
  return *a;
}

inline const MobDef& mob(const char* id) {
  const MobDef* m = content().find_mob(id);
  if (!m) throw std::runtime_error(std::string("no mob ") + id);
  return *m;
}

constexpr float kDt = 0.05f;

}  // namespace zt
