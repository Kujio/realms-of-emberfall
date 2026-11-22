// GameServer, Phase 7 social systems: chat channels, slash commands, groups, guilds, friends, ignore
// lists and /who. Game rules live in chat/groups/guilds/commands (pure); this file routes and persists.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>

#include <spdlog/spdlog.h>

#include "auth/bans.hpp"
#include "auth/names.hpp"
#include "game/game_server.hpp"
#include "net/byte_buffer.hpp"

namespace ef::game {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

std::int64_t unix_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint8_t pct(int v, int max) {
  if (max <= 0) return 0;
  return static_cast<std::uint8_t>(std::clamp(static_cast<long long>(v) * 100 / max, 0LL, 100LL));
}

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& b) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const std::uint8_t x : b) h = (h ^ x) * 1099511628211ULL;
  return h | 1ULL;  // never 0 (0 = nothing sent yet)
}

std::uint32_t ms(float seconds) { return static_cast<std::uint32_t>(std::lround(static_cast<double>(seconds) * 1000.0)); }

// Each line fits one chat message (ChatMessage.text carries 256 bytes).
const char* const kHelp[] = {
    "Chat: /say /shout /ooc /tell NAME /reply /gsay /guildsay /emote.",
    "Groups: /invite [NAME] /join /decline /disband /kick NAME /makeleader NAME /lootrule ng|rr.",
    ("Guilds: /guildcreate NAME /guildinvite [NAME] /guildjoin /guilddecline /guildleave /guildremove NAME /guildpromote NAME "
     "/guilddemote NAME /guildleader NAME /guildmotd [TEXT] /guilddisband /guild."),
    "Social: /friend [NAME] /ignore [NAME] /channel NAME on|off /who [all] [NAME] [CLASS] [RACE] [N-M] [ZONE] [guild NAME].",
    "Other: /assist [NAME] /loc /sit /stand /con /target NAME.",
};

}  // namespace

// ------------------------------------------------------------------------- lookups and plumbing

GameServer::Session* GameServer::in_world_by_char(std::int64_t char_id) {
  auto it = online_chars_.find(char_id);
  if (it == online_chars_.end()) return nullptr;
  Session* s = find_session(it->second);
  return s && s->state == State::InWorld && s->char_id == char_id ? s : nullptr;
}

GameServer::Session* GameServer::in_world_by_name(std::string_view name) {
  auto it = online_names_.find(lower(name));
  return it == online_names_.end() ? nullptr : in_world_by_char(it->second);
}

PlayerEntity* GameServer::player_of(Session& s) {
  if (s.state != State::InWorld) return nullptr;
  Zone* z = zone_for(s);
  return z ? z->find(s.entity) : nullptr;
}

std::string GameServer::zone_display(const std::string& zone_id) const {
  const ZoneDef* z = content_.find_zone(zone_id);
  return z ? z->name : zone_id;
}

void GameServer::system_to(Session& s, const std::string& text) { send(s.peer, proto::ChatMessage{chat_id(ChatChannel::System), "", text}); }

void GameServer::send_chat(const Session& to, ChatChannel channel, const std::string& from, const std::string& text) {
  send(to.peer, proto::ChatMessage{chat_id(channel), from, text});
}

std::string GameServer::name_or_target(Session& s, const Command& c, std::size_t arg) {
  if (c.args.size() > arg && !c.args[arg].empty()) return c.args[arg];
  PlayerEntity* p = player_of(s);
  Zone* z = zone_for(s);
  if (!p || !z || p->target == 0 || p->target == p->id) return {};
  if (const PlayerEntity* t = z->find(p->target)) return t->name;
  return {};
}

void GameServer::on_world_enter_social(Session& s) {
  online_chars_[s.char_id] = s.peer;
  online_names_[lower(s.char_name)] = s.char_id;
  s.group_hash = 0;
  if (auto note = auth::mute_notice(s.muted_until, unix_now())) system_to(s, *note);
  if (Guild* g = guilds_.of(s.char_id)) {
    if (GuildMember* m = g->member(s.char_id); m)
      if (const PlayerEntity* p = player_of(s)) {
        m->level = p->level;
        m->class_id = p->class_id;
      }
    if (!g->motd.empty()) send_chat(s, ChatChannel::Guild, "", "Guild message of the day: " + g->motd);
    guild_dirty(g->id);
  }
  send_guild_info(s);
  send_friends(s);
  notify_friends(s, true);
  send_group_state(s, true);
}

void GameServer::on_world_leave_social(Session& s) {
  const GroupChange c = groups_.remove_character(s.char_id);
  if (!c.touched.empty()) group_changed(c, c.disbanded ? "Your group has disbanded." : s.char_name + " has left the group.", s.char_id);
  guilds_.drop_invites_of(s.char_id);
  if (Guild* g = guilds_.of(s.char_id)) {
    if (GuildMember* m = g->member(s.char_id))
      if (const PlayerEntity* p = player_of(s)) m->level = p->level;
    guild_dirty(g->id);
  }
  notify_friends(s, false);
  online_chars_.erase(s.char_id);
  if (auto it = online_names_.find(lower(s.char_name)); it != online_names_.end() && it->second == s.char_id) online_names_.erase(it);
  s.group_hash = 0;
  s.reply_to.clear();
  s.guild_pending = false;
}

void GameServer::social_tick() {
  for (const auto& [to, inv] : groups_.expire_invites(now_)) {
    if (Session* t = in_world_by_char(to)) system_to(*t, "The group invitation from " + inv.from_name + " has expired.");
    if (Session* f = in_world_by_char(inv.from)) system_to(*f, "Your group invitation was not answered.");
  }
  for (const auto& [to, inv] : guilds_.expire_invites(now_)) {
    if (Session* t = in_world_by_char(to)) system_to(*t, "The guild invitation from " + inv.from_name + " has expired.");
    if (Session* f = in_world_by_char(inv.from)) system_to(*f, "Your guild invitation was not answered.");
  }
  for (auto& [peer, s] : sessions_) {
    if (s.state != State::InWorld) continue;
    // Only members' bars move; leaving a group sends the empty state at once (group_changed).
    if (groups_.group_of(s.char_id)) send_group_state(s, false);
    if (s.friends_dirty) send_friends(s);
  }
  for (const std::int64_t id : dirty_guilds_)
    if (const Guild* g = guilds_.find(id))
      for (const auto& m : g->members)
        if (Session* ms = in_world_by_char(m.char_id)) send_guild_info(*ms);
  dirty_guilds_.clear();
}

