// Social content (Phase 7): content/social.json parsing and checks, plus the channel and permission
// vocabulary from social_types.hpp.
#include <algorithm>
#include <cctype>
#include <set>

#include "game/content.hpp"
#include "game/content_parser.hpp"

namespace ef::game {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

constexpr std::uint16_t kAllPerms = 127;

// The channels players speak on, as social.json's chat.channels names them (each needs a rate).
constexpr const char* kRateChannels[] = {"say", "shout", "ooc", "tell", "group", "guild", "emote", "raid"};

}  // namespace

SocialRules::SocialRules() {
  rates.fill({1.0, 5.0});
  rates[chat_id(ChatChannel::Shout)] = {0.2, 2.0};
  rates[chat_id(ChatChannel::Ooc)] = {0.2, 2.0};
  rates[chat_id(ChatChannel::Emote)] = {0.5, 3.0};
  guild_ranks = {{"leader", "Leader", kAllPerms},
                 {"officer", "Officer", static_cast<std::uint16_t>(kAllPerms & ~static_cast<std::uint16_t>(GuildPerm::Disband))},
                 {"member", "Member", static_cast<std::uint16_t>(static_cast<std::uint16_t>(GuildPerm::Invite) | static_cast<std::uint16_t>(GuildPerm::Speak))},
                 {"recruit", "Recruit", static_cast<std::uint16_t>(GuildPerm::Speak)}};
}

std::optional<ChatChannel> sendable_channel(std::uint8_t id) {
  switch (static_cast<ChatChannel>(id)) {
    case ChatChannel::Say:
    case ChatChannel::Shout:
    case ChatChannel::Ooc:
    case ChatChannel::Tell:
    case ChatChannel::Group:
    case ChatChannel::Guild:
    case ChatChannel::Emote:
    case ChatChannel::Raid:
      return id < kChatChannelCount ? std::optional<ChatChannel>(static_cast<ChatChannel>(id)) : std::nullopt;
    default:
      return std::nullopt;
  }
}

bool mutable_channel(ChatChannel c) {
  return c == ChatChannel::Shout || c == ChatChannel::Ooc || c == ChatChannel::Emote || c == ChatChannel::Say || c == ChatChannel::Guild;
}

std::optional<ChatChannel> channel_by_name(std::string_view name) {
  const std::string n = lower(name);
  if (n == "say" || n == "s") return ChatChannel::Say;
  if (n == "shout" || n == "sh") return ChatChannel::Shout;
  if (n == "ooc") return ChatChannel::Ooc;
  if (n == "tell" || n == "t") return ChatChannel::Tell;
  if (n == "group" || n == "g" || n == "gsay") return ChatChannel::Group;
  if (n == "guild" || n == "gu" || n == "guildsay") return ChatChannel::Guild;
  if (n == "emote" || n == "em" || n == "me") return ChatChannel::Emote;
  if (n == "gm") return ChatChannel::Gm;
  if (n == "raid" || n == "rs" || n == "rsay") return ChatChannel::Raid;
  return std::nullopt;
}

const char* channel_name(ChatChannel c) {
  switch (c) {
    case ChatChannel::Say: return "say";
    case ChatChannel::System: return "system";
    case ChatChannel::Combat: return "combat";
    case ChatChannel::Shout: return "shout";
    case ChatChannel::Ooc: return "ooc";
    case ChatChannel::Tell:
    case ChatChannel::TellEcho: return "tell";
    case ChatChannel::Group: return "group";
    case ChatChannel::Guild: return "guild";
    case ChatChannel::Emote: return "emote";
    case ChatChannel::Gm: return "gm";
    case ChatChannel::Raid: return "raid";
  }
  return "?";
}

std::optional<GuildPerm> guild_perm_by_name(std::string_view name) {
  if (name == "invite") return GuildPerm::Invite;
  if (name == "remove") return GuildPerm::Remove;
  if (name == "promote") return GuildPerm::Promote;
  if (name == "demote") return GuildPerm::Demote;
  if (name == "motd") return GuildPerm::Motd;
  if (name == "speak") return GuildPerm::Speak;
  if (name == "disband") return GuildPerm::Disband;
  return std::nullopt;
}

