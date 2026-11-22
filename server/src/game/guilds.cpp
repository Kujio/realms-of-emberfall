#include "game/guilds.hpp"

#include <algorithm>

#include "game/chat.hpp"

namespace ef::game {

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
bool letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

}  // namespace

GuildMember* Guild::member(std::int64_t char_id) {
  for (auto& m : members)
    if (m.char_id == char_id) return &m;
  return nullptr;
}

const GuildMember* Guild::member(std::int64_t char_id) const {
  for (const auto& m : members)
    if (m.char_id == char_id) return &m;
  return nullptr;
}

const GuildMember* Guild::member_named(std::string_view who) const {
  for (const auto& m : members)
    if (iequals(m.name, who)) return &m;
  return nullptr;
}

int Guild::leader_count() const {
  return static_cast<int>(std::count_if(members.begin(), members.end(), [](const GuildMember& m) { return m.rank == 0; }));
}

const char* guild_error_text(GuildError e) {
  switch (e) {
    case GuildError::Ok: return "";
    case GuildError::NotInGuild: return "You are not in a guild.";
    case GuildError::NoPermission: return "Your guild rank does not allow that.";
    case GuildError::NotMember: return "They are not in your guild.";
    case GuildError::RankTooHigh: return "They rank as high as you do.";
    case GuildError::TopRank: return "You cannot promote them any higher.";
    case GuildError::BottomRank: return "They already hold the lowest rank.";
    case GuildError::LeaderMustPass: return "Pass leadership to another member first (/guildleader NAME), or disband the guild.";
    case GuildError::Full: return "The guild is full.";
    case GuildError::Self: return "You cannot do that to yourself.";
  }
  return "";
}

bool guild_allows(const SocialRules& rules, int rank, GuildPerm perm) {
  if (rank < 0 || rank >= static_cast<int>(rules.guild_ranks.size())) return false;
  return rules.guild_ranks[static_cast<std::size_t>(rank)].has(perm);
}

const std::string& rank_name(const SocialRules& rules, int rank) {
  static const std::string kMember = "Member";
  if (rank < 0 || rank >= static_cast<int>(rules.guild_ranks.size())) return kMember;
  return rules.guild_ranks[static_cast<std::size_t>(rank)].name;
}

int lowest_rank(const SocialRules& rules) { return std::max(0, static_cast<int>(rules.guild_ranks.size()) - 1); }

std::string normalize_guild_name(std::string_view text) {
  std::string out;
  bool space = false;
  bool word_start = true;
  for (char c : text) {
    if (c == ' ' || c == '\t') {
      space = !out.empty();
      word_start = true;
      continue;
    }
    if (space) out.push_back(' ');
    space = false;
    out.push_back(word_start ? upper(c) : lower(c));
    word_start = false;
  }
  return out;
}

std::optional<std::string> validate_guild_name(std::string_view n, const SocialRules& rules, const ChatFilter* filter) {
  const auto len = static_cast<int>(n.size());
  if (len < rules.guild_name_min || len > rules.guild_name_max)
    return "A guild name must be " + std::to_string(rules.guild_name_min) + "-" + std::to_string(rules.guild_name_max) + " characters.";
  if (!letter(n.front())) return "A guild name must start with a letter.";
  for (std::size_t i = 0; i < n.size(); ++i) {
    const char c = n[i];
    if (!letter(c) && c != ' ' && c != '\'') return "A guild name may only use letters, spaces and apostrophes.";
    if (c == '\'' && i + 1 < n.size() && n[i + 1] == '\'') return "A guild name may not repeat an apostrophe.";
  }
  if (filter && filter->offensive(n)) return "That guild name is not allowed.";
  return std::nullopt;
}

GuildError guild_remove(Guild& g, const SocialRules& rules, std::int64_t actor, std::int64_t target) {
  const GuildMember* a = g.member(actor);
  if (!a) return GuildError::NotInGuild;
  if (actor == target) return GuildError::Self;
  const GuildMember* t = g.member(target);
  if (!t) return GuildError::NotMember;
  if (!guild_allows(rules, a->rank, GuildPerm::Remove)) return GuildError::NoPermission;
  if (t->rank <= a->rank) return GuildError::RankTooHigh;
  std::erase_if(g.members, [&](const GuildMember& m) { return m.char_id == target; });
  return GuildError::Ok;
}

GuildError guild_leave(Guild& g, std::int64_t who) {
  const GuildMember* m = g.member(who);
  if (!m) return GuildError::NotInGuild;
  if (m->rank == 0 && g.members.size() > 1 && g.leader_count() <= 1) return GuildError::LeaderMustPass;
  std::erase_if(g.members, [&](const GuildMember& x) { return x.char_id == who; });
  return GuildError::Ok;
}

GuildError guild_promote(Guild& g, const SocialRules& rules, std::int64_t actor, std::int64_t target) {
  const GuildMember* a = g.member(actor);
  if (!a) return GuildError::NotInGuild;
  if (actor == target) return GuildError::Self;
  GuildMember* t = g.member(target);
  if (!t) return GuildError::NotMember;
  if (!guild_allows(rules, a->rank, GuildPerm::Promote)) return GuildError::NoPermission;
  if (t->rank <= a->rank) return GuildError::RankTooHigh;
  // Nobody is promoted to their promoter's own rank, nor to leader (that is /guildleader).
  if (t->rank - 1 <= a->rank || t->rank - 1 < 1) return GuildError::TopRank;
  --t->rank;
  return GuildError::Ok;
}