// ------------------------------------------------------------------------- chat

void GameServer::handle(Session& s, const proto::ChatSend& m) {
  if (s.state != State::InWorld) {
    if (s.state != State::CharSelect) violation(s, "chat in wrong state");
    return;
  }
  const auto ch = sendable_channel(m.channel);
  if (!ch) return violation(s, "chat on an unknown channel");
  chat(s, *ch, m.target, m.text);
}

void GameServer::chat(Session& s, ChatChannel ch, const std::string& target, std::string_view raw) {
  if (s.state != State::InWorld) return;
  if (auto mute = auth::mute_notice(s.muted_until, unix_now())) return system_to(s, *mute);
  std::string text = auth::sanitize_chat(raw);
  if (text.empty()) return;
  Session* tell_to = nullptr;
  if (ch == ChatChannel::Tell) {
    if (target.empty()) return system_to(s, "Tell whom? /tell NAME message");
    tell_to = in_world_by_name(target);
    // Someone ignoring you looks offline to you.
    if (!tell_to || tell_to->social.ignores(s.char_name)) return system_to(s, auth::normalize_character_name(target) + " is not online.");
    if (tell_to == &s) return system_to(s, "You mutter to yourself.");
  }
  const Group* group = nullptr;
  const Guild* guild = nullptr;
  if (ch == ChatChannel::Group && !(group = groups_.group_of(s.char_id))) return system_to(s, "You are not in a group.");
  if (ch == ChatChannel::Guild) {
    guild = guilds_.of(s.char_id);
    if (!guild) return system_to(s, "You are not in a guild.");
    const GuildMember* gm = guild->member(s.char_id);
    if (!gm || !guild_allows(content_.social, gm->rank, GuildPerm::Speak)) return system_to(s, "Your guild rank may not speak in guild chat.");
  }
  if (const ChatVerdict v = s.chat.check(ch, text, now_); v != ChatVerdict::Ok) return system_to(s, chat_refusal(v));
  text = filter_->clean(text);
  Zone* zone = zone_for(s);
  const PlayerEntity* me = player_of(s);
  if (!zone || !me) return;

  const proto::ChatMessage msg{chat_id(ch), s.char_name, text};
  proto::encode_packet_into(msg, send_buf_);
  auto deliver = [&](const Session& to) {
    if (&to != &s && (to.social.ignores(s.char_name) || !to.social.hears(ch))) return;
    transport_.send(to.peer, proto::ChatMessage::kChannel, send_buf_, true);
  };
  switch (ch) {
    case ChatChannel::Say:
    case ChatChannel::Emote: {
      std::vector<EntityId> near;
      zone->players_near(me->pos, ch == ChatChannel::Say ? zone->def().say_radius : content_.social.emote_radius, near);
      for (EntityId id : near)
        if (const PlayerEntity* p = zone->find(id))
          if (const Session* to = find_session(p->session)) deliver(*to);
      break;
    }
    case ChatChannel::Shout:
      for (const auto& [peer, o] : sessions_)
        if (o.state == State::InWorld && o.zone_id == s.zone_id) deliver(o);
      break;
    case ChatChannel::Ooc:
      for (const auto& [peer, o] : sessions_)
        if (o.state == State::InWorld) deliver(o);
      break;
    case ChatChannel::Tell:
      transport_.send(tell_to->peer, proto::ChatMessage::kChannel, send_buf_, true);
      tell_to->reply_to = s.char_name;
      send_chat(s, ChatChannel::TellEcho, tell_to->char_name, text);
      break;
    case ChatChannel::Group:
      for (const auto& m : group->members)
        if (const Session* to = in_world_by_char(m.char_id)) deliver(*to);
      break;
    case ChatChannel::Guild:
      for (const auto& m : guild->members)
        if (const Session* to = in_world_by_char(m.char_id)) deliver(*to);
      break;
    default:
      return;
  }
  // Tells are private: the log keeps who wrote to whom, not what was said.
  if (ch == ChatChannel::Tell) spdlog::info("[tell] {} -> {} ({} bytes)", s.char_name, tell_to->char_name, text.size());
  else spdlog::info("[{}] {}: {}", channel_name(ch), s.char_name, text);
}

// ------------------------------------------------------------------------- slash commands

void GameServer::handle(Session& s, const proto::SlashCommand& m) {
  if (s.state != State::InWorld) {
    if (s.state != State::CharSelect) violation(s, "command in wrong state");
    return;
  }
  if (!s.command_limiter.consume(1.0, now_)) return system_to(s, "You are sending commands too quickly.");
  const auto c = parse_command(auth::sanitize_chat(m.text));
  if (c) command(s, *c);
}

