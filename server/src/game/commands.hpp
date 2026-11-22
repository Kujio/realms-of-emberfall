// Slash commands (Phase 7), pure and unit-tested: splitting a typed line into a command and its
// arguments, durations for bans and mutes, and /who filters. The game server dispatches the result.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ef::game {

struct Content;

struct Command {
  std::string name;               // lower case, without the slash ("who", "gm")
  std::vector<std::string> args;  // whitespace-separated; "double quotes" group words
  std::string rest;               // everything after the name, trimmed (free text: a MOTD, a reason)
  // The text after the first `n` arguments, trimmed ("" if there is none).
  std::string rest_after(std::size_t n) const;
  const std::string& arg(std::size_t i) const;  // "" when missing
};

// nullopt for an empty line. A leading '/' is optional.
std::optional<Command> parse_command(std::string_view text);

// Durations for bans and mutes, in seconds: "90" (minutes), "30m", "12h", "7d", "2w", or "perm" /
// "permanent" / "forever" (kPermanent). nullopt when it cannot be read or is not positive.
inline constexpr std::int64_t kPermanent = -1;
std::optional<std::int64_t> parse_duration(std::string_view text);
// "2 days 3 hours", "15 minutes", "permanently" for messages.
std::string describe_duration(std::int64_t seconds);

// /who [all] [name] [class] [race] [N | N-M] [zone] [guild "Name"] [gm]
struct WhoQuery {
  bool all = false;  // everyone online, not just your zone
  std::string name;  // name fragment, lower case
  std::string class_id;
  std::string race;
  int min_level = 0;
  int max_level = 0;  // 0 = no upper bound
  std::string zone;   // zone id
  std::string guild;  // lower case, matched as a fragment
  bool gm = false;
  std::string describe() const;  // what the results window shows as the query
};
// Words that name a class, race or zone (by id or display name) become those filters; numbers
// become levels; "guild X" a guild; the first other word a name fragment. Returns an error message
// for something it cannot read.
std::optional<std::string> parse_who(const std::vector<std::string>& args, const Content& content, WhoQuery& out);

struct WhoCandidate {
  std::string name;
  int level = 1;
  std::string class_id;
  std::string race;
  std::string zone;
  std::string guild;
  bool gm = false;
};
bool who_matches(const WhoQuery& q, const WhoCandidate& c, std::string_view my_zone);

}  // namespace ef::game
