// Guilds (Phase 7), pure and unit-tested: names, ranks and what each may do, and the roster in memory.
// The game server persists every change (db::Repository guild calls) and tells the members.
// Ranks are indices into SocialRules::guild_ranks: 0 is the leader, larger numbers are lower ranks.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "game/social_types.hpp"

namespace ef::game {

class ChatFilter;

struct GuildMember {
  std::int64_t char_id = 0;
  std::string name;
  int rank = 0;
  int level = 1;
  std::string class_id;
};

struct Guild {
  std::int64_t id = 0;  // database id
  std::string name;
  std::string motd;
  std::vector<GuildMember> members;
  GuildMember* member(std::int64_t char_id);
  const GuildMember* member(std::int64_t char_id) const;
  const GuildMember* member_named(std::string_view who) const;
  int leader_count() const;
};

enum class GuildError : std::uint8_t {
  Ok,
  NotInGuild,       // you are not in a guild
  NoPermission,     // your rank may not do that
  NotMember,        // they are not in your guild
  RankTooHigh,      // they rank as high as you (or higher)
  TopRank,          // they cannot be promoted further (only the leader outranks an officer)
  BottomRank,       // they are already the lowest rank
  LeaderMustPass,   // the leader must hand over leadership (or disband) before leaving
  Full,
  Self,             // that cannot be done to yourself
};

const char* guild_error_text(GuildError e);

bool guild_allows(const SocialRules& rules, int rank, GuildPerm perm);
// The rank's name ("Officer"), or "Member" for a rank the rules do not have.
const std::string& rank_name(const SocialRules& rules, int rank);
int lowest_rank(const SocialRules& rules);

// "  the   iron  vow " -> "The Iron Vow": trimmed, single spaces, each word capitalized.
std::string normalize_guild_name(std::string_view text);
// A player-facing problem with a (normalized) guild name, or nullopt: letters, spaces and apostrophes,
// starting with a letter, name_min..name_max long, and nothing the filter objects to.
std::optional<std::string> validate_guild_name(std::string_view normalized, const SocialRules& rules, const ChatFilter* filter);

// The actor (a member) acting on the target (a member of the same guild). These change `g` only on Ok.
GuildError guild_remove(Guild& g, const SocialRules& rules, std::int64_t actor, std::int64_t target);
GuildError guild_leave(Guild& g, std::int64_t who);  // the leader may leave only as the last member
GuildError guild_promote(Guild& g, const SocialRules& rules, std::int64_t actor, std::int64_t target);
GuildError guild_demote(Guild& g, const SocialRules& rules, std::int64_t actor, std::int64_t target);
// The leader hands leadership to `target`, becoming the rank below.
GuildError guild_pass_leadership(Guild& g, std::int64_t actor, std::int64_t target);
GuildError guild_set_motd(Guild& g, const SocialRules& rules, std::int64_t actor, std::string motd);
GuildError guild_can_invite(const Guild& g, const SocialRules& rules, std::int64_t actor);

struct GuildInvite {
  std::int64_t guild = 0;
  std::int64_t from = 0;
  std::string from_name;
  double expires_at = 0.0;
};

// Every guild in memory (loaded at start), with a character -> guild index and pending invitations.
class GuildRegistry {
 public:
  void load(std::vector<Guild> guilds);
  Guild* find(std::int64_t id);
  const Guild* find(std::int64_t id) const;
  Guild* of(std::int64_t char_id);
  const Guild* of(std::int64_t char_id) const;
  const Guild* by_name(std::string_view name) const;  // case-insensitive
  Guild& add(Guild g);
  void remove(std::int64_t id);
  // Keeps the character index in step after a roster change.
  void joined(std::int64_t guild, std::int64_t char_id);
  void left(std::int64_t char_id);
  std::size_t size() const { return guilds_.size(); }
  const std::map<std::int64_t, Guild>& all() const { return guilds_; }

  void invite(std::int64_t to, GuildInvite inv) { invites_[to] = std::move(inv); }
  std::optional<GuildInvite> take_invite(std::int64_t to);
  const GuildInvite* invite_for(std::int64_t to) const;
  std::vector<std::pair<std::int64_t, GuildInvite>> expire_invites(double now);
  void drop_invites_of(std::int64_t char_id);  // to or from them

 private:
  std::map<std::int64_t, Guild> guilds_;
  std::map<std::int64_t, std::int64_t> member_of_;
  std::map<std::int64_t, GuildInvite> invites_;
};

}  // namespace ef::game