void GameServer::command(Session& s, const Command& c) {
  const std::string& n = c.name;
  auto is = [&](std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(), [&](const char* x) { return n == x; });
  };
  // Chat channels typed as commands.
  if (is({"say", "s"})) return chat(s, ChatChannel::Say, "", c.rest);
  if (is({"shout", "sh"})) return chat(s, ChatChannel::Shout, "", c.rest);
  if (is({"ooc"})) return chat(s, ChatChannel::Ooc, "", c.rest);
  if (is({"tell", "t", "msg", "whisper"})) return chat(s, ChatChannel::Tell, c.arg(0), c.rest_after(1));
  if (is({"reply", "r"})) {
    if (s.reply_to.empty()) return system_to(s, "Nobody has sent you a tell to reply to.");
    return chat(s, ChatChannel::Tell, s.reply_to, c.rest);
  }
  if (is({"gsay", "g", "groupsay"})) return chat(s, ChatChannel::Group, "", c.rest);
  if (is({"guildsay", "gu"})) return chat(s, ChatChannel::Guild, "", c.rest);
  if (is({"emote", "em", "me"})) return chat(s, ChatChannel::Emote, "", c.rest);
  // Groups.
  if (is({"invite", "inv"})) return group_invite(s, name_or_target(s, c));
  if (is({"join", "follow"})) return group_accept(s);
  if (is({"decline"})) return group_decline(s);
  if (is({"disband", "leave", "leavegroup"})) return group_leave(s);
  if (is({"kick"})) return group_kick(s, name_or_target(s, c));
  if (is({"makeleader", "ml"})) return group_make_leader(s, name_or_target(s, c));
  if (is({"lootrule"})) {
    const std::string r = lower(c.arg(0));
    if (r == "rr" || r == "roundrobin") return group_loot_rule(s, LootRule::RoundRobin);
    if (r == "ng" || r == "need" || r == "needgreed" || r == "greed") return group_loot_rule(s, LootRule::NeedGreed);
    return system_to(s, "Usage: /lootrule ng (need before greed) | rr (round robin)");
  }
  // Guilds.
  if (is({"guildcreate"})) return guild_create(s, c.rest);
  if (is({"guildinvite", "ginvite"})) return guild_invite(s, name_or_target(s, c));
  if (is({"guildjoin", "gjoin"})) return guild_accept(s);
  if (is({"guilddecline", "gdecline"})) return guild_decline(s);
  if (is({"guildleave", "gleave"})) return guild_leave(s);
  if (is({"guildremove", "gremove", "guildkick"})) return guild_remove(s, name_or_target(s, c));
  if (is({"guildpromote", "gpromote"})) return guild_rank(s, name_or_target(s, c), 1);
  if (is({"guilddemote", "gdemote"})) return guild_rank(s, name_or_target(s, c), -1);
  if (is({"guildleader", "gleader"})) return guild_rank(s, name_or_target(s, c), 0);
  if (is({"guildmotd", "gmotd"})) return guild_motd(s, c.rest);
  if (is({"guilddisband", "gdisband"})) return guild_disband(s);
  if (is({"guild", "guildinfo", "ginfo"})) {
    const Guild* g = guilds_.of(s.char_id);
    if (!g) return system_to(s, "You are not in a guild. Found one with /guildcreate NAME.");
    send_guild_info(s);
    return system_to(s, "<" + g->name + ">: " + std::to_string(g->members.size()) + " members. " +
                            (g->motd.empty() ? std::string("No message of the day.") : "Message of the day: " + g->motd));
  }
  // Friends, ignore, channels, /who, /assist.
  if (is({"friend", "friends"})) return friend_toggle(s, c.arg(0), 0);
  if (is({"addfriend"})) return friend_toggle(s, c.arg(0), 1);
  if (is({"removefriend", "delfriend"})) return friend_toggle(s, c.arg(0), -1);
  if (is({"ignore"})) return ignore_toggle(s, c.arg(0), 0);
  if (is({"unignore"})) return ignore_toggle(s, c.arg(0), -1);
  if (is({"channel", "chan"})) return channel_toggle(s, c);
  if (is({"who"})) return who(s, c);
  if (is({"assist"})) return assist(s, c);
  if (is({"gm"})) return gm_command(s, c);
  if (is({"help", "commands", "?"})) {
    for (const char* line : kHelp) system_to(s, line);
    return;
  }
  system_to(s, "Unknown command /" + n + ". Type /help for a list.");
}

// ------------------------------------------------------------------------- groups

void GameServer::group_invite(Session& s, const std::string& name) {
  if (name.empty()) return system_to(s, "Invite whom? Target a player or /invite NAME.");
  Session* to = in_world_by_name(name);
  if (!to || to->social.ignores(s.char_name)) return system_to(s, auth::normalize_character_name(name) + " is not online.");
  if (to == &s) return system_to(s, group_error_text(GroupError::InviteSelf));
  const GroupChange c = groups_.invite(s.char_id, s.char_name, to->char_id, now_ + static_cast<double>(content_.social.group_invite_seconds));
  if (!c.ok()) {
    if (c.error == GroupError::TargetGrouped) return system_to(s, to->char_name + " is already in a group.");
    return system_to(s, group_error_text(c.error));
  }
  send(to->peer, proto::SocialInvite{0, s.char_name, "", ms(content_.social.group_invite_seconds)});
  system_to(*to, s.char_name + " invites you to join a group. (/join to accept, /decline to refuse)");
  system_to(s, "You invite " + to->char_name + " to join your group.");
}

void GameServer::group_accept(Session& s) {
  const GroupInvite* inv = groups_.invite_for(s.char_id);
  if (!inv) return system_to(s, group_error_text(GroupError::NoInvite));
  Session* from = in_world_by_char(inv->from);
  if (!from) {
    groups_.decline(s.char_id);
    return system_to(s, "Whoever invited you is no longer in the world.");
  }
  const GroupChange c = groups_.accept(s.char_id, s.char_name);
  if (!c.ok()) return system_to(s, group_error_text(c.error));
  const Group* g = groups_.find(c.group);
  const GroupMemberRef* lead = g ? g->member(g->leader) : nullptr;
  system_to(s, "You join " + (lead ? lead->name : from->char_name) + "'s group.");
  group_changed(c, s.char_name + " has joined the group.", s.char_id);
}

void GameServer::group_decline(Session& s) {
  const std::int64_t from = groups_.decline(s.char_id);
  if (from == 0) return system_to(s, group_error_text(GroupError::NoInvite));
  system_to(s, "You decline the invitation.");
  if (Session* f = in_world_by_char(from)) system_to(*f, s.char_name + " declines your invitation.");
}

void GameServer::group_leave(Session& s) {
  const GroupChange c = groups_.leave(s.char_id);
  if (!c.ok()) return system_to(s, group_error_text(c.error));
  system_to(s, "You leave the group.");
  group_changed(c, c.disbanded ? "Your group has disbanded." : s.char_name + " has left the group.", s.char_id);
}

void GameServer::group_kick(Session& s, const std::string& name) {
  const Group* g = groups_.group_of(s.char_id);
  if (!g) return system_to(s, group_error_text(GroupError::NotInGroup));
  if (name.empty()) return system_to(s, "Remove whom? /kick NAME");
  std::int64_t target = 0;
  std::string target_name;
  for (const auto& m : g->members)
    if (lower(m.name) == lower(name)) {
      target = m.char_id;
      target_name = m.name;
    }
  if (target == 0) return system_to(s, group_error_text(GroupError::NotMember));
  const GroupChange c = groups_.kick(s.char_id, target);
  if (!c.ok()) return system_to(s, group_error_text(c.error));
  if (Session* t = in_world_by_char(target)) system_to(*t, "You have been removed from the group.");
  group_changed(c, c.disbanded ? "Your group has disbanded." : target_name + " has been removed from the group.", target);
}

