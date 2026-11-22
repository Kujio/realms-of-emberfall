#include "game/nav_grid.hpp"

#include <algorithm>
#include <cmath>
#include <functional>

namespace ef::game {

namespace {

constexpr std::size_t kMaxCells = 4'000'000;  // a 4 km square zone at 2 m

}  // namespace

void NavGrid::build(const Bounds& b, float cell, const CollisionMap& map, float clearance) {
  w_ = h_ = 0;
  blocked_.clear();
  if (map.empty() || !(cell > 0.1f)) return;
  cell_ = cell;
  x0_ = b.min_x;
  z0_ = b.min_z;
  int w = std::max(1, static_cast<int>(std::ceil((b.max_x - b.min_x) / cell)));
  int h = std::max(1, static_cast<int>(std::ceil((b.max_z - b.min_z) / cell)));
  while (static_cast<std::size_t>(w) * static_cast<std::size_t>(h) > kMaxCells) {  // coarsen huge zones
    cell_ *= 2.0f;
    w = std::max(1, static_cast<int>(std::ceil((b.max_x - b.min_x) / cell_)));
    h = std::max(1, static_cast<int>(std::ceil((b.max_z - b.min_z) / cell_)));
  }
  w_ = w;
  h_ = h;
  blocked_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0);
  auto mark_area = [&](float x0, float z0, float x1, float z1, const std::function<bool(float, float)>& hit) {
    const int cx0 = std::max(0, ix(x0));
    const int cz0 = std::max(0, iz(z0));
    const int cx1 = std::min(w_ - 1, ix(x1));
    const int cz1 = std::min(h_ - 1, iz(z1));
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx) {
        auto& cellv = blocked_[static_cast<std::size_t>(cz) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(cx)];
        if (cellv) continue;
        const Vec3 c = center(cx, cz);
        if (hit(c.x, c.z)) cellv = 1;
      }
  };
  for (const auto& o : map.circles()) {
    const float r = o.radius + clearance;
    mark_area(o.x - r, o.z - r, o.x + r, o.z + r, [&](float x, float z) {
      float px = x, pz = z;
      return push_out_circle(px, pz, clearance, o);
    });
  }
  for (const auto& bx : map.boxes()) {
    const float r = std::sqrt(bx.hx * bx.hx + bx.hz * bx.hz) + clearance;
    mark_area(bx.x - r, bx.z - r, bx.x + r, bx.z + r, [&](float x, float z) {
      float px = x, pz = z;
      return push_out_box(px, pz, clearance, bx);
    });
  }
  if (!map.tiles().empty()) {
    const TileGrid& t = map.tiles();
    const float half = t.cell * 0.5f;
    mark_area(b.min_x, b.min_z, b.max_x, b.max_z, [&](float x, float z) {
      for (int tz = t.cell_z(z - clearance); tz <= t.cell_z(z + clearance); ++tz)
        for (int tx = t.cell_x(x - clearance); tx <= t.cell_x(x + clearance); ++tx) {
          if (!t.solid_cell(tx, tz)) continue;
          float px = x, pz = z;
          const WallBox wb{t.origin_x + (static_cast<float>(tx) + 0.5f) * t.cell, t.origin_z + (static_cast<float>(tz) + 0.5f) * t.cell, half, half, 0.0f};
          if (push_out_box(px, pz, clearance, wb)) return true;
        }
      return false;
    });
  }
}

bool NavGrid::walkable_cell(int cx, int cz) const {
  if (cx < 0 || cz < 0 || cx >= w_ || cz >= h_) return false;
  return blocked_[static_cast<std::size_t>(cz) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(cx)] == 0;
}

bool NavGrid::walkable(float x, float z) const { return empty() || walkable_cell(ix(x), iz(z)); }

std::size_t NavGrid::blocked_count() const { return static_cast<std::size_t>(std::count(blocked_.begin(), blocked_.end(), 1)); }

bool NavGrid::line_walkable(const Vec3& a, const Vec3& b) const {
  if (empty()) return true;
  const float dx = b.x - a.x;
  const float dz = b.z - a.z;
  const float len = std::sqrt(dx * dx + dz * dz);
  const int steps = std::max(1, static_cast<int>(std::ceil(len / (cell_ * 0.5f))));
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    if (!walkable(a.x + dx * t, a.z + dz * t)) return false;
  }
  return true;
}