namespace content_detail {

void parse_social(Parser& p, const ContentDocs& docs, Content& c) {
  SocialRules& s = c.social;
  const json& doc = docs.social;
  if (doc.is_null()) return;  // partial test content: the defaults
  if (!doc.is_object()) {
    p.error("social.json: must be a JSON object");
    return;
  }
  auto object = [&](const json& parent, const char* key, const std::string& where) -> const json* {
    if (!parent.contains(key) || !parent.at(key).is_object()) {
      p.error(where + ": missing object '" + key + "'");
      return nullptr;
    }
    return &parent.at(key);
  };
  if (const json* g = object(doc, "group", "social.json")) {
    const std::string w = "social.group";
    s.group_max = p.integer(*g, "max_members", w, s.group_max);
    s.group_invite_seconds = p.num(*g, "invite_seconds", w, s.group_invite_seconds);
    s.group_status_seconds = p.num(*g, "status_seconds", w, s.group_status_seconds);
    s.group_xp_range = p.num(*g, "xp_range", w, s.group_xp_range);
    s.group_level_window = p.integer(*g, "level_window", w, s.group_level_window);
    s.group_level_fraction = p.num(*g, "level_fraction", w, s.group_level_fraction);
    s.group_offline_grace_seconds = p.num(*g, "offline_grace_seconds", w, s.group_offline_grace_seconds);
  }
  if (const json* r = object(doc, "raid", "social.json")) {
    const std::string w = "social.raid";
    s.raid_max_groups = p.integer(*r, "max_groups", w, s.raid_max_groups);
    s.raid_invite_seconds = p.num(*r, "invite_seconds", w, s.raid_invite_seconds);
    s.raid_lockout_hours = p.num(*r, "lockout_hours", w, s.raid_lockout_hours);
    s.raid_evict_seconds = p.num(*r, "evict_seconds", w, s.raid_evict_seconds);
  }
  if (const json* ch = object(doc, "chat", "social.json")) {
    const std::string w = "social.chat";
    if (const json* rates = object(*ch, "channels", w)) {
      for (const char* name : kRateChannels) {
        const auto id = channel_by_name(name);
        const std::string wc = w + ".channels." + name;
        if (!rates->contains(name) || !rates->at(name).is_object()) {
          p.error(wc + ": missing (every channel players speak on needs a rate)");
          continue;
        }
        if (!id) continue;  // every name in the list above is a channel
        ChannelRate& r = s.rates[chat_id(*id)];
        r.rate = p.dbl(rates->at(name), "rate", wc, r.rate);
        r.burst = p.dbl(rates->at(name), "burst", wc, r.burst);
        if (!rates->at(name).contains("rate") || !rates->at(name).contains("burst")) p.error(wc + ": needs 'rate' and 'burst'");
      }
      for (const auto& [k, v] : rates->items()) {
        const auto known = channel_by_name(k);
        if (!known || !sendable_channel(chat_id(*known))) p.error(w + ".channels: unknown channel '" + k + "'");
      }
    }
    s.repeat_window_seconds = p.num(*ch, "repeat_window_seconds", w, s.repeat_window_seconds);
    s.repeat_limit = p.integer(*ch, "repeat_limit", w, s.repeat_limit);
    s.emote_radius = p.num(*ch, "emote_radius", w, s.emote_radius);
  }
  if (const json* f = object(doc, "filter", "social.json")) {
    s.filter_mask = p.str(*f, "mask", "social.filter", true, s.filter_mask);
    s.filter_words.clear();
    for (const auto& word : p.str_list(*f, "words", "social.filter")) s.filter_words.push_back(lower(word));
  }
  if (const json* g = object(doc, "guild", "social.json")) {
    const std::string w = "social.guild";
    s.guild_name_min = p.integer(*g, "name_min", w, s.guild_name_min);
    s.guild_name_max = p.integer(*g, "name_max", w, s.guild_name_max);
    s.guild_max_members = p.integer(*g, "max_members", w, s.guild_max_members);
    s.guild_invite_seconds = p.num(*g, "invite_seconds", w, s.guild_invite_seconds);
    s.motd_max = p.integer(*g, "motd_max", w, s.motd_max);
    if (!g->contains("ranks") || !g->at("ranks").is_array()) {
      p.error(w + ": 'ranks' must be an array");
    } else {
      s.guild_ranks.clear();
      const auto& ranks = g->at("ranks");
      for (std::size_t i = 0; i < ranks.size(); ++i) {
        const std::string wr = w + ".ranks[" + std::to_string(i) + "]";
        GuildRankDef r;
        r.id = p.str(ranks[i], "id", wr);
        r.name = p.str(ranks[i], "name", wr);
        for (const auto& perm : p.str_list(ranks[i], "permissions", wr)) {
          if (const auto bit = guild_perm_by_name(perm)) r.perms |= static_cast<std::uint16_t>(*bit);
          else p.error(wr + ": unknown permission '" + perm + "'");
        }
        s.guild_ranks.push_back(std::move(r));
      }
    }
  }
  if (const json* f = object(doc, "friends", "social.json")) {
    s.max_friends = p.integer(*f, "max_friends", "social.friends", s.max_friends);
    s.max_ignored = p.integer(*f, "max_ignored", "social.friends", s.max_ignored);
  }
  if (const json* who = object(doc, "who", "social.json")) s.who_max_results = p.integer(*who, "max_results", "social.who", s.who_max_results);
}

void validate_social(const Content& c, std::vector<std::string>& e) {
  const SocialRules& s = c.social;
  if (s.group_max < 2 || s.group_max > 6) e.emplace_back("social.group.max_members must be 2..6 (the wire carries six)");
  if (c.rules.economy.loot_party_max < s.group_max) e.emplace_back("rules.economy.loot_party_max must be at least social.group.max_members");
  if (s.group_invite_seconds < 5.0f || s.group_invite_seconds > 600.0f) e.emplace_back("social.group.invite_seconds must be 5..600");
  if (s.group_status_seconds < 0.05f || s.group_status_seconds > 10.0f) e.emplace_back("social.group.status_seconds must be 0.05..10");
  if (s.group_xp_range < 10.0f || s.group_xp_range > 1000.0f) e.emplace_back("social.group.xp_range must be 10..1000");
  if (s.group_offline_grace_seconds < 0.0f || s.group_offline_grace_seconds > 86400.0f)
    e.emplace_back("social.group.offline_grace_seconds must be 0..86400");
  if (s.group_level_window < 0 || s.group_level_window > 59) e.emplace_back("social.group.level_window must be 0..59");
  if (s.group_level_fraction < 0.0f || s.group_level_fraction > 1.0f) e.emplace_back("social.group.level_fraction must be 0..1");
  if (s.raid_max_groups < 2 || s.raid_max_groups > 12) e.emplace_back("social.raid.max_groups must be 2..12 (the wire carries twelve)");
  if (s.raid_invite_seconds < 5.0f || s.raid_invite_seconds > 600.0f) e.emplace_back("social.raid.invite_seconds must be 5..600");
  if (s.raid_lockout_hours < 0.0f || s.raid_lockout_hours > 336.0f) e.emplace_back("social.raid.lockout_hours must be 0..336 (two weeks)");
  if (s.raid_evict_seconds < 5.0f || s.raid_evict_seconds > 600.0f) e.emplace_back("social.raid.evict_seconds must be 5..600");
  for (const char* name : kRateChannels) {
    const ChannelRate& r = s.rates[chat_id(*channel_by_name(name))];
    if (r.rate <= 0.0 || r.rate > 100.0 || r.burst < 1.0 || r.burst > 100.0)
      e.push_back(std::string("social.chat.channels.") + name + ": rate must be in (0, 100] and burst in [1, 100]");
  }
  if (s.repeat_window_seconds < 0.0f || s.repeat_window_seconds > 3600.0f) e.emplace_back("social.chat.repeat_window_seconds must be 0..3600");
  if (s.repeat_limit < 1 || s.repeat_limit > 16) e.emplace_back("social.chat.repeat_limit must be 1..16");
  if (s.emote_radius < 1.0f || s.emote_radius > 500.0f) e.emplace_back("social.chat.emote_radius must be 1..500");
  if (s.filter_mask.size() != 1 || static_cast<unsigned char>(s.filter_mask[0]) < 0x21 || static_cast<unsigned char>(s.filter_mask[0]) > 0x7E)
    e.emplace_back("social.filter.mask must be one printable ASCII character");
  std::set<std::string> words;
  for (const auto& w : s.filter_words) {
    if (w.empty() || w.size() > 32 || !std::all_of(w.begin(), w.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; }))
      e.push_back("social.filter.words: '" + w + "' must be 1-32 ASCII letters");
    if (!words.insert(w).second) e.push_back("social.filter.words: '" + w + "' is listed twice");
  }
  if (s.guild_name_min < 3 || s.guild_name_max > 32 || s.guild_name_min > s.guild_name_max)
    e.emplace_back("social.guild: name_min/name_max must satisfy 3 <= name_min <= name_max <= 32");
  if (s.guild_max_members < 2 || s.guild_max_members > 256) e.emplace_back("social.guild.max_members must be 2..256 (the roster carries 256)");
  if (s.guild_invite_seconds < 5.0f || s.guild_invite_seconds > 600.0f) e.emplace_back("social.guild.invite_seconds must be 5..600");
  if (s.motd_max < 1 || s.motd_max > 256) e.emplace_back("social.guild.motd_max must be 1..256");
  if (s.guild_ranks.size() < 2 || s.guild_ranks.size() > 8) e.emplace_back("social.guild.ranks must list 2..8 ranks (leader first)");
  std::set<std::string> rank_ids;
  for (const auto& r : s.guild_ranks) {
    if (r.id.empty() || r.name.empty() || r.name.size() > 24) e.push_back("social.guild.ranks: rank '" + r.id + "' needs an id and a name of 1-24 characters");
    if (!rank_ids.insert(r.id).second) e.push_back("social.guild.ranks: '" + r.id + "' is listed twice");
  }
  if (!s.guild_ranks.empty()) {
    const auto& lead = s.guild_ranks.front();
    if (lead.perms != kAllPerms) e.emplace_back("social.guild.ranks[0] (the leader) must hold every permission");
    for (std::size_t i = 1; i < s.guild_ranks.size(); ++i)
      if (s.guild_ranks[i].has(GuildPerm::Disband)) e.push_back("social.guild.ranks: only the leader may disband ('" + s.guild_ranks[i].id + "')");
  }
  if (s.max_friends < 0 || s.max_friends > 64 || s.max_ignored < 0 || s.max_ignored > 64)
    e.emplace_back("social.friends: max_friends and max_ignored must be 0..64 (the list carries 64)");
  if (s.who_max_results < 1 || s.who_max_results > 64) e.emplace_back("social.who.max_results must be 1..64");
}

}  // namespace content_detail
}  // namespace ef::game