void GameServer::group_make_leader(Session& s, const std::string& name) {
  const Group* g = groups_.group_of(s.char_id);
  if (!g) return system_to(s, group_error_text(GroupError::NotInGroup));
  std::int64_t target = 0;
  std::string target_name;
  for (const auto& m : g->members)
    if (lower(m.name) == lower(name)) {
      target = m.char_id;
      target_name = m.name;
    }
  if (target == 0) return system_to(s, group_error_text(GroupError::NotMember));
  const GroupChange c = groups_.make_leader(s.char_id, target);
  if (!c.ok()) return system_to(s, group_error_text(c.error));
  group_changed(c, target_name + " is now the group leader.");
}

void GameServer::group_loot_rule(Session& s, LootRule rule) {
  if (!groups_.group_of(s.char_id)) {
    if (Zone* z = zone_for(s)) z->set_loot_rule(s.entity, rule, *this);
    return;
  }
  const GroupChange c = groups_.set_loot_rule(s.char_id, rule);
  if (!c.ok()) return system_to(s, group_error_text(c.error));
  group_changed(c, rule == LootRule::RoundRobin ? "The group shares loot round robin." : "The group rolls need before greed for loot.");
}

void GameServer::group_changed(const GroupChange& c, const std::string& message, std::int64_t except) {
  for (const std::int64_t id : c.touched)
    if (Session* m = in_world_by_char(id)) {
      if (!message.empty() && id != except) system_to(*m, message);
      send_group_state(*m, true);
    }
}

void GameServer::send_group_state(Session& s, bool force) {
  proto::GroupState st;
  if (const Group* g = groups_.group_of(s.char_id)) {
    st.group_id = g->id;
    if (const GroupMemberRef* lead = g->member(g->leader)) st.leader = lead->name;
    st.loot_rule = static_cast<std::uint8_t>(g->loot_rule);
    for (const auto& m : g->members) {
      proto::GroupMember gm;
      gm.name = m.name;
      gm.mana_pct = 255;
      if (Session* ms = in_world_by_char(m.char_id))
        if (const PlayerEntity* p = player_of(*ms)) {
          gm.class_id = p->class_id;
          gm.level = static_cast<std::uint8_t>(std::clamp(p->level, 1, 255));
          gm.zone = zone_display(ms->zone_id);
          gm.entity_id = ms->zone_id == s.zone_id ? p->id : 0;
          gm.hp_pct = p->dead ? 0 : pct(p->hp, p->stats.max_hp);
          gm.mana_pct = p->stats.max_mana > 0 ? pct(p->mana, p->stats.max_mana) : 255;
          gm.endurance_pct = pct(p->endurance, p->stats.max_endurance);
          gm.dead = p->dead;
        }
      st.members.push_back(std::move(gm));
    }
  }
  proto::encode_packet_into(st, send_buf_);
  const std::uint64_t h = hash_bytes(send_buf_);
  if (!force && h == s.group_hash) return;
  s.group_hash = h;
  transport_.send(s.peer, proto::GroupState::kChannel, send_buf_, true);
}

// ------------------------------------------------------------------------- guilds

void GameServer::load_guilds() {
  guilds_loading_ = true;
  db_.post([this](db::Repository& repo, db::DbWorker& w) {
    auto rows = repo.load_guilds();
    w.complete([this, rows = std::move(rows)] {
      guilds_loading_ = false;
      if (!rows) {
        spdlog::error("loading guilds failed; retrying in 10 s");
        guild_retry_tick_ = tick_ + 10 * std::max<std::uint32_t>(1, cfg_.tick_rate);
        return;
      }
      std::vector<Guild> all;
      all.reserve(rows->size());
      for (const auto& r : *rows) {
        Guild g;
        g.id = r.id;
        g.name = r.name;
        g.motd = r.motd;
        for (const auto& m : r.members) g.members.push_back({m.char_id, m.name, m.rank, m.level, m.class_id});
        // Left over from a cleanup that did not finish: an empty guild is deleted (freeing its name) ...
        if (g.members.empty()) {
          const std::int64_t gid = g.id;
          spdlog::warn("guild <{}> has no members; deleting it", g.name);
          db_.post([gid](db::Repository& rp, db::DbWorker&) { rp.delete_guild(gid); });
          continue;
        }
        // ... and one without a leader gets one: the highest rank present.
        if (g.leader_count() == 0) {
          auto top = std::min_element(g.members.begin(), g.members.end(), [](const GuildMember& a, const GuildMember& b) { return a.rank < b.rank; });
          top->rank = 0;
          const std::int64_t gid = g.id;
          const std::int64_t cid = top->char_id;
          spdlog::warn("guild <{}> has no leader; {} takes over", g.name, top->name);
          db_.post([gid, cid](db::Repository& rp, db::DbWorker&) { rp.set_guild_member(gid, cid, 0); });
        }
        all.push_back(std::move(g));
      }
      guilds_.load(std::move(all));
      guilds_loaded_ = true;
      if (!rows->empty()) spdlog::info("restored {} guild(s)", rows->size());
      for (auto& [peer, s] : sessions_)
        if (s.state == State::InWorld)
          if (const Guild* g = guilds_.of(s.char_id)) {
            set_guild_tag(s, g->name);
            send_guild_info(s);
          }
    });
  });
}

void GameServer::set_guild_tag(Session& s, const std::string& tag) {
  if (Zone* z = zone_for(s)) z->set_guild_tag(s.entity, tag, *this);
}

void GameServer::guild_dirty(std::int64_t guild_id) { dirty_guilds_.insert(guild_id); }

void GameServer::guild_broadcast(const Guild& g, const std::string& text) {
  for (const auto& m : g.members)
    if (Session* ms = in_world_by_char(m.char_id)) send_chat(*ms, ChatChannel::Guild, "", text);
}

