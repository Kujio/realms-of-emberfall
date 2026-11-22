#include "game/world_geom.hpp"

#include <algorithm>
#include <cmath>

namespace ef::game {

namespace {

constexpr float kEps = 1e-4f;
constexpr int kMaxBuckets = 512;  // per axis

void box_local(const WallBox& b, float x, float z, float& lx, float& lz) {
  const float c = std::cos(b.yaw);
  const float s = std::sin(b.yaw);
  const float dx = x - b.x;
  const float dz = z - b.z;
  lx = dx * c - dz * s;
  lz = dx * s + dz * c;
}

void box_world(const WallBox& b, float lx, float lz, float& x, float& z) {
  const float c = std::cos(b.yaw);
  const float s = std::sin(b.yaw);
  x = b.x + lx * c + lz * s;
  z = b.z - lx * s + lz * c;
}

float box_reach(const WallBox& b) { return std::sqrt(b.hx * b.hx + b.hz * b.hz); }

// Segment (in the box's frame) against an axis-aligned box: slab test.
bool segment_hits_aabb(float ax, float az, float bx, float bz, float hx, float hz) {
  float t0 = 0.0f;
  float t1 = 1.0f;
  const float d[2] = {bx - ax, bz - az};
  const float o[2] = {ax, az};
  const float h[2] = {hx, hz};
  for (int i = 0; i < 2; ++i) {
    if (std::fabs(d[i]) < 1e-9f) {
      if (o[i] < -h[i] || o[i] > h[i]) return false;
      continue;
    }
    float ta = (-h[i] - o[i]) / d[i];
    float tb = (h[i] - o[i]) / d[i];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

}  // namespace

float Terrain::height(float x, float z) const {
  float y = base;
  for (const auto& h : hills) {
    const float dx = x - h.x;
    const float dz = z - h.z;
    const float r2 = h.radius * h.radius;
    const float d2 = dx * dx + dz * dz;
    if (d2 >= r2) continue;
    const float t = 1.0f - d2 / r2;
    y += h.height * t * t;
  }
  return y;
}

bool TileGrid::solid_cell(int ix, int iz) const {
  if (ix < 0 || iz < 0 || ix >= w || iz >= h) return true;
  return solid[static_cast<std::size_t>(iz) * static_cast<std::size_t>(w) + static_cast<std::size_t>(ix)] != 0;
}

int TileGrid::cell_x(float x) const { return static_cast<int>(std::floor((x - origin_x) / cell)); }
int TileGrid::cell_z(float z) const { return static_cast<int>(std::floor((z - origin_z) / cell)); }

bool push_out_circle(float& x, float& z, float radius, const Obstacle& o) {
  const float dx = x - o.x;
  const float dz = z - o.z;
  const float need = o.radius + radius;
  const float d2 = dx * dx + dz * dz;
  if (d2 >= need * need) return false;
  const float d = std::sqrt(d2);
  if (d < kEps) {
    x = o.x + need;  // dead centre: out along +x
    return true;
  }
  x = o.x + dx / d * need;
  z = o.z + dz / d * need;
  return true;
}

bool push_out_box(float& x, float& z, float radius, const WallBox& b) {
  float lx = 0.0f;
  float lz = 0.0f;
  box_local(b, x, z, lx, lz);
  const float cx = std::clamp(lx, -b.hx, b.hx);
  const float cz = std::clamp(lz, -b.hz, b.hz);
  const float dx = lx - cx;
  const float dz = lz - cz;
  const float d2 = dx * dx + dz * dz;
  if (d2 >= radius * radius) return false;
  if (d2 > kEps * kEps) {
    const float d = std::sqrt(d2);
    lx = cx + dx / d * radius;
    lz = cz + dz / d * radius;
  } else {
    // The centre is inside the box: leave by the nearest face.
    const float px = b.hx - std::fabs(lx);
    const float pz = b.hz - std::fabs(lz);
    if (px < pz) lx = (lx < 0.0f ? -1.0f : 1.0f) * (b.hx + radius);
    else lz = (lz < 0.0f ? -1.0f : 1.0f) * (b.hz + radius);
  }
  box_world(b, lx, lz, x, z);
  return true;
}

CollisionMap::CollisionMap(std::vector<Obstacle> circles, std::vector<WallBox> boxes, TileGrid tiles)
    : circles_(std::move(circles)), boxes_(std::move(boxes)), tiles_(std::move(tiles)) {
  const std::size_t n = circles_.size() + boxes_.size();
  stamp_.assign(n, 0);
  if (n == 0) return;
  float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
  auto grow = [&](float x, float z, float r) {
    x0 = std::min(x0, x - r);
    z0 = std::min(z0, z - r);
    x1 = std::max(x1, x + r);
    z1 = std::max(z1, z + r);
  };
  for (const auto& c : circles_) grow(c.x, c.z, c.radius);
  for (const auto& b : boxes_) grow(b.x, b.z, box_reach(b));
  bx0_ = x0;
  bz0_ = z0;
  bucket_ = std::max(16.0f, std::max(x1 - x0, z1 - z0) / static_cast<float>(kMaxBuckets));
  bw_ = std::max(1, static_cast<int>(std::ceil((x1 - x0) / bucket_)) + 1);
  bh_ = std::max(1, static_cast<int>(std::ceil((z1 - z0) / bucket_)) + 1);
  buckets_.assign(static_cast<std::size_t>(bw_) * static_cast<std::size_t>(bh_), {});
  auto insert = [&](std::uint32_t idx, float x, float z, float r) {
    const int ix0 = static_cast<int>(std::floor((x - r - bx0_) / bucket_));
    const int iz0 = static_cast<int>(std::floor((z - r - bz0_) / bucket_));
    const int ix1 = static_cast<int>(std::floor((x + r - bx0_) / bucket_));
    const int iz1 = static_cast<int>(std::floor((z + r - bz0_) / bucket_));
    if ((ix1 - ix0 + 1) * (iz1 - iz0 + 1) > 64) {
      unbucketed_.push_back(idx);
      return;
    }
    for (int iz = std::max(0, iz0); iz <= std::min(bh_ - 1, iz1); ++iz)
      for (int ix = std::max(0, ix0); ix <= std::min(bw_ - 1, ix1); ++ix)
        buckets_[static_cast<std::size_t>(iz) * static_cast<std::size_t>(bw_) + static_cast<std::size_t>(ix)].push_back(idx);
  };
  for (std::size_t i = 0; i < circles_.size(); ++i) insert(static_cast<std::uint32_t>(i), circles_[i].x, circles_[i].z, circles_[i].radius);
  for (std::size_t i = 0; i < boxes_.size(); ++i)
    insert(static_cast<std::uint32_t>(circles_.size() + i), boxes_[i].x, boxes_[i].z, box_reach(boxes_[i]));
}

void CollisionMap::candidates(float x, float z, float reach) const {
  scratch_.clear();
  if (circles_.empty() && boxes_.empty()) return;
  if (++stamp_gen_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    stamp_gen_ = 1;
  }
  auto take = [&](std::uint32_t idx) {
    if (stamp_[idx] == stamp_gen_) return;
    stamp_[idx] = stamp_gen_;
    scratch_.push_back(idx);
  };
  for (std::uint32_t idx : unbucketed_) take(idx);
  const int ix0 = std::max(0, static_cast<int>(std::floor((x - reach - bx0_) / bucket_)));
  const int iz0 = std::max(0, static_cast<int>(std::floor((z - reach - bz0_) / bucket_)));
  const int ix1 = std::min(bw_ - 1, static_cast<int>(std::floor((x + reach - bx0_) / bucket_)));
  const int iz1 = std::min(bh_ - 1, static_cast<int>(std::floor((z + reach - bz0_) / bucket_)));
  for (int iz = iz0; iz <= iz1; ++iz)
    for (int ix = ix0; ix <= ix1; ++ix)
      for (std::uint32_t idx : buckets_[static_cast<std::size_t>(iz) * static_cast<std::size_t>(bw_) + static_cast<std::size_t>(ix)]) take(idx);
  // Same order as the client, which simply walks every shape: circles, then boxes, by index.
  std::sort(scratch_.begin(), scratch_.end());
}

Vec3 CollisionMap::resolve(Vec3 p, float radius) const {
  if (empty() || !std::isfinite(p.x) || !std::isfinite(p.z)) return p;
  const std::uint32_t nc = static_cast<std::uint32_t>(circles_.size());
  for (int pass = 0; pass < 2; ++pass) {
    candidates(p.x, p.z, radius + 2.0f);
    for (std::uint32_t idx : scratch_) {
      if (idx < nc) push_out_circle(p.x, p.z, radius, circles_[idx]);
      else push_out_box(p.x, p.z, radius, boxes_[idx - nc]);
    }
    if (!tiles_.empty()) {
      const int ix0 = tiles_.cell_x(p.x - radius);
      const int ix1 = tiles_.cell_x(p.x + radius);
      const int iz0 = tiles_.cell_z(p.z - radius);
      const int iz1 = tiles_.cell_z(p.z + radius);
      const float half = tiles_.cell * 0.5f;
      for (int iz = iz0; iz <= iz1; ++iz)
        for (int ix = ix0; ix <= ix1; ++ix) {
          if (!tiles_.solid_cell(ix, iz)) continue;
          const WallBox b{tiles_.origin_x + (static_cast<float>(ix) + 0.5f) * tiles_.cell,
                          tiles_.origin_z + (static_cast<float>(iz) + 0.5f) * tiles_.cell, half, half, 0.0f};
          push_out_box(p.x, p.z, radius, b);
        }
    }
  }
  return p;
}

bool CollisionMap::overlaps(float x, float z, float radius) const {
  if (empty()) return false;
  radius -= 1e-3f;  // touching is not overlapping (a pushed-out body sits exactly at the surface)
  const std::uint32_t nc = static_cast<std::uint32_t>(circles_.size());
  candidates(x, z, radius + 2.0f);
  for (std::uint32_t idx : scratch_) {
    float px = x;
    float pz = z;
    if (idx < nc ? push_out_circle(px, pz, radius, circles_[idx]) : push_out_box(px, pz, radius, boxes_[idx - nc])) return true;
  }
  if (!tiles_.empty()) {
    const float half = tiles_.cell * 0.5f;
    for (int iz = tiles_.cell_z(z - radius); iz <= tiles_.cell_z(z + radius); ++iz)
      for (int ix = tiles_.cell_x(x - radius); ix <= tiles_.cell_x(x + radius); ++ix) {
        if (!tiles_.solid_cell(ix, iz)) continue;
        float px = x;
        float pz = z;
        const WallBox b{tiles_.origin_x + (static_cast<float>(ix) + 0.5f) * tiles_.cell,
                        tiles_.origin_z + (static_cast<float>(iz) + 0.5f) * tiles_.cell, half, half, 0.0f};
        if (push_out_box(px, pz, radius, b)) return true;
      }
  }
  return false;
}

bool CollisionMap::segment_clear(const Vec3& a, const Vec3& b) const {
  if (empty()) return true;
  const float dx = b.x - a.x;
  const float dz = b.z - a.z;
  const float len_sq = dx * dx + dz * dz;
  for (const auto& o : circles_) {
    const float r2 = o.radius * o.radius;
    const auto inside = [&](const Vec3& p) { return (p.x - o.x) * (p.x - o.x) + (p.z - o.z) * (p.z - o.z) <= r2; };
    if (inside(a) || inside(b)) continue;
    float t = len_sq > 0.0f ? ((o.x - a.x) * dx + (o.z - a.z) * dz) / len_sq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = a.x + dx * t - o.x;
    const float cz = a.z + dz * t - o.z;
    if (cx * cx + cz * cz < r2) return false;
  }
  for (const auto& w : boxes_) {
    float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
    box_local(w, a.x, a.z, ax, az);
    box_local(w, b.x, b.z, bx, bz);
    if (segment_hits_aabb(ax, az, bx, bz, w.hx, w.hz)) return false;
  }
  if (!tiles_.empty()) {
    // Walk the segment in quarter-cell steps (cheap and plenty for spell ranges).
    const float len = std::sqrt(len_sq);
    const int steps = std::max(1, static_cast<int>(std::ceil(len / (tiles_.cell * 0.25f))));
    for (int i = 1; i < steps; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(steps);
      if (tiles_.solid_cell(tiles_.cell_x(a.x + dx * t), tiles_.cell_z(a.z + dz * t))) return false;
    }
  }
  return true;
}

void LayoutHash::add(std::int64_t v) {
  const auto u = static_cast<std::uint64_t>(v);
  for (int i = 0; i < 8; ++i) {
    h_ ^= static_cast<std::uint32_t>((u >> (8 * i)) & 0xFFu);
    h_ *= 16777619u;
  }
}

}  // namespace ef::game
