#include "game/lockouts.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace ef::game {

namespace {
constexpr std::size_t kMaxEntries = 64;
constexpr std::size_t kMaxId = 64;
}  // namespace

std::int64_t Lockouts::until(std::string_view boss, std::int64_t now) const {
  for (const auto& [id, at] : entries_)
    if (id == boss) return at > now ? at : 0;
  return 0;
}

bool Lockouts::lock(std::string_view boss, std::int64_t until, std::int64_t now) {
  prune(now);
  if (boss.empty() || boss.size() > kMaxId || until <= now || locked(boss, now)) return false;
  if (entries_.size() >= kMaxEntries) return false;
  entries_.emplace_back(std::string(boss), until);
  return true;
}

void Lockouts::prune(std::int64_t now) {
  std::erase_if(entries_, [now](const auto& e) { return e.second <= now; });
}

std::vector<std::pair<std::string, std::int64_t>> Lockouts::active(std::int64_t now) const {
  std::vector<std::pair<std::string, std::int64_t>> out;
  for (const auto& e : entries_)
    if (e.second > now) out.push_back(e);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
  return out;
}

std::string Lockouts::to_json() const {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, at] : entries_) j[id] = at;
  return j.dump();
}

Lockouts Lockouts::from_json(std::string_view text) {
  Lockouts out;
  const auto j = nlohmann::json::parse(text, nullptr, false);
  if (!j.is_object()) return out;
  for (const auto& [k, v] : j.items()) {
    if (out.entries_.size() >= kMaxEntries) break;
    if (k.empty() || k.size() > kMaxId || !v.is_number_integer()) continue;
    const auto at = v.get<std::int64_t>();
    if (at > 0) out.entries_.emplace_back(k, at);
  }
  return out;
}

std::string format_span(std::int64_t seconds) {
  const std::int64_t minutes = std::max<std::int64_t>(1, (seconds + 59) / 60);
  const std::int64_t d = minutes / (24 * 60);
  const std::int64_t h = (minutes / 60) % 24;
  const std::int64_t m = minutes % 60;
  if (d > 0) return std::to_string(d) + "d" + (h > 0 ? " " + std::to_string(h) + "h" : std::string());
  if (h > 0) return std::to_string(h) + "h" + (m > 0 ? " " + std::to_string(m) + "m" : std::string());
  return std::to_string(m) + "m";
}

}  // namespace ef::game