void GameServer::send_guild_info(Session& s) {
  proto::GuildInfo info;
  const SocialRules& R = content_.social;
  for (const auto& r : R.guild_ranks) info.ranks.push_back({r.name, r.perms});
  if (const Guild* g = guilds_.of(s.char_id)) {
    info.guild_id = static_cast<std::uint32_t>(std::clamp<std::int64_t>(g->id, 0, 0xFFFFFFFFLL));
    info.name = g->name;
    info.motd = g->motd;
    if (const GuildMember* me = g->member(s.char_id)) info.my_rank = static_cast<std::uint8_t>(std::clamp(me->rank, 0, 255));
    std::vector<const GuildMember*> order;
    for (const auto& m : g->members) order.push_back(&m);
    std::sort(order.begin(), order.end(), [](const GuildMember* a, const GuildMember* b) { return a->rank != b->rank ? a->rank < b->rank : a->name < b->name; });
    for (const GuildMember* m : order) {
      proto::GuildMemberInfo e;
      e.name = m->name;
      e.rank = static_cast<std::uint8_t>(std::clamp(m->rank, 0, 255));
      e.level = static_cast<std::uint8_t>(std::clamp(m->level, 1, 255));
      e.class_id = m->class_id;
      if (Session* ms = in_world_by_char(m->char_id)) {
        e.online = true;
        e.zone = zone_display(ms->zone_id);
        if (const PlayerEntity* p = player_of(*ms)) e.level = static_cast<std::uint8_t>(std::clamp(p->level, 1, 255));
      }
      info.members.push_back(std::move(e));
    }
  }
  send(s.peer, info);
}

void GameServer::guild_create(Session& s, const std::string& raw_name) {
  if (!guilds_loaded_) return system_to(s, "Guilds are still being set up. Try again in a moment.");
  if (guilds_.of(s.char_id)) return system_to(s, "You are already in a guild.");
  if (s.guild_pending) return system_to(s, "Your guild is already being founded.");
  const std::string name = normalize_guild_name(raw_name);
  if (name.empty()) return system_to(s, "Usage: /guildcreate GUILD NAME");
  if (auto err = validate_guild_name(name, content_.social, filter_.get())) return system_to(s, *err);
  if (guilds_.by_name(name)) return system_to(s, "That guild name is already taken.");
  const PlayerEntity* p = player_of(s);
  if (!p) return;
  s.guild_pending = true;
  const net::PeerId peer = s.peer;
  const std::int64_t cid = s.char_id;
  const std::string cname = s.char_name;
  const int level = p->level;
  const std::string cls = p->class_id;
  db_.post([this, peer, cid, cname, level, cls, name](db::Repository& repo, db::DbWorker& w) {
    std::int64_t id = 0;
    const db::DbStatus st = repo.create_guild(name, cid, id);
    w.complete([this, peer, cid, cname, level, cls, name, st, id] {
      Session* sp = find_session(peer);
      if (sp && sp->char_id == cid) sp->guild_pending = false;
      if (st != db::DbStatus::Ok) {
        if (sp && sp->char_id == cid)
          system_to(*sp, st == db::DbStatus::Duplicate ? "That guild name is already taken." : "The guild could not be founded. Please try again.");
        return;
      }
      Guild g;
      g.id = id;
      g.name = name;
      g.members.push_back({cid, cname, 0, level, cls});
      guilds_.add(std::move(g));
      spdlog::info("'{}' founded the guild <{}> (id {})", cname, name, id);
      if (Session* m = in_world_by_char(cid)) {
        send_chat(*m, ChatChannel::Guild, "", "You found the guild <" + name + ">. Invite members with /guildinvite NAME.");
        set_guild_tag(*m, name);
        send_guild_info(*m);
      }
    });
  });
}

void GameServer::guild_invite(Session& s, const std::string& name) {
  const Guild* g = guilds_.of(s.char_id);
  if (!g) return system_to(s, guild_error_text(GuildError::NotInGuild));
  if (const GuildError e = guild_can_invite(*g, content_.social, s.char_id); e != GuildError::Ok) return system_to(s, guild_error_text(e));
  if (name.empty()) return system_to(s, "Invite whom? Target a player or /guildinvite NAME.");
  Session* to = in_world_by_name(name);
  if (!to || to->social.ignores(s.char_name)) return system_to(s, auth::normalize_character_name(name) + " is not online.");
  if (to == &s) return system_to(s, guild_error_text(GuildError::Self));
  if (guilds_.of(to->char_id) || to->guild_pending) return system_to(s, to->char_name + " is already in a guild.");
  if (guilds_.invite_for(to->char_id)) return system_to(s, to->char_name + " is considering another guild invitation.");
  guilds_.invite(to->char_id, {g->id, s.char_id, s.char_name, now_ + static_cast<double>(content_.social.guild_invite_seconds)});
  send(to->peer, proto::SocialInvite{1, s.char_name, g->name, ms(content_.social.guild_invite_seconds)});
  system_to(*to, s.char_name + " invites you to join the guild <" + g->name + ">. (/guildjoin to accept, /guilddecline to refuse)");
  system_to(s, "You invite " + to->char_name + " to join <" + g->name + ">.");
}

bool GameServer::guild_busy(Session& s, std::int64_t guild_id) {
  if (!guild_busy_.contains(guild_id)) return false;
  system_to(s, "Your guild's roster is being updated. Try again in a moment.");
  return true;
}

void GameServer::guild_write(std::int64_t guild_id, net::PeerId actor, std::function<db::DbStatus(db::Repository&)> write,
                             std::function<void()> apply, std::function<void()> failed) {
  // Changes reach the database first; the roster in memory and the announcement follow once they are
  // stored. One change per guild at a time, so `apply` finds the guild as it was checked.
  guild_busy_.insert(guild_id);
  db_.post([this, guild_id, actor, write = std::move(write), apply = std::move(apply), failed = std::move(failed)](db::Repository& repo,
                                                                                                                db::DbWorker& w) mutable {
    const db::DbStatus st = write(repo);
    w.complete([this, guild_id, actor, st, apply = std::move(apply), failed = std::move(failed)] {
      guild_busy_.erase(guild_id);
      if (st == db::DbStatus::Ok) {
        apply();
        return;
      }
      spdlog::error("a change to guild {} could not be saved", guild_id);
      if (failed) failed();
      if (Session* a = find_session(actor); a && a->state == State::InWorld) system_to(*a, "That could not be saved. Please try again.");
    });
  });
}

