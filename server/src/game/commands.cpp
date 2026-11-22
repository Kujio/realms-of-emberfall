#include "game/commands.hpp"

#include <algorithm>
#include <charconv>

#include "game/content.hpp"

namespace ef::game {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string to_lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), lower);
  return out;
}

bool space(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && space(s.front())) s.remove_prefix(1);
  while (!s.empty() && space(s.back())) s.remove_suffix(1);
  return s;
}

std::optional<int> small_int(std::string_view s) {
  int v = 0;
  if (s.empty() || s.size() > 6) return std::nullopt;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
  return v;
}

// Splits on whitespace, keeping "quoted words" together (quotes removed).
std::vector<std::string> split_args(std::string_view s) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < s.size() && out.size() < 32) {
    while (i < s.size() && space(s[i])) ++i;
    if (i >= s.size()) break;
    std::string word;
    if (s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') word.push_back(s[i++]);
      if (i < s.size()) ++i;  // closing quote
    } else {
      while (i < s.size() && !space(s[i])) word.push_back(s[i++]);
    }
    out.push_back(std::move(word));
  }
  return out;
}

}  // namespace

const std::string& Command::arg(std::size_t i) const {
  static const std::string kEmpty;
  return i < args.size() ? args[i] : kEmpty;
}

std::string Command::rest_after(std::size_t n) const {
  // Walks the raw text the same way split_args does, skipping n arguments.
  std::string_view s = rest;
  std::size_t i = 0;
  for (std::size_t k = 0; k < n; ++k) {
    while (i < s.size() && space(s[i])) ++i;
    if (i >= s.size()) return {};
    if (s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') ++i;
      if (i < s.size()) ++i;
    } else {
      while (i < s.size() && !space(s[i])) ++i;
    }
  }
  return std::string(trim(s.substr(std::min(i, s.size()))));
}

std::optional<Command> parse_command(std::string_view text) {
  std::string_view s = trim(text);
  if (!s.empty() && s.front() == '/') s.remove_prefix(1);
  s = trim(s);
  if (s.empty()) return std::nullopt;
  std::size_t end = 0;
  while (end < s.size() && !space(s[end])) ++end;
  Command c;
  c.name = to_lower(s.substr(0, end));
  if (c.name.empty()) return std::nullopt;
  c.rest = std::string(trim(s.substr(end)));
  c.args = split_args(c.rest);
  return c;
}

std::optional<std::int64_t> parse_duration(std::string_view text) {
  const std::string t = to_lower(trim(text));
  if (t == "perm" || t == "permanent" || t == "forever") return kPermanent;
  if (t.empty()) return std::nullopt;
  std::int64_t unit = 60;  // a bare number is minutes
  std::string_view digits = t;
  switch (t.back()) {
    case 's': unit = 1; digits.remove_suffix(1); break;
    case 'm': unit = 60; digits.remove_suffix(1); break;
    case 'h': unit = 3600; digits.remove_suffix(1); break;
    case 'd': unit = 86400; digits.remove_suffix(1); break;
    case 'w': unit = 7LL * 86400; digits.remove_suffix(1); break;
    default: break;
  }
  std::int64_t v = 0;
  if (digits.empty() || digits.size() > 6) return std::nullopt;
  const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
  if (ec != std::errc() || p != digits.data() + digits.size() || v <= 0) return std::nullopt;
  const std::int64_t secs = v * unit;
  if (secs > 10LL * 365 * 86400) return std::nullopt;  // ten years: say "perm" instead
  return secs;
}

std::string describe_duration(std::int64_t seconds) {
  if (seconds == kPermanent) return "permanently";
  if (seconds <= 0) return "0 minutes";
  struct Unit {
    std::int64_t secs;
    const char* name;
  };
  const Unit units[] = {{7LL * 86400, "week"}, {86400, "day"}, {3600, "hour"}, {60, "minute"}, {1, "second"}};
  std::string out;
  int parts = 0;
  for (const auto& u : units) {
    if (seconds < u.secs || parts == 2) continue;
    const std::int64_t n = seconds / u.secs;
    seconds -= n * u.secs;
    if (!out.empty()) out += ' ';
    out += std::to_string(n) + " " + u.name + (n == 1 ? "" : "s");
    ++parts;
  }
  return out;
}

std::string WhoQuery::describe() const {
  std::string out = all ? "all" : "zone";
  auto add = [&](const std::string& s) {
    if (!s.empty()) out += " " + s;
  };
  add(name);
  add(class_id);
  add(race);
  if (min_level > 0 || max_level > 0) add(std::to_string(min_level) + (max_level > 0 ? "-" + std::to_string(max_level) : "+"));
  add(zone);
  if (!guild.empty()) add("guild \"" + guild + "\"");
  if (gm) add("gm");
  return out;
}

std::optional<std::string> parse_who(const std::vector<std::string>& args, const Content& content, WhoQuery& q) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string a = to_lower(args[i]);
    if (a.empty()) continue;
    if (a == "all") {
      q.all = true;
      continue;
    }
    if (a == "gm" || a == "gms") {
      q.gm = true;
      continue;
    }
    if (a == "guild") {
      if (i + 1 >= args.size()) return "Usage: /who guild \"Guild Name\"";
      q.guild = to_lower(args[++i]);
      continue;
    }
    if (const auto dash = a.find('-'); dash != std::string::npos) {
      const auto lo = small_int(std::string_view(a).substr(0, dash));
      const auto hi = small_int(std::string_view(a).substr(dash + 1));
      if (!lo || !hi || *lo < 1 || *hi < *lo) return "Levels are written like 10-20.";
      q.min_level = *lo;
      q.max_level = *hi;
      continue;
    }
    if (const auto n = small_int(a)) {
      if (*n < 1) return "Levels start at 1.";
      q.min_level = q.max_level = *n;
      continue;
    }
    bool matched = false;
    for (const auto& c : content.classes)
      if (to_lower(c.id) == a || to_lower(c.name) == a) {
        q.class_id = c.id;
        matched = true;
      }
    if (!matched)
      for (const auto& r : content.races)
        if (to_lower(r.id) == a || to_lower(r.name) == a) {
          q.race = r.id;
          matched = true;
        }
    if (!matched)
      for (const auto& z : content.zones)
        if (z.id == a) {
          q.zone = z.id;
          q.all = true;  // naming a zone looks there, wherever you are
          matched = true;
        }
    if (matched) continue;
    if (!q.name.empty()) return "Only one name can be searched for at a time.";
    q.name = a;
  }
  return std::nullopt;
}

bool who_matches(const WhoQuery& q, const WhoCandidate& c, std::string_view my_zone) {
  if (!q.all && c.zone != my_zone) return false;
  if (!q.zone.empty() && c.zone != q.zone) return false;
  if (!q.name.empty() && to_lower(c.name).find(q.name) == std::string::npos) return false;
  if (!q.class_id.empty() && c.class_id != q.class_id) return false;
  if (!q.race.empty() && c.race != q.race) return false;
  if (q.min_level > 0 && c.level < q.min_level) return false;
  if (q.max_level > 0 && c.level > q.max_level) return false;
  if (!q.guild.empty() && to_lower(c.guild).find(q.guild) == std::string::npos) return false;
  if (q.gm && !c.gm) return false;
  return true;
}

}  // namespace ef::game