bool NavGrid::find_path(const Vec3& from, const Vec3& to, std::vector<Vec3>& out, int max_expansions) const {
  out.clear();
  if (empty()) {
    out.push_back(to);
    return true;
  }
  const int sx = std::clamp(ix(from.x), 0, w_ - 1);
  const int sz = std::clamp(iz(from.z), 0, h_ - 1);
  // The search window: centred between start and goal, clamped to the grid.
  const int mx = (sx + std::clamp(ix(to.x), 0, w_ - 1)) / 2;
  const int mz = (sz + std::clamp(iz(to.z), 0, h_ - 1)) / 2;
  const int win_w = std::min(kWindow, w_);
  const int win_h = std::min(kWindow, h_);
  const int wx0 = std::clamp(mx - win_w / 2, 0, w_ - win_w);
  const int wz0 = std::clamp(mz - win_h / 2, 0, h_ - win_h);
  auto in_win = [&](int cx, int cz) { return cx >= wx0 && cz >= wz0 && cx < wx0 + win_w && cz < wz0 + win_h; };
  if (!in_win(sx, sz)) return false;
  const int gx = std::clamp(ix(to.x), wx0, wx0 + win_w - 1);
  const int gz = std::clamp(iz(to.z), wz0, wz0 + win_h - 1);
  const bool goal_exact = gx == ix(to.x) && gz == iz(to.z) && walkable_cell(gx, gz);

  const std::size_t n = static_cast<std::size_t>(win_w) * static_cast<std::size_t>(win_h);
  if (g_.size() < n) {
    g_.assign(n, 0.0f);
    parent_.assign(n, -1);
    seen_.assign(n, 0);
  }
  if (++gen_ == 0) {
    std::fill(seen_.begin(), seen_.end(), 0);
    gen_ = 1;
  }
  auto local = [&](int cx, int cz) { return (cz - wz0) * win_w + (cx - wx0); };
  auto heur = [&](int cx, int cz) {
    const float ddx = static_cast<float>(std::abs(cx - gx));
    const float ddz = static_cast<float>(std::abs(cz - gz));
    return std::max(ddx, ddz) + 0.41421356f * std::min(ddx, ddz);
  };
  heap_.clear();
  const int s = local(sx, sz);
  g_[static_cast<std::size_t>(s)] = 0.0f;
  parent_[static_cast<std::size_t>(s)] = -1;
  seen_[static_cast<std::size_t>(s)] = gen_;
  // Min-heap on f via std::greater over (f, cell).
  auto push = [&](float f, std::int32_t c) {
    heap_.emplace_back(f, c);
    std::push_heap(heap_.begin(), heap_.end(), std::greater<>());
  };
  push(heur(sx, sz), s);
  int best = s;
  float best_h = heur(sx, sz);
  int expansions = 0;
  bool reached = false;
  static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static constexpr int kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
  while (!heap_.empty() && expansions < max_expansions) {
    std::pop_heap(heap_.begin(), heap_.end(), std::greater<>());
    const auto [f, c] = heap_.back();
    heap_.pop_back();
    const int cx = wx0 + c % win_w;
    const int cz = wz0 + c / win_w;
    const float gc = g_[static_cast<std::size_t>(c)];
    if (f > gc + heur(cx, cz) + 1e-3f) continue;  // stale entry
    ++expansions;
    const float hc = heur(cx, cz);
    if (hc < best_h) {
      best_h = hc;
      best = c;
    }
    if (cx == gx && cz == gz) {
      reached = true;
      best = c;
      break;
    }
    for (int k = 0; k < 8; ++k) {
      const int nx = cx + kDx[k];
      const int nz = cz + kDz[k];
      if (!in_win(nx, nz) || !walkable_cell(nx, nz)) continue;
      if (k >= 4 && (!walkable_cell(cx + kDx[k], cz) || !walkable_cell(cx, cz + kDz[k]))) continue;  // no corner cutting
      const int nc = local(nx, nz);
      const float ng = gc + (k >= 4 ? 1.41421356f : 1.0f);
      if (seen_[static_cast<std::size_t>(nc)] == gen_ && g_[static_cast<std::size_t>(nc)] <= ng) continue;
      seen_[static_cast<std::size_t>(nc)] = gen_;
      g_[static_cast<std::size_t>(nc)] = ng;
      parent_[static_cast<std::size_t>(nc)] = c;
      push(ng + heur(nx, nz), nc);
    }
  }
  if (best == s) return false;
  cells_.clear();
  for (int c = best; c != -1; c = parent_[static_cast<std::size_t>(c)]) cells_.push_back(c);
  std::reverse(cells_.begin(), cells_.end());
  // String pulling: from each anchor, jump to the farthest cell still in a straight walkable line.
  Vec3 anchor = from;
  std::size_t i = 1;
  while (i < cells_.size()) {
    std::size_t far = i;
    for (std::size_t j = cells_.size() - 1; j > i; --j) {
      const Vec3 cj = center(wx0 + cells_[j] % win_w, wz0 + cells_[j] / win_w);
      if (line_walkable(anchor, cj)) {
        far = j;
        break;
      }
    }
    Vec3 pt = center(wx0 + cells_[far] % win_w, wz0 + cells_[far] / win_w);
    if (far == cells_.size() - 1 && reached && goal_exact) pt = to;
    out.push_back(pt);
    anchor = pt;
    i = far + 1;
  }
  return !out.empty();
}

}  // namespace ef::game