void GameServer::guild_accept(Session& s) {
  const GuildInvite* pending = guilds_.invite_for(s.char_id);
  if (!pending) return system_to(s, "You have not been invited to a guild.");
  if (guild_busy(s, pending->guild)) return;  // the invitation stays for the retry
  if (guilds_.of(s.char_id) || s.guild_pending) return system_to(s, "You are already in a guild.");
  const GuildInvite inv = *pending;
  const Guild* g = guilds_.find(inv.guild);
  if (!g) {
    guilds_.take_invite(s.char_id);
    return system_to(s, "That guild no longer exists.");
  }
  if (static_cast<int>(g->members.size()) >= content_.social.guild_max_members) return system_to(s, guild_error_text(GuildError::Full));
  const PlayerEntity* p = player_of(s);
  if (!p) return;
  const int rank = lowest_rank(content_.social);
  const std::int64_t gid = g->id;
  const std::int64_t cid = s.char_id;
  GuildMember m{cid, s.char_name, rank, p->level, p->class_id};
  s.guild_pending = true;  // no founding or second joining meanwhile
  guild_write(
      gid, s.peer, [gid, cid, rank](db::Repository& repo) { return repo.set_guild_member(gid, cid, rank); },
      [this, gid, cid, m = std::move(m)] {
        Session* me = in_world_by_char(cid);
        if (me) me->guild_pending = false;
        if (const GuildInvite* left = guilds_.invite_for(cid); left && left->guild == gid) guilds_.take_invite(cid);
        Guild* gg = guilds_.find(gid);
        if (!gg) return;
        gg->members.push_back(m);
        guilds_.joined(gid, cid);
        guild_broadcast(*gg, m.name + " has joined the guild.");
        if (me) set_guild_tag(*me, gg->name);
        guild_dirty(gid);
      },
      [this, cid] {
        // Nothing was stored: the invitation (if it has not run out) is still there to accept again.
        if (Session* me = in_world_by_char(cid)) me->guild_pending = false;
      });
}

void GameServer::guild_decline(Session& s) {
  const auto inv = guilds_.take_invite(s.char_id);
  if (!inv) return system_to(s, "You have not been invited to a guild.");
  system_to(s, "You decline the guild invitation.");
  if (Session* f = in_world_by_char(inv->from)) system_to(*f, s.char_name + " declines your guild invitation.");
}

void GameServer::guild_leave(Session& s) {
  const Guild* g = guilds_.of(s.char_id);
  if (!g) return system_to(s, guild_error_text(GuildError::NotInGuild));
  if (guild_busy(s, g->id)) return;
  Guild check = *g;
  if (const GuildError e = game::guild_leave(check, s.char_id); e != GuildError::Ok) return system_to(s, guild_error_text(e));
  const std::int64_t gid = g->id;
  const std::int64_t cid = s.char_id;
  const bool empty = check.members.empty();
  guild_write(
      gid, s.peer,
      [cid, gid, empty](db::Repository& repo) {
        // The last member out takes the guild with them: one transaction removes the roster and the guild.
        return empty ? repo.delete_guild(gid) : repo.remove_guild_member(cid);
      },
      [this, gid, cid] {
        Guild* gg = guilds_.find(gid);
        if (!gg || game::guild_leave(*gg, cid) != GuildError::Ok) return;
        guilds_.left(cid);
        const std::string gname = gg->name;
        std::string who = "Someone";
        if (Session* me = in_world_by_char(cid)) {
          who = me->char_name;
          send_chat(*me, ChatChannel::Guild, "", "You leave <" + gname + ">.");
          set_guild_tag(*me, "");
          send_guild_info(*me);
        }
        if (gg->members.empty()) {
          spdlog::info("guild <{}> disbanded (its last member left)", gname);
          guilds_.remove(gid);
          return;
        }
        guild_broadcast(*gg, who + " has left the guild.");
        guild_dirty(gid);
      });
}

void GameServer::guild_remove(Session& s, const std::string& name) {
  const Guild* g = guilds_.of(s.char_id);
  if (!g) return system_to(s, guild_error_text(GuildError::NotInGuild));
  if (guild_busy(s, g->id)) return;
  const GuildMember* t = g->member_named(name);
  if (!t) return system_to(s, guild_error_text(GuildError::NotMember));
  const std::int64_t target = t->char_id;
  const std::string target_name = t->name;
  const std::int64_t actor = s.char_id;
  const std::string actor_name = s.char_name;
  Guild check = *g;
  if (const GuildError e = game::guild_remove(check, content_.social, actor, target); e != GuildError::Ok) return system_to(s, guild_error_text(e));
  const std::int64_t gid = g->id;
  guild_write(gid, s.peer, [target](db::Repository& repo) { return repo.remove_guild_member(target); },
              [this, gid, actor, target, target_name, actor_name] {
                Guild* gg = guilds_.find(gid);
                if (!gg || game::guild_remove(*gg, content_.social, actor, target) != GuildError::Ok) return;
                guilds_.left(target);
                if (Session* ts = in_world_by_char(target)) {
                  send_chat(*ts, ChatChannel::Guild, "", "You have been removed from <" + gg->name + ">.");
                  set_guild_tag(*ts, "");
                  send_guild_info(*ts);
                }
                guild_broadcast(*gg, target_name + " has been removed from the guild by " + actor_name + ".");
                guild_dirty(gid);
              });
}

void GameServer::guild_rank(Session& s, const std::string& name, int how) {
  const Guild* g = guilds_.of(s.char_id);
  if (!g) return system_to(s, guild_error_text(GuildError::NotInGuild));
  if (guild_busy(s, g->id)) return;
  const GuildMember* t = g->member_named(name);
  if (!t) return system_to(s, guild_error_text(GuildError::NotMember));
  const std::int64_t target = t->char_id;
  const std::int64_t actor = s.char_id;
  const SocialRules& R = content_.social;
  auto change = [&R, how, actor, target](Guild& gg) {
    if (how > 0) return guild_promote(gg, R, actor, target);
    if (how < 0) return guild_demote(gg, R, actor, target);
    return guild_pass_leadership(gg, actor, target);
  };
  Guild check = *g;
  if (const GuildError e = change(check); e != GuildError::Ok) return system_to(s, guild_error_text(e));
  const std::int64_t gid = g->id;
  std::vector<std::pair<std::int64_t, int>> ranks{{target, check.member(target)->rank}};
  if (how == 0) ranks.emplace_back(actor, check.member(actor)->rank);
  guild_write(
      gid, s.peer,
      [gid, ranks](db::Repository& repo) { return repo.set_guild_ranks(gid, ranks); },
      [this, gid, how, target, change] {
        Guild* gg = guilds_.find(gid);
        if (!gg || change(*gg) != GuildError::Ok) return;
        const GuildMember* now = gg->member(target);
        if (how == 0) guild_broadcast(*gg, now->name + " now leads the guild.");
        else guild_broadcast(*gg, now->name + " is now " + (how > 0 ? "promoted to " : "demoted to ") + rank_name(content_.social, now->rank) + ".");
        guild_dirty(gid);
      });
}