GuildError guild_demote(Guild& g, const SocialRules& rules, std::int64_t actor, std::int64_t target) {
  const GuildMember* a = g.member(actor);
  if (!a) return GuildError::NotInGuild;
  if (actor == target) return GuildError::Self;
  GuildMember* t = g.member(target);
  if (!t) return GuildError::NotMember;
  if (!guild_allows(rules, a->rank, GuildPerm::Demote)) return GuildError::NoPermission;
  if (t->rank <= a->rank) return GuildError::RankTooHigh;
  if (t->rank >= lowest_rank(rules)) return GuildError::BottomRank;
  ++t->rank;
  return GuildError::Ok;
}

GuildError guild_pass_leadership(Guild& g, std::int64_t actor, std::int64_t target) {
  GuildMember* a = g.member(actor);
  if (!a) return GuildError::NotInGuild;
  if (a->rank != 0) return GuildError::NoPermission;
  if (actor == target) return GuildError::Self;
  GuildMember* t = g.member(target);
  if (!t) return GuildError::NotMember;
  t->rank = 0;
  a->rank = 1;
  return GuildError::Ok;
}

GuildError guild_set_motd(Guild& g, const SocialRules& rules, std::int64_t actor, std::string motd) {
  const GuildMember* a = g.member(actor);
  if (!a) return GuildError::NotInGuild;
  if (!guild_allows(rules, a->rank, GuildPerm::Motd)) return GuildError::NoPermission;
  g.motd = std::move(motd);
  return GuildError::Ok;
}

GuildError guild_can_invite(const Guild& g, const SocialRules& rules, std::int64_t actor) {
  const GuildMember* a = g.member(actor);
  if (!a) return GuildError::NotInGuild;
  if (!guild_allows(rules, a->rank, GuildPerm::Invite)) return GuildError::NoPermission;
  if (static_cast<int>(g.members.size()) >= rules.guild_max_members) return GuildError::Full;
  return GuildError::Ok;
}

// ---------------------------------------------------------------- registry

void GuildRegistry::load(std::vector<Guild> guilds) {
  guilds_.clear();
  member_of_.clear();
  for (auto& g : guilds) add(std::move(g));
}

Guild* GuildRegistry::find(std::int64_t id) {
  auto it = guilds_.find(id);
  return it == guilds_.end() ? nullptr : &it->second;
}

const Guild* GuildRegistry::find(std::int64_t id) const {
  auto it = guilds_.find(id);
  return it == guilds_.end() ? nullptr : &it->second;
}

Guild* GuildRegistry::of(std::int64_t char_id) {
  auto it = member_of_.find(char_id);
  return it == member_of_.end() ? nullptr : find(it->second);
}

const Guild* GuildRegistry::of(std::int64_t char_id) const {
  auto it = member_of_.find(char_id);
  return it == member_of_.end() ? nullptr : find(it->second);
}

const Guild* GuildRegistry::by_name(std::string_view name) const {
  for (const auto& [id, g] : guilds_)
    if (iequals(g.name, name)) return &g;
  return nullptr;
}

Guild& GuildRegistry::add(Guild g) {
  const std::int64_t id = g.id;
  auto& slot = guilds_[id];
  slot = std::move(g);
  for (const auto& m : slot.members) member_of_[m.char_id] = id;
  return slot;
}

void GuildRegistry::remove(std::int64_t id) {
  auto it = guilds_.find(id);
  if (it == guilds_.end()) return;
  for (const auto& m : it->second.members) member_of_.erase(m.char_id);
  std::erase_if(invites_, [&](const auto& kv) { return kv.second.guild == id; });
  guilds_.erase(it);
}

void GuildRegistry::joined(std::int64_t guild, std::int64_t char_id) { member_of_[char_id] = guild; }

void GuildRegistry::left(std::int64_t char_id) { member_of_.erase(char_id); }

std::optional<GuildInvite> GuildRegistry::take_invite(std::int64_t to) {
  auto it = invites_.find(to);
  if (it == invites_.end()) return std::nullopt;
  GuildInvite inv = std::move(it->second);
  invites_.erase(it);
  return inv;
}

const GuildInvite* GuildRegistry::invite_for(std::int64_t to) const {
  auto it = invites_.find(to);
  return it == invites_.end() ? nullptr : &it->second;
}

std::vector<std::pair<std::int64_t, GuildInvite>> GuildRegistry::expire_invites(double now) {
  std::vector<std::pair<std::int64_t, GuildInvite>> out;
  for (auto it = invites_.begin(); it != invites_.end();) {
    if (it->second.expires_at <= now) {
      out.emplace_back(it->first, std::move(it->second));
      it = invites_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

void GuildRegistry::drop_invites_of(std::int64_t char_id) {
  invites_.erase(char_id);
  std::erase_if(invites_, [&](const auto& kv) { return kv.second.from == char_id; });
}

}  // namespace ef::game
