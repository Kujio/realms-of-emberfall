#include "game/dungeon_gen.hpp"

#include <algorithm>
#include <cmath>

namespace ef::game {

std::string check_dungeon_params(const DungeonParams& p) {
  if (p.rooms < 2 || p.rooms > 64) return "rooms must be 2..64";
  if (!(p.cell >= 1.0f && p.cell <= 16.0f)) return "cell must be 1..16";
  if (p.sector < 8 || p.sector > 32) return "sector must be 8..32";
  if (p.min_room < 3 || p.max_room < p.min_room || p.max_room > p.sector - 2) return "room sizes must satisfy 3 <= min_room <= max_room <= sector - 2";
  if (p.corridor < 1 || p.corridor > p.min_room) return "corridor must be 1..min_room";
  return {};
}

Vec3 DungeonLayout::room_center(int i) const {
  const auto& r = rooms.at(static_cast<std::size_t>(i));
  return {tiles.origin_x + (static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f) * tiles.cell, 0.0f,
          tiles.origin_z + (static_cast<float>(r.z) + static_cast<float>(r.h) * 0.5f) * tiles.cell};
}

float DungeonLayout::room_radius(int i) const {
  const auto& r = rooms.at(static_cast<std::size_t>(i));
  return std::max(0.0f, (static_cast<float>(std::min(r.w, r.h)) * 0.5f - 1.0f) * tiles.cell);
}

DungeonLayout generate_dungeon(const DungeonParams& p) {
  DungeonLayout d;
  if (!check_dungeon_params(p).empty()) return d;
  XorShift32 rng(p.seed);
  int g = 1;
  while (g * g < p.rooms) ++g;
  g += 1;  // room to branch
  const int S = p.sector;

  // Grow a tree of sectors from the bottom middle: each step attaches a free neighbour of a placed one.
  std::vector<int> owner(static_cast<std::size_t>(g * g), -1);  // sector -> room index
  struct Placed {
    int sx, sz;
  };
  std::vector<Placed> placed;
  const int start_x = g / 2;
  const int start_z = g - 1;
  placed.push_back({start_x, start_z});
  owner[static_cast<std::size_t>(start_z * g + start_x)] = 0;
  d.rooms.push_back({0, 0, 1, 1, 0, -1});
  static constexpr int kDx[4] = {0, 1, 0, -1};
  static constexpr int kDz[4] = {-1, 0, 1, 0};
  struct Cand {
    int from, sx, sz;
  };
  std::vector<Cand> cands;
  while (static_cast<int>(placed.size()) < p.rooms) {
    cands.clear();
    for (int i = 0; i < static_cast<int>(placed.size()); ++i)
      for (int k = 0; k < 4; ++k) {
        const int nx = placed[static_cast<std::size_t>(i)].sx + kDx[k];
        const int nz = placed[static_cast<std::size_t>(i)].sz + kDz[k];
        if (nx < 0 || nz < 0 || nx >= g || nz >= g) continue;
        if (owner[static_cast<std::size_t>(nz * g + nx)] >= 0) continue;
        cands.push_back({i, nx, nz});
      }
    if (cands.empty()) break;  // cannot happen with g*g > rooms, but never loop forever
    const Cand c = cands[static_cast<std::size_t>(rng.below(static_cast<int>(cands.size())))];
    owner[static_cast<std::size_t>(c.sz * g + c.sx)] = static_cast<int>(placed.size());
    placed.push_back({c.sx, c.sz});
    d.rooms.push_back({0, 0, 1, 1, d.rooms[static_cast<std::size_t>(c.from)].depth + 1, c.from});
  }
  // The boss room: the deepest (first on ties), and it fills its sector.
  for (int i = 1; i < static_cast<int>(d.rooms.size()); ++i)
    if (d.rooms[static_cast<std::size_t>(i)].depth > d.rooms[static_cast<std::size_t>(d.boss)].depth) d.boss = i;
  const int span = p.max_room - p.min_room + 1;
  for (int i = 0; i < static_cast<int>(d.rooms.size()); ++i) {
    auto& r = d.rooms[static_cast<std::size_t>(i)];
    const int ox = placed[static_cast<std::size_t>(i)].sx * S;
    const int oz = placed[static_cast<std::size_t>(i)].sz * S;
    if (i == d.boss) {
      r.w = S - 2;
      r.h = S - 2;
      r.x = ox + 1;
      r.z = oz + 1;
      continue;
    }
    r.w = p.min_room + rng.below(span);
    r.h = p.min_room + rng.below(span);
    r.x = ox + 1 + rng.below(S - 2 - r.w + 1);
    r.z = oz + 1 + rng.below(S - 2 - r.h + 1);
  }

  TileGrid& t = d.tiles;
  t.w = g * S;
  t.h = g * S;
  t.cell = p.cell;
  t.origin_x = -static_cast<float>(t.w) * p.cell * 0.5f;
  t.origin_z = -static_cast<float>(t.h) * p.cell * 0.5f;
  t.solid.assign(static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h), 1);
  auto carve = [&](int x0, int z0, int x1, int z1) {
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    for (int z = std::max(1, z0); z <= std::min(t.h - 2, z1); ++z)
      for (int x = std::max(1, x0); x <= std::min(t.w - 2, x1); ++x)
        t.solid[static_cast<std::size_t>(z) * static_cast<std::size_t>(t.w) + static_cast<std::size_t>(x)] = 0;
  };
  for (const auto& r : d.rooms) carve(r.x, r.z, r.x + r.w - 1, r.z + r.h - 1);
  // Corridors: an L from the parent's centre to the child's, horizontal or vertical leg first.
  const int cw = p.corridor;
  for (int i = 1; i < static_cast<int>(d.rooms.size()); ++i) {
    const auto& a = d.rooms[static_cast<std::size_t>(d.rooms[static_cast<std::size_t>(i)].parent)];
    const auto& b = d.rooms[static_cast<std::size_t>(i)];
    const int ax = a.x + a.w / 2;
    const int az = a.z + a.h / 2;
    const int bx = b.x + b.w / 2;
    const int bz = b.z + b.h / 2;
    if (rng.below(2) == 0) {
      carve(ax, az, bx, az + cw - 1);
      carve(bx, az, bx + cw - 1, bz);
    } else {
      carve(ax, az, ax + cw - 1, bz);
      carve(ax, bz, bx, bz + cw - 1);
    }
  }
  return d;
}

std::uint32_t dungeon_hash(const DungeonLayout& d) {
  LayoutHash h;
  h.add(d.tiles.w);
  h.add(d.tiles.h);
  for (std::uint8_t c : d.tiles.solid) h.add(c);
  h.add(static_cast<std::int64_t>(d.rooms.size()));
  for (const auto& r : d.rooms) {
    h.add(r.x);
    h.add(r.z);
    h.add(r.w);
    h.add(r.h);
    h.add(r.depth);
    h.add(r.parent);
  }
  h.add(d.boss);
  return h.value();
}

}  // namespace ef::game