void GameServer::guild_motd(Session& s, const std::string& text) {
  const Guild* g = guilds_.of(s.char_id);
  if (!g) return system_to(s, guild_error_text(GuildError::NotInGuild));
  std::string motd = filter_->clean(auth::sanitize_chat(text));
  if (motd.empty()) return send_chat(s, ChatChannel::Guild, "", g->motd.empty() ? "Your guild has no message of the day." : "Guild message of the day: " + g->motd);
  if (auto mute = auth::mute_notice(s.muted_until, unix_now())) return system_to(s, *mute);  // a MOTD is speech too
  if (guild_busy(s, g->id)) return;
  motd.resize(net::utf8_truncate_len(motd, static_cast<std::size_t>(std::max(1, content_.social.motd_max))));
  Guild check = *g;
  if (const GuildError e = guild_set_motd(check, content_.social, s.char_id, motd); e != GuildError::Ok) return system_to(s, guild_error_text(e));
  const std::int64_t gid = g->id;
  const std::int64_t actor = s.char_id;
  const std::string actor_name = s.char_name;
  guild_write(gid, s.peer, [gid, motd](db::Repository& repo) { return repo.set_guild_motd(gid, motd); },
              [this, gid, actor, actor_name, motd] {
                Guild* gg = guilds_.find(gid);
                if (!gg || guild_set_motd(*gg, content_.social, actor, motd) != GuildError::Ok) return;
                guild_broadcast(*gg, "Guild message of the day (" + actor_name + "): " + motd);
                guild_dirty(gid);
              });
}

void GameServer::guild_disband(Session& s) {
  const Guild* g = guilds_.of(s.char_id);
  if (!g) return system_to(s, guild_error_text(GuildError::NotInGuild));
  if (guild_busy(s, g->id)) return;
  const GuildMember* me = g->member(s.char_id);
  if (!me || !guild_allows(content_.social, me->rank, GuildPerm::Disband)) return system_to(s, guild_error_text(GuildError::NoPermission));
  const std::int64_t gid = g->id;
  const std::string actor_name = s.char_name;
  guild_write(gid, s.peer, [gid](db::Repository& repo) { return repo.delete_guild(gid); },
              [this, gid, actor_name] {
                const Guild* gg = guilds_.find(gid);
                if (!gg) return;
                const std::string gname = gg->name;
                const std::vector<GuildMember> members = gg->members;
                guilds_.remove(gid);
                spdlog::info("'{}' disbanded the guild <{}>", actor_name, gname);
                for (const auto& m : members)
                  if (Session* ms = in_world_by_char(m.char_id)) {
                    send_chat(*ms, ChatChannel::Guild, "", "<" + gname + "> has been disbanded.");
                    set_guild_tag(*ms, "");
                    send_guild_info(*ms);
                  }
              });
}

void GameServer::guild_after_character_deleted(std::int64_t char_id) {
  // The character's roster row went with it (ON DELETE CASCADE); what follows tidies the guild. If one of
  // these writes fails, load_guilds puts it right at the next start (empty guilds are deleted, a
  // leaderless one gets a leader).
  Guild* g = guilds_.of(char_id);
  if (!g) return;
  std::erase_if(g->members, [&](const GuildMember& m) { return m.char_id == char_id; });
  guilds_.left(char_id);
  const std::int64_t gid = g->id;
  if (g->members.empty()) {
    db_.post([gid](db::Repository& repo, db::DbWorker&) {
      if (repo.delete_guild(gid) == db::DbStatus::Error) spdlog::error("deleting the empty guild {} failed; it is retried at the next start", gid);
    });
    guilds_.remove(gid);
    return;
  }
  if (g->leader_count() == 0) {
    auto top = std::min_element(g->members.begin(), g->members.end(), [](const GuildMember& a, const GuildMember& b) { return a.rank < b.rank; });
    top->rank = 0;
    const std::int64_t cid = top->char_id;
    db_.post([gid, cid](db::Repository& repo, db::DbWorker&) {
      if (repo.set_guild_member(gid, cid, 0) != db::DbStatus::Ok) spdlog::error("saving guild {}'s new leader failed; it is chosen again at the next start", gid);
    });
    guild_broadcast(*g, top->name + " now leads the guild.");
  }
  guild_dirty(gid);
}

// ------------------------------------------------------------------------- friends, ignore, channels

void GameServer::send_friends(Session& s) {
  s.friends_dirty = false;
  proto::FriendsList fl;
  for (const auto& name : s.social.friends) {
    proto::FriendInfo f;
    f.name = name;
    if (friend_visible(s, name)) {
      Session* o = in_world_by_name(name);
      f.online = true;
      if (const PlayerEntity* p = player_of(*o)) {
        f.level = static_cast<std::uint8_t>(std::clamp(p->level, 1, 255));
        f.class_id = p->class_id;
      }
      f.zone = zone_display(o->zone_id);
    }
    fl.friends.push_back(std::move(f));
  }
  fl.ignored = s.social.ignored;
  fl.muted_channels = s.social.muted;
  send(s.peer, fl);
}

void GameServer::notify_friends(const Session& s, bool online) {
  for (auto& [peer, o] : sessions_) {
    if (&o == &s || o.state != State::InWorld || !has_name(o.social.friends, s.char_name) || s.social.ignores(o.char_name)) continue;
    system_to(o, "Your friend " + s.char_name + (online ? " has entered the world." : " has left the world."));
    o.friends_dirty = true;
  }
}

bool GameServer::friend_visible(const Session& viewer, const std::string& name) {
  const Session* o = in_world_by_name(name);
  return o && !o->social.ignores(viewer.char_name);
}

