// World content (Phase 6): zone files, scene exports, dungeon layouts, zone lines, entries, travel
// points, and the checks that tie them together (every zone reachable, every line landing somewhere).
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>

#include "game/content.hpp"
#include "game/content_parser.hpp"
#include "game/factions.hpp"

namespace ef::game {

namespace {

using content_detail::Parser;
using nlohmann::json;

const std::set<std::string, std::less<>> kZoneKinds{"hub", "outdoor", "dungeon", "test"};

bool has(const json& j, const char* key) { return j.is_object() && j.contains(key); }

std::string room_of(Parser& p, const json& j, const std::string& w) {
  if (!has(j, "room")) return {};
  const auto& r = j.at("room");
  if (r.is_number_integer()) return std::to_string(r.get<long long>());
  if (r.is_string()) return r.get<std::string>();
  p.error(w + ": 'room' must be a room index or entrance/boss/all");
  return {};
}

void parse_obstacles(Parser& p, const json& j, const std::string& w, std::vector<Obstacle>& out) {
  if (!has(j, "obstacles")) return;
  if (!j.at("obstacles").is_array()) return p.error(w + ": obstacles must be an array");
  const auto& ob = j.at("obstacles");
  for (std::size_t k = 0; k < ob.size(); ++k) {
    const std::string wo = w + ".obstacles[" + std::to_string(k) + "]";
    Obstacle o;
    o.x = p.num(ob[k], "x", wo, 0.0f);
    o.z = p.num(ob[k], "z", wo, 0.0f);
    o.radius = p.num(ob[k], "radius", wo, 1.0f);
    out.push_back(o);
  }
}

void parse_walls(Parser& p, const json& j, const std::string& w, std::vector<WallBox>& out) {
  if (!has(j, "walls")) return;
  if (!j.at("walls").is_array()) return p.error(w + ": walls must be an array");
  const auto& wl = j.at("walls");
  for (std::size_t k = 0; k < wl.size(); ++k) {
    const std::string ww = w + ".walls[" + std::to_string(k) + "]";
    WallBox b;
    b.x = p.num(wl[k], "x", ww, 0.0f);
    b.z = p.num(wl[k], "z", ww, 0.0f);
    b.hx = p.num(wl[k], "hx", ww, 0.5f);
    b.hz = p.num(wl[k], "hz", ww, 0.5f);
    b.yaw = p.opt(wl[k], "yaw", ww, 0.0f);
    out.push_back(b);
  }
}

void parse_spawns(Parser& p, const json& j, const std::string& w, std::vector<MobSpawn>& out) {
  if (!has(j, "spawns")) return;
  if (!j.at("spawns").is_array()) return p.error(w + ": spawns must be an array");
  const auto& sp = j.at("spawns");
  for (std::size_t k = 0; k < sp.size(); ++k) {
    const std::string ws = w + ".spawns[" + std::to_string(k) + "]";
    MobSpawn s;
    s.mob = p.str(sp[k], "mob", ws);
    s.room = room_of(p, sp[k], ws);
    const bool in_room = !s.room.empty();
    s.x = p.num(sp[k], "x", ws, 0.0f, !in_room);
    s.z = p.num(sp[k], "z", ws, 0.0f, !in_room);
    s.count = p.integer(sp[k], "count", ws, 1, false);
    s.radius = p.opt(sp[k], "radius", ws, 0.0f);
    s.rare = p.str(sp[k], "rare", ws, false);
    s.rare_chance = p.opt(sp[k], "rare_chance", ws, s.rare.empty() ? 0.0f : 0.1f);
    out.push_back(std::move(s));
  }
}

void parse_lines(Parser& p, const json& j, const std::string& w, std::vector<ZoneLineDef>& out) {
  if (!has(j, "zone_lines")) return;
  if (!j.at("zone_lines").is_array()) return p.error(w + ": zone_lines must be an array");
  const auto& zl = j.at("zone_lines");
  for (std::size_t k = 0; k < zl.size(); ++k) {
    const std::string wl = w + ".zone_lines[" + std::to_string(k) + "]";
    ZoneLineDef l;
    l.id = p.str(zl[k], "id", wl);
    l.room = room_of(p, zl[k], wl);
    l.x = p.num(zl[k], "x", wl, 0.0f, l.room.empty());
    l.z = p.num(zl[k], "z", wl, 0.0f, l.room.empty());
    l.hx = p.opt(zl[k], "hx", wl, 4.0f);
    l.hz = p.opt(zl[k], "hz", wl, 4.0f);
    l.yaw = p.opt(zl[k], "yaw", wl, 0.0f);
    l.to = p.str(zl[k], "to", wl);
    l.entry = p.str(zl[k], "entry", wl);
    out.push_back(std::move(l));
  }
}

void parse_entries(Parser& p, const json& j, const std::string& w, std::vector<ZoneEntryDef>& out) {
  if (!has(j, "entries")) return;
  if (!j.at("entries").is_array()) return p.error(w + ": entries must be an array");
  const auto& en = j.at("entries");
  for (std::size_t k = 0; k < en.size(); ++k) {
    const std::string we = w + ".entries[" + std::to_string(k) + "]";
    ZoneEntryDef e;
    e.id = p.str(en[k], "id", we);
    e.room = room_of(p, en[k], we);
    e.at.x = p.num(en[k], "x", we, 0.0f, e.room.empty());
    e.at.z = p.num(en[k], "z", we, 0.0f, e.room.empty());
    e.at.yaw = p.opt(en[k], "yaw", we, 0.0f);
    out.push_back(std::move(e));
  }
}

// Room "entrance", "boss" or an index -> room index; -1 if none.
int room_index(const DungeonLayout& d, const std::string& room) {
  if (room == "entrance") return 0;
  if (room == "boss") return d.boss;
  if (room.empty() || room.size() > 3 || !std::all_of(room.begin(), room.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return -1;
  const int i = std::stoi(room);
  return i < static_cast<int>(d.rooms.size()) ? i : -1;
}

void build_dungeon(ZoneDef& z, std::vector<std::string>& errors) {
  const std::string w = "zone '" + z.id + "': ";
  if (const std::string why = check_dungeon_params(z.dungeon); !why.empty()) {
    errors.push_back(w + "dungeon " + why);
    return;
  }
  auto layout = std::make_shared<DungeonLayout>(generate_dungeon(z.dungeon));
  const DungeonLayout& d = *layout;
  z.min_x = d.min_x();
  z.min_z = d.min_z();
  z.max_x = d.max_x();
  z.max_z = d.max_z();
  const Vec3 entrance = d.room_center(0);
  if (z.safe_spawn.x == 0.0f && z.safe_spawn.z == 0.0f) z.safe_spawn = {entrance.x, 0.0f, entrance.z, 0.0f};
  // Rooms in spawns, entries and zone lines become positions.
  std::vector<MobSpawn> spawns;
  for (auto& s : z.spawns) {
    if (s.room.empty()) {
      spawns.push_back(s);
      continue;
    }
    std::vector<int> rooms;
    if (s.room == "all") {
      for (int i = 1; i < static_cast<int>(d.rooms.size()); ++i)
        if (i != d.boss) rooms.push_back(i);
    } else if (const int i = room_index(d, s.room); i >= 0) {
      rooms.push_back(i);
    } else {
      errors.push_back(w + "spawn for '" + s.mob + "' names room '" + s.room + "', which the layout does not have");
    }
    for (int i : rooms) {
      MobSpawn r = s;
      const Vec3 c = d.room_center(i);
      r.x = c.x;
      r.z = c.z;
      r.radius = std::min(s.radius > 0.0f ? s.radius : 100.0f, d.room_radius(i));
      spawns.push_back(r);
    }
  }
  z.spawns = std::move(spawns);
  for (auto& e : z.entries) {
    if (e.room.empty()) continue;
    const int i = room_index(d, e.room);
    if (i < 0) {
      errors.push_back(w + "entry '" + e.id + "' names room '" + e.room + "', which the layout does not have");
      continue;
    }
    // Arrive a little to the side of the centre, where the room's exit line is not.
    const Vec3 c = d.room_center(i);
    e.at.x = c.x;
    e.at.z = c.z + d.room_radius(i) * 0.6f;
  }
  for (auto& l : z.zone_lines) {
    if (l.room.empty()) continue;
    const int i = room_index(d, l.room);
    if (i < 0) {
      errors.push_back(w + "zone line '" + l.id + "' names room '" + l.room + "', which the layout does not have");
      continue;
    }
    const Vec3 c = d.room_center(i);
    l.x = c.x;
    l.z = c.z - d.room_radius(i) * 0.6f;
    l.hx = std::min(l.hx, d.tiles.cell);
    l.hz = std::min(l.hz, d.tiles.cell * 0.5f);
  }
  z.layout = std::move(layout);
}

void hash_zone(ZoneDef& z) {
  LayoutHash h;
  h.add_cm(z.min_x);
  h.add_cm(z.min_z);
  h.add_cm(z.max_x);
  h.add_cm(z.max_z);
  h.add_cm(z.terrain.base);
  h.add(static_cast<std::int64_t>(z.terrain.hills.size()));
  for (const auto& hl : z.terrain.hills) {
    h.add_cm(hl.x);
    h.add_cm(hl.z);
    h.add_cm(hl.radius);
    h.add_cm(hl.height);
  }
  h.add(static_cast<std::int64_t>(z.obstacles.size()));
  for (const auto& o : z.obstacles) {
    h.add_cm(o.x);
    h.add_cm(o.z);
    h.add_cm(o.radius);
  }
  h.add(static_cast<std::int64_t>(z.walls.size()));
  for (const auto& b : z.walls) {
    h.add_cm(b.x);
    h.add_cm(b.z);
    h.add_cm(b.hx);
    h.add_cm(b.hz);
    h.add_q(b.yaw, 10000.0);
  }
  h.add(z.layout ? static_cast<std::int64_t>(dungeon_hash(*z.layout)) : 0);
  z.layout_hash = h.value();
}

}  // namespace

bool ZoneLineDef::contains(float px, float pz) const {
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const float dx = px - x;
  const float dz = pz - z;
  const float lx = dx * c - dz * s;
  const float lz = dx * s + dz * c;
  return std::fabs(lx) <= hx && std::fabs(lz) <= hz;
}

const ZoneEntryDef* ZoneDef::find_entry(std::string_view entry_id) const {
  for (const auto& e : entries)
    if (e.id == entry_id) return &e;
  return nullptr;
}

const TravelPointDef* Content::find_travel_point(std::string_view id) const {
  for (const auto& t : travel_points)
    if (t.id == id) return &t;
  return nullptr;
}

namespace content_detail {

void parse_zones(Parser& p, const ContentDocs& docs, Content& c) {
  // Scene exports first, keyed by zone, merged after the zone's own lists.
  std::map<std::string, const json*, std::less<>> scenes;
  if (const auto* list = p.list(docs.zone_scenes, "scenes", "zones/scenes")) {
    for (std::size_t i = 0; i < list->size(); ++i) {
      const std::string zid = p.str((*list)[i], "zone", "scenes[" + std::to_string(i) + "]");
      if (!zid.empty() && !scenes.emplace(zid, &(*list)[i]).second) p.error("zones/scenes: two scene exports for zone '" + zid + "'");
    }
  }
  const auto* list = p.list(docs.zones, "zones", "zones");
  if (!list) return;
  for (std::size_t i = 0; i < list->size(); ++i) {
    const auto& j = (*list)[i];
    const std::string w = "zones[" + std::to_string(i) + "]";
    ZoneDef z;
    z.id = p.str(j, "id", w);
    z.name = p.str(j, "name", w);
    z.description = p.str(j, "description", w, false);
    z.kind = p.str(j, "kind", w, false, "outdoor");
    z.persistent = p.boolean(j, "persistent", w, z.kind == "hub" || z.kind == "test", false);
    z.level_max = c.rules.max_level;
    if (has(j, "levels")) {
      z.level_min = p.integer(j.at("levels"), "min", w + ".levels", 1);
      z.level_max = p.integer(j.at("levels"), "max", w + ".levels", c.rules.max_level);
    }
    z.has_dungeon = has(j, "dungeon");
    if (z.has_dungeon) {
      const auto& d = j.at("dungeon");
      const std::string wd = w + ".dungeon";
      const int seed = p.integer(d, "seed", wd, 1);
      z.dungeon.seed = static_cast<std::uint32_t>(seed < 0 ? 1 : seed);
      z.dungeon.rooms = p.integer(d, "rooms", wd, 8);
      z.dungeon.cell = p.opt(d, "cell", wd, z.dungeon.cell);
      z.dungeon.sector = p.integer(d, "sector", wd, z.dungeon.sector, false);
      z.dungeon.min_room = p.integer(d, "min_room", wd, z.dungeon.min_room, false);
      z.dungeon.max_room = p.integer(d, "max_room", wd, z.dungeon.max_room, false);
      z.dungeon.corridor = p.integer(d, "corridor", wd, z.dungeon.corridor, false);
    } else if (has(j, "bounds")) {
      const auto& b = j.at("bounds");
      z.min_x = p.num(b, "min_x", w + ".bounds", 0.0f);
      z.min_z = p.num(b, "min_z", w + ".bounds", 0.0f);
      z.max_x = p.num(b, "max_x", w + ".bounds", 0.0f);
      z.max_z = p.num(b, "max_z", w + ".bounds", 0.0f);
    } else {
      p.error(w + ": missing 'bounds'");
    }
    z.ground_y = p.num(j, "ground_y", w, 0.0f, false);
    z.terrain.base = z.ground_y;
    if (has(j, "terrain")) {
      const auto& t = j.at("terrain");
      z.terrain.base = p.opt(t, "base", w + ".terrain", z.ground_y);
      z.ground_y = z.terrain.base;
      if (has(t, "hills")) {
        const auto& hl = t.at("hills");
        if (!hl.is_array()) p.error(w + ".terrain: hills must be an array of [x, z, radius, height]");
        for (std::size_t k = 0; hl.is_array() && k < hl.size(); ++k) {
          const auto& h = hl[k];
          if (!h.is_array() || h.size() != 4 || !std::all_of(h.begin(), h.end(), [](const json& v) { return v.is_number(); })) {
            p.error(w + ".terrain.hills[" + std::to_string(k) + "]: must be [x, z, radius, height]");
            continue;
          }
          z.terrain.hills.push_back({h[0].get<float>(), h[1].get<float>(), h[2].get<float>(), h[3].get<float>()});
        }
      }
    }
    if (has(j, "safe_spawn") || !z.has_dungeon) z.safe_spawn = p.spawn(j, "safe_spawn", w);
    z.interest_radius = p.opt(j, "interest_radius", w, 80.0f);
    z.say_radius = p.opt(j, "say_radius", w, 60.0f);
    z.nav_cell = p.opt(j, "nav_cell", w, 2.0f);
    z.scene = p.str(j, "scene", w, false);
    parse_spawns(p, j, w, z.spawns);
    parse_obstacles(p, j, w, z.obstacles);
    parse_walls(p, j, w, z.walls);
    parse_lines(p, j, w, z.zone_lines);
    parse_entries(p, j, w, z.entries);
    if (auto it = scenes.find(z.id); it != scenes.end()) {
      const json& sj = *it->second;
      const std::string ws = "scene '" + z.id + "'";
      parse_spawns(p, sj, ws, z.spawns);
      parse_obstacles(p, sj, ws, z.obstacles);
      parse_walls(p, sj, ws, z.walls);
      parse_lines(p, sj, ws, z.zone_lines);
      parse_entries(p, sj, ws, z.entries);
      if (has(sj, "npcs") && sj.at("npcs").is_array()) {
        for (const auto& n : sj.at("npcs")) {
          NpcPlacement pl;
          pl.npc = p.str(n, "id", ws + ".npcs");
          pl.x = p.num(n, "x", ws + ".npcs", 0.0f);
          pl.z = p.num(n, "z", ws + ".npcs", 0.0f);
          pl.yaw = p.opt(n, "yaw", ws + ".npcs", 0.0f);
          z.npc_placements.push_back(std::move(pl));
        }
      }
      scenes.erase(it);
    } else if (!z.scene.empty() && !docs.zone_scenes.is_null()) {
      p.error(w + ": scene '" + z.scene + "' has no export in zones/scenes (run tools/export_zones.sh)");
    }
    c.zones.push_back(std::move(z));
  }
  for (const auto& [zid, sj] : scenes) p.error("zones/scenes: export for unknown zone '" + zid + "'");
}

void parse_travel(Parser& p, const ContentDocs& docs, Content& c) {
  const auto* list = p.list(docs.travel, "points", "travel.json");
  if (!list) return;
  for (std::size_t i = 0; i < list->size(); ++i) {
    const auto& j = (*list)[i];
    const std::string w = "travel.points[" + std::to_string(i) + "]";
    TravelPointDef t;
    t.id = p.str(j, "id", w);
    t.name = p.str(j, "name", w);
    t.zone = p.str(j, "zone", w);
    t.at.x = p.num(j, "x", w, 0.0f);
    t.at.z = p.num(j, "z", w, 0.0f);
    t.at.yaw = p.opt(j, "yaw", w, 0.0f);
    c.travel_points.push_back(std::move(t));
  }
}

void finalize_world(Content& c, std::vector<std::string>& errors) {
  for (auto& z : c.zones) {
    if (z.has_dungeon) build_dungeon(z, errors);
    // Ground height everywhere from the terrain.
    z.safe_spawn.y = z.ground_at(z.safe_spawn.x, z.safe_spawn.z);
    hash_zone(z);
  }
  // Scene placements move their NPCs into that zone; an NPC stands in one place only.
  std::map<std::string, std::string, std::less<>> placed_in;
  for (const auto& z : c.zones)
    for (const auto& pl : z.npc_placements) {
      if (auto [it, fresh] = placed_in.emplace(pl.npc, z.id); !fresh) {
        errors.push_back("zone '" + z.id + "': scene places npc '" + pl.npc + "', which the scene of '" + it->second + "' already places");
        continue;
      }
      auto it = std::find_if(c.npcs.begin(), c.npcs.end(), [&](const NpcDef& n) { return n.id == pl.npc; });
      if (it == c.npcs.end()) {
        errors.push_back("zone '" + z.id + "': scene places unknown npc '" + pl.npc + "'");
        continue;
      }
      it->zone = z.id;
      it->x = pl.x;
      it->z = pl.z;
      it->yaw = pl.yaw;
    }
}

void validate_world(const Content& c, std::vector<std::string>& e) {
  auto inside = [](const ZoneDef& z, float x, float zz) { return x >= z.min_x && x <= z.max_x && zz >= z.min_z && zz <= z.max_z; };
  auto blocked = [](const ZoneDef& z, float x, float zz) {
    const CollisionMap map(z.obstacles, z.walls, z.layout ? z.layout->tiles : TileGrid{});
    return map.overlaps(x, zz, 0.3f);
  };
  for (const auto& z : c.zones) {
    const std::string w = "zone '" + z.id + "': ";
    if (!kZoneKinds.contains(z.kind)) e.push_back(w + "kind must be hub, outdoor, dungeon or test");
    if (z.kind == "dungeon" && !z.has_dungeon && z.walls.empty()) e.push_back(w + "a dungeon needs a 'dungeon' layout or walls");
    if (!z.has_dungeon) {
      // A room only means something in a generated layout (it would silently become (0, 0) elsewhere).
      for (const auto& sp : z.spawns)
        if (!sp.room.empty()) e.push_back(w + "spawn for '" + sp.mob + "' names a room, but the zone has no dungeon layout");
      for (const auto& en : z.entries)
        if (!en.room.empty()) e.push_back(w + "entry '" + en.id + "' names a room, but the zone has no dungeon layout");
      for (const auto& l : z.zone_lines)
        if (!l.room.empty()) e.push_back(w + "zone line '" + l.id + "' names a room, but the zone has no dungeon layout");
    }
    if (z.level_min < 1 || z.level_max < z.level_min || z.level_max > c.rules.max_level) e.push_back(w + "levels must satisfy 1 <= min <= max <= max_level");
    if (!(z.min_x < z.max_x && z.min_z < z.max_z)) e.push_back(w + "bounds min must be < max");
    if ((z.max_x - z.min_x) > 4000.0f || (z.max_z - z.min_z) > 4000.0f) e.push_back(w + "a zone may be at most 4 km across");
    if (z.interest_radius < 10.0f || z.interest_radius > 500.0f) e.push_back(w + "interest_radius must be 10..500");
    if (z.say_radius <= 0.0f) e.push_back(w + "say_radius must be > 0");
    if (z.nav_cell < 0.5f || z.nav_cell > 8.0f) e.push_back(w + "nav_cell must be 0.5..8");
    const auto& s = z.safe_spawn;
    if (!inside(z, s.x, s.z)) e.push_back(w + "safe_spawn is outside bounds");
    else if (blocked(z, s.x, s.z)) e.push_back(w + "safe_spawn is inside a wall or obstacle");
    if (z.obstacles.size() > 4000) e.push_back(w + "at most 4000 obstacles");
    if (z.walls.size() > 2000) e.push_back(w + "at most 2000 walls");
    if (z.terrain.hills.size() > 256) e.push_back(w + "at most 256 hills");
    for (const auto& h : z.terrain.hills)
      if (h.radius < 1.0f || h.radius > 2000.0f || std::fabs(h.height) > 200.0f) e.push_back(w + "hill radius must be 1..2000 and |height| <= 200");
    for (const auto& o : z.obstacles)
      if (o.radius <= 0.0f || o.radius > 100.0f) e.push_back(w + "obstacle radius must be (0, 100]");
    for (const auto& b : z.walls)
      if (b.hx <= 0.0f || b.hz <= 0.0f || b.hx > 500.0f || b.hz > 500.0f) e.push_back(w + "wall half extents must be (0, 500]");
    for (const auto& sp : z.spawns) {
      if (!c.find_mob(sp.mob)) e.push_back(w + "spawn references unknown mob '" + sp.mob + "'");
      if (sp.count < 1 || sp.count > 20) e.push_back(w + "spawn count for '" + sp.mob + "' must be 1..20");
      if (sp.radius < 0.0f || sp.radius > 100.0f) e.push_back(w + "spawn radius for '" + sp.mob + "' must be 0..100");
      if (!inside(z, sp.x, sp.z)) e.push_back(w + "spawn for '" + sp.mob + "' is outside bounds");
      if (!sp.rare.empty()) {
        const MobDef* r = c.find_mob(sp.rare);
        if (!r) e.push_back(w + "spawn for '" + sp.mob + "' has unknown rare '" + sp.rare + "'");
        else if (!r->named) e.push_back(w + "rare '" + sp.rare + "' must be marked named");
        if (sp.rare_chance <= 0.0f || sp.rare_chance > 1.0f) e.push_back(w + "rare_chance must be in (0, 1]");
      }
    }
    std::set<std::string> ids;
    for (const auto& en : z.entries) {
      if (!ids.insert(en.id).second) e.push_back(w + "entry '" + en.id + "' is defined twice");
      if (!inside(z, en.at.x, en.at.z)) e.push_back(w + "entry '" + en.id + "' is outside bounds");
      else if (blocked(z, en.at.x, en.at.z)) e.push_back(w + "entry '" + en.id + "' is inside a wall or obstacle");
      for (const auto& l : z.zone_lines)
        if (l.contains(en.at.x, en.at.z)) e.push_back(w + "entry '" + en.id + "' lies inside zone line '" + l.id + "' (players would bounce)");
    }
    ids.clear();
    for (const auto& l : z.zone_lines) {
      if (!ids.insert(l.id).second) e.push_back(w + "zone line '" + l.id + "' is defined twice");
      if (l.hx <= 0.0f || l.hz <= 0.0f || l.hx > 200.0f || l.hz > 200.0f) e.push_back(w + "zone line '" + l.id + "' half extents must be (0, 200]");
      if (!inside(z, l.x, l.z)) e.push_back(w + "zone line '" + l.id + "' is outside bounds");
      const ZoneDef* to = c.find_zone(l.to);
      if (!to) e.push_back(w + "zone line '" + l.id + "' leads to unknown zone '" + l.to + "'");
      else if (!to->find_entry(l.entry)) e.push_back(w + "zone line '" + l.id + "' arrives at unknown entry '" + l.entry + "' in '" + l.to + "'");
      if (l.contains(z.safe_spawn.x, z.safe_spawn.z)) e.push_back(w + "safe_spawn lies inside zone line '" + l.id + "'");
    }
  }
  // Factions: every mob's and service NPC's faction exists; hits and race adjustments name real things.
  std::set<std::string> f_ids;
  for (const auto& f : c.factions) {
    const std::string w = "faction '" + f.id + "': ";
    if (!f_ids.insert(f.id).second) e.push_back(w + "defined twice");
    if (f.base < kFactionMin || f.base > kFactionMax) e.push_back(w + "default must be within -2000..2000");
    for (const auto& [race, v] : f.race_mods) {
      if (!c.find_race(race)) e.push_back(w + "race_mods names unknown race '" + race + "'");
      if (v < -2000 || v > 2000) e.push_back(w + "race_mods must be within -2000..2000");
    }
    for (const auto& [other, v] : f.hits) {
      if (!c.find_faction(other)) e.push_back(w + "hits name unknown faction '" + other + "'");
      if (v < -500 || v > 500) e.push_back(w + "a hit must be within -500..500");
    }
  }
  if (!c.factions.empty()) {
    for (const auto& m : c.mobs)
      if (!c.find_faction(m.faction)) e.push_back("mob '" + m.id + "': unknown faction '" + m.faction + "'");
  }
  for (const auto& n : c.npcs)
    if (!n.faction.empty() && !c.find_faction(n.faction)) e.push_back("npc '" + n.id + "': unknown faction '" + n.faction + "'");
  // Mob spells: cast at players, never learned; specials name them and their adds.
  std::set<std::string> ms_ids;
  for (const auto& a : c.mob_spells) {
    const std::string w = "mob spell '" + a.id + "': ";
    if (!ms_ids.insert(a.id).second) e.push_back(w + "defined twice");
    if (c.find_spell(a.id) || c.find_ability(a.id) || c.find_item_effect(a.id)) e.push_back(w + "id is also a spell, ability or item effect id");
    if (a.target != TargetType::Enemy) e.push_back(w + "target must be enemy (the mob casts it at players)");
    if (a.effects.empty()) e.push_back(w + "needs at least one effect");
    for (const auto& ef : a.effects) {
      switch (ef.type) {
        case EffectType::Damage:
        case EffectType::Dot:
        case EffectType::Debuff:
        case EffectType::Slow:
        case EffectType::Snare:
        case EffectType::Root:
        case EffectType::Stun:
        case EffectType::Fear:
        case EffectType::Mez:
          break;
        default:
          e.push_back(w + "mob spells only damage, damage over time, debuff, slow, snare, root, stun, fear or mesmerize");
      }
      if (ef.type == EffectType::Damage && (ef.base <= 0.0f || ef.base > 100000.0f)) e.push_back(w + "damage must be 1..100000");
    }
  }
  for (const auto& m : c.mobs) {
    const std::string w = "mob '" + m.id + "': ";
    if (m.specials.size() > 8) e.push_back(w + "at most 8 specials");
    if (m.boss && !m.named) e.push_back(w + "a boss must also be named");
    for (const auto& s : m.specials) {
      const bool casts = s.type == MobSpecialType::Proc || s.type == MobSpecialType::Cast || s.type == MobSpecialType::Aoe;
      if (casts && !c.find_mob_spell(s.effect)) e.push_back(w + "special names unknown mob spell '" + s.effect + "'");
      if (!casts && !s.effect.empty()) e.push_back(w + "only proc, cast and aoe specials name an effect");
      if ((s.type == MobSpecialType::Proc || s.type == MobSpecialType::Rampage) && (s.chance <= 0.0f || s.chance > 1.0f))
        e.push_back(w + "proc and rampage need a chance in (0, 1]");
      if ((s.type == MobSpecialType::Cast || s.type == MobSpecialType::Aoe || s.type == MobSpecialType::Summon) && (s.every < 2.0f || s.every > 600.0f))
        e.push_back(w + "cast, aoe and summon need 'every' of 2..600 seconds");
      if ((s.type == MobSpecialType::Aoe || s.type == MobSpecialType::Summon) && (s.range <= 0.0f || s.range > 100.0f))
        e.push_back(w + "aoe and summon need a range of (0, 100]");
      if (s.type == MobSpecialType::Enrage && (s.below <= 0.0f || s.below >= 1.0f)) e.push_back(w + "enrage needs 'below' in (0, 1)");
      if ((s.type == MobSpecialType::Enrage || s.type == MobSpecialType::EnrageTimer) && (s.value <= 0.0f || s.value > 1000.0f))
        e.push_back(w + "enrage value (extra damage %) must be (0, 1000]");
      if (s.type == MobSpecialType::EnrageTimer && (s.seconds < 10.0f || s.seconds > 3600.0f)) e.push_back(w + "enrage_timer needs 'seconds' of 10..3600");
      if (s.type == MobSpecialType::Adds) {
        const MobDef* add = c.find_mob(s.mob);
        if (!add) e.push_back(w + "adds name unknown mob '" + s.mob + "'");
        else if (add->boss || std::any_of(add->specials.begin(), add->specials.end(), [](const MobSpecial& x) { return x.type == MobSpecialType::Adds; }))
          e.push_back(w + "adds cannot be bosses or call adds themselves");
        if (s.count < 1 || s.count > 8) e.push_back(w + "adds count must be 1..8");
        if (s.at.empty() || s.at.size() > 6) e.push_back(w + "adds need 1..6 health thresholds in 'at'");
        for (std::size_t k = 0; k < s.at.size(); ++k)
          if (s.at[k] <= 0.0f || s.at[k] >= 1.0f || (k > 0 && s.at[k] >= s.at[k - 1])) e.push_back(w + "adds 'at' must fall from below 1 toward 0");
      }
      if (s.message.size() > 200) e.push_back(w + "special message must be at most 200 chars");
    }
  }
  std::set<std::string> tp_ids;
  for (const auto& t : c.travel_points) {
    const std::string w = "travel point '" + t.id + "': ";
    if (!tp_ids.insert(t.id).second) e.push_back(w + "defined twice");
    if (c.find_city(t.id)) e.push_back(w + "shares its id with a city");
    const ZoneDef* z = c.find_zone(t.zone);
    if (!z) e.push_back(w + "unknown zone '" + t.zone + "'");
    else if (!inside(*z, t.at.x, t.at.z)) e.push_back(w + "outside its zone");
    else if (blocked(*z, t.at.x, t.at.z)) e.push_back(w + "inside a wall or obstacle");
  }
  // Every zone (but the test zone) can be reached from a starting city's zone by zone lines, and
  // every zone has a way back to one.
  std::set<std::string> starts;
  for (const auto& city : c.cities)
    if (const ZoneDef* z = c.find_zone(city.zone); z && z->kind != "test") starts.insert(z->id);
  if (starts.empty()) return;
  std::map<std::string, std::set<std::string>> fwd;
  std::map<std::string, std::set<std::string>> back;
  for (const auto& z : c.zones)
    for (const auto& l : z.zone_lines) {
      fwd[z.id].insert(l.to);
      back[l.to].insert(z.id);
    }
  auto reach = [&](std::map<std::string, std::set<std::string>>& g) {
    std::set<std::string> seen(starts.begin(), starts.end());
    std::deque<std::string> q(starts.begin(), starts.end());
    while (!q.empty()) {
      const std::string at = q.front();
      q.pop_front();
      for (const auto& n : g[at])
        if (seen.insert(n).second) q.push_back(n);
    }
    return seen;
  };
  const auto from_city = reach(fwd);
  const auto to_city = reach(back);
  for (const auto& z : c.zones) {
    if (z.kind == "test") continue;
    if (!from_city.contains(z.id)) e.push_back("zone '" + z.id + "' cannot be reached from any starting city by zone lines");
    if (!to_city.contains(z.id)) e.push_back("zone '" + z.id + "' has no way back to a starting city by zone lines");
  }
}

}  // namespace content_detail

}  // namespace ef::game
