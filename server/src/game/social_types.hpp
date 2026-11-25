// Phase 7: the small vocabulary shared by content (social.json), the chat and guild rules, and the
// game server: chat channels as they travel on the wire, and guild permissions.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ef::game {

// ChatMessage.channel / ChatSend.channel values. Say, System and Combat keep their Phase 1-2 numbers.
enum class ChatChannel : std::uint8_t {
  Say = 0,
  System = 1,
  Combat = 2,
  Shout = 3,     // the whole zone
  Ooc = 4,       // the whole server
  Tell = 5,      // a private message to you (from_name = the sender)
  TellEcho = 6,  // your own tell, echoed back (from_name = the recipient)
  Group = 7,
  Guild = 8,
  Emote = 9,     // "Name waves." to everyone in say range
  Gm = 10,       // game master announcements
  Raid = 11,     // Phase 14: everyone in your raid
};
inline constexpr std::size_t kChatChannelCount = 12;

constexpr std::uint8_t chat_id(ChatChannel c) { return static_cast<std::uint8_t>(c); }
constexpr std::uint16_t chat_bit(ChatChannel c) { return static_cast<std::uint16_t>(1u << static_cast<unsigned>(c)); }

// The channels a player may speak on (ChatSend).
std::optional<ChatChannel> sendable_channel(std::uint8_t id);
// The channels a player may turn off for themselves (/channel NAME off).
bool mutable_channel(ChatChannel c);
// "say", "shout", "ooc", "tell", "group", "guild", "emote", "gm", "raid" (and a few aliases) -> channel.
std::optional<ChatChannel> channel_by_name(std::string_view name);
const char* channel_name(ChatChannel c);

// Guild rank permissions (GuildRankInfo.permissions bits).
enum class GuildPerm : std::uint16_t {
  Invite = 1,
  Remove = 2,
  Promote = 4,
  Demote = 8,
  Motd = 16,
  Speak = 32,
  Disband = 64,
};
std::optional<GuildPerm> guild_perm_by_name(std::string_view name);

struct GuildRankDef {
  std::string id;
  std::string name;
  std::uint16_t perms = 0;
  bool has(GuildPerm p) const { return (perms & static_cast<std::uint16_t>(p)) != 0; }
};

struct ChannelRate {
  double rate = 1.0;   // messages per second
  double burst = 5.0;  // messages that may be sent at once
};

// content/social.json.
struct SocialRules {
  // Groups.
  int group_max = 6;
  float group_invite_seconds = 60.0f;
  float group_status_seconds = 0.5f;  // how often a member's bars may refresh for the others
  // Phase 12: how long a member who left the world stays in their group, offline (0 = they leave at once).
  float group_offline_grace_seconds = 900.0f;
  float group_xp_range = 120.0f;      // metres from the kill a member must be to share XP and loot
  int group_level_window = 5;         // a member this many levels (or level_fraction of the highest) below the
  float group_level_fraction = 0.25f; // highest member still shares experience; lower than that gets none
  // Raids (Phase 14): whole groups joined together for the raid dungeons.
  int raid_max_groups = 12;
  float raid_invite_seconds = 60.0f;
  // Chat.
  std::array<ChannelRate, kChatChannelCount> rates{};
  float repeat_window_seconds = 30.0f;
  int repeat_limit = 3;  // a line said this many times within the window may not be said again (spam)
  float emote_radius = 30.0f;
  std::string filter_mask = "*";
  std::vector<std::string> filter_words;  // lower case
  // Guilds.
  int guild_name_min = 4;
  int guild_name_max = 24;
  int guild_max_members = 250;
  float guild_invite_seconds = 60.0f;
  int motd_max = 200;
  std::vector<GuildRankDef> guild_ranks;  // [0] = leader
  // Friends and /who.
  int max_friends = 50;
  int max_ignored = 50;
  int who_max_results = 50;

  SocialRules();
};

}  // namespace ef::game