void GameServer::friend_toggle(Session& s, const std::string& raw, int mode) {
  if (raw.empty()) {
    send_friends(s);
    std::string text = s.social.friends.empty() ? "Your friends list is empty. Add someone with /friend NAME." : "Friends:";
    for (const auto& f : s.social.friends) text += " " + f + (friend_visible(s, f) ? " (online)" : "");
    return system_to(s, text);
  }
  const std::string name = list_name(raw);
  if (name.empty()) return system_to(s, "That is not a character name.");
  if (name == s.char_name) return system_to(s, "You are always your own friend.");
  const bool listed = has_name(s.social.friends, name);
  if ((mode > 0 && listed) || (mode < 0 && !listed))
    return system_to(s, listed ? name + " is already on your friends list." : name + " is not on your friends list.");
  bool added = false;
  if (!toggle_name(s.social.friends, name, content_.social.max_friends, added))
    return system_to(s, "Your friends list is full (" + std::to_string(content_.social.max_friends) + ").");
  system_to(s, added ? name + " is now on your friends list." : name + " is no longer on your friends list.");
  mark_dirty(s);
  send_friends(s);
}

void GameServer::ignore_toggle(Session& s, const std::string& raw, int mode) {
  if (raw.empty()) {
    send_friends(s);
    std::string text = s.social.ignored.empty() ? "You are not ignoring anyone." : "Ignoring:";
    for (const auto& n : s.social.ignored) text += " " + n;
    return system_to(s, text);
  }
  const std::string name = list_name(raw);
  if (name.empty()) return system_to(s, "That is not a character name.");
  if (name == s.char_name) return system_to(s, "You cannot ignore yourself.");
  const bool listed = s.social.ignores(name);
  if ((mode > 0 && listed) || (mode < 0 && !listed))
    return system_to(s, listed ? "You are already ignoring " + name + "." : "You are not ignoring " + name + ".");
  bool added = false;
  if (!toggle_name(s.social.ignored, name, content_.social.max_ignored, added))
    return system_to(s, "Your ignore list is full (" + std::to_string(content_.social.max_ignored) + ").");
  system_to(s, added ? "You are now ignoring " + name + "." : "You are no longer ignoring " + name + ".");
  mark_dirty(s);
  send_friends(s);
  if (Session* o = in_world_by_name(name)) o->friends_dirty = true;  // their friend list shows you offline now
}

void GameServer::channel_toggle(Session& s, const Command& c) {
  if (c.args.empty()) {
    std::string text = "Channels:";
    for (ChatChannel ch : {ChatChannel::Say, ChatChannel::Shout, ChatChannel::Ooc, ChatChannel::Emote, ChatChannel::Guild})
      text += std::string(" ") + channel_name(ch) + (s.social.hears(ch) ? " on" : " off");
    return system_to(s, text + ". Turn one off with /channel NAME off.");
  }
  const auto ch = channel_by_name(c.arg(0));
  if (!ch || !mutable_channel(*ch)) return system_to(s, "You can turn say, shout, ooc, emote and guild on or off.");
  const std::string want = lower(c.arg(1));
  bool on = !s.social.hears(*ch);
  if (want == "on") on = true;
  else if (want == "off") on = false;
  else if (!want.empty()) return system_to(s, "Usage: /channel NAME on|off");
  if (on) s.social.muted = static_cast<std::uint16_t>(s.social.muted & ~chat_bit(*ch));
  else s.social.muted = static_cast<std::uint16_t>(s.social.muted | chat_bit(*ch));
  system_to(s, std::string("You turn ") + channel_name(*ch) + (on ? " on." : " off."));
  mark_dirty(s);
  send_friends(s);
}

// ------------------------------------------------------------------------- /who and /assist

void GameServer::who(Session& s, const Command& c) {
  WhoQuery q;
  if (auto err = parse_who(c.args, content_, q)) return system_to(s, *err);
  std::vector<proto::WhoEntry> found;
  std::size_t total = 0;
  for (auto& [peer, o] : sessions_) {
    if (o.state != State::InWorld) continue;
    const PlayerEntity* p = player_of(o);
    if (!p) continue;
    const Guild* g = guilds_.of(o.char_id);
    WhoCandidate cand{o.char_name, p->level, p->class_id, o.race, o.zone_id, g ? g->name : std::string(), o.is_admin};
    if (!who_matches(q, cand, s.zone_id)) continue;
    ++total;
    const Group* grp = groups_.group_of(o.char_id);
    found.push_back({o.char_name, static_cast<std::uint8_t>(std::clamp(p->level, 1, 255)), p->class_id, o.race, zone_display(o.zone_id), cand.guild,
                     o.is_admin, static_cast<std::uint8_t>(grp ? grp->members.size() : 0)});
  }
  std::sort(found.begin(), found.end(), [](const proto::WhoEntry& a, const proto::WhoEntry& b) { return a.name < b.name; });
  if (found.size() > static_cast<std::size_t>(content_.social.who_max_results)) found.resize(static_cast<std::size_t>(content_.social.who_max_results));
  proto::WhoResults r;
  r.query = q.describe();
  r.total = static_cast<std::uint16_t>(std::min<std::size_t>(total, 0xFFFF));
  r.entries = std::move(found);
  const std::size_t shown = r.entries.size();
  send(s.peer, r);
  std::string where = q.all ? "in the world" : "in " + zone_display(s.zone_id);
  system_to(s, std::to_string(total) + " player" + (total == 1 ? "" : "s") + " found " + where +
                   (shown < total ? " (showing " + std::to_string(shown) + ")." : "."));
}

void GameServer::assist(Session& s, const Command& c) {
  Zone* z = zone_for(s);
  PlayerEntity* me = player_of(s);
  if (!z || !me) return;
  EntityId helped = 0;
  if (!c.args.empty()) {
    const std::string want = lower(c.arg(0));
    z->for_each_player([&](const PlayerEntity& p) {
      if (lower(p.name) == want) helped = p.id;
    });
    if (helped == 0) return system_to(s, auth::normalize_character_name(c.arg(0)) + " is not here.");
  } else if (me->target != 0 && z->find(me->target)) {
    helped = me->target;
  }
  z->assist(s.entity, helped, *this);
}

}  // namespace ef::game
