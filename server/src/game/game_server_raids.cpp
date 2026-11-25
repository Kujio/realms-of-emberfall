// GameServer, Phase 14: raids. The rules (who may invite, join, lead and leave) live in the pure
// RaidRegistry (game/raids.cpp); zones read it for shared experience, kill credit and loot. This file
// routes the commands, keeps the raid in step with its groups, sends RaidState and saves every raid.
#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "auth/names.hpp"
#include "game/game_server.hpp"

namespace ef::game {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
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

const char* loot_text(RaidLoot l) {
  return l == RaidLoot::Master ? "The raid leader holds the loot and hands it out (master loot)."
                               : "The raid rolls need before greed for loot.";
}

}  // namespace

// ------------------------------------------------------------------------- commands

bool GameServer::raid_command(Session& s, const Command& c) {
  const std::string& n = c.name;
  auto is = [&](std::initializer_list<const char*> names) {
    return std::any_of(names.begin(), names.end(), [&](const char* x) { return n == x; });
  };
  if (is({"rsay", "rs", "raidsay"})) return chat(s, ChatChannel::Raid, "", c.rest), true;
  if (is({"raidinvite", "rinvite", "rinv"})) return raid_invite(s, name_or_target(s, c)), true;
  if (is({"raidaccept", "raidjoin", "rjoin"})) return raid_accept(s), true;
  if (is({"raiddecline", "rdecline"})) return raid_decline(s), true;
  if (is({"raidleave", "rleave"})) return raid_leave(s), true;
  if (is({"raidkick", "rkick"})) return raid_kick(s, name_or_target(s, c)), true;
  if (is({"raidleader", "rleader"})) return raid_make_leader(s, name_or_target(s, c)), true;
  if (is({"raiddisband", "rdisband"})) return raid_disband(s), true;
  if (is({"raidloot", "rloot"})) {
    const std::string r = lower(c.arg(0));
    if (r == "master" || r == "ml" || r == "m") return raid_loot(s, RaidLoot::Master), true;
    if (r == "ng" || r == "need" || r == "needgreed" || r == "greed") return raid_loot(s, RaidLoot::NeedGreed), true;
    return system_to(s, "Usage: /raidloot master (the leader hands out loot) | ng (need before greed)"), true;
  }
  if (is({"raid", "raidinfo"})) return raid_info(s), true;
  return false;
}

void GameServer::raid_invite(Session& s, const std::string& name) {
  if (name.empty()) return system_to(s, "Invite whom to the raid? Target a group leader or /raidinvite NAME.");
  if (!groups_loaded_ || !raids_loaded_) return system_to(s, "Groups and raids are still being restored. Try again in a moment.");
  Session* to = in_world_by_name(name);
  if (!to || to->social.ignores(s.char_name)) return system_to(s, auth::normalize_character_name(name) + " is not online.");
  if (to == &s) return system_to(s, "You cannot invite yourself.");
  const RaidChange c = raids_.invite(groups_, s.char_id, s.char_name, to->char_id, now_ + static_cast<double>(content_.social.raid_invite_seconds));
  if (!c.ok()) return system_to(s, raid_error_text(c.error));
  send(to->peer, proto::SocialInvite{2, s.char_name, "", static_cast<std::uint32_t>(std::lround(content_.social.raid_invite_seconds * 1000.0f))});
  system_to(*to, s.char_name + " invites your group to join a raid. (/raidaccept to accept, /raiddecline to refuse)");
  system_to(s, "You invite " + to->char_name + "'s group to join your raid.");
}

void GameServer::raid_accept(Session& s) {
  if (!raids_loaded_) return system_to(s, "Raids are still being restored. Try again in a moment.");
  const RaidInvite* inv = raids_.invite_for(s.char_id);
  if (!inv) return system_to(s, raid_error_text(RaidError::NoInvite));
  const std::string from_name = inv->from_name;
  if (!in_world_by_char(inv->from)) {
    raids_.decline(s.char_id);
    return system_to(s, "Whoever invited you is no longer in the world.");
  }
  const RaidChange c = raids_.accept(groups_, s.char_id);
  if (!c.ok()) return system_to(s, raid_error_text(c.error));
  system_to(s, "Your group joins " + from_name + "'s raid.");
  raid_changed(c, s.char_name + "'s group has joined the raid.", s.char_id);
}

void GameServer::raid_decline(Session& s) {
  const std::int64_t from = raids_.decline(s.char_id);
  if (from == 0) return system_to(s, raid_error_text(RaidError::NoInvite));
  system_to(s, "You decline the raid invitation.");
  if (Session* f = in_world_by_char(from)) system_to(*f, s.char_name + " declines your raid invitation.");
}

void GameServer::raid_leave(Session& s) {
  const RaidChange c = raids_.leave(groups_, s.char_id);
  if (!c.ok()) {
    if (c.error == RaidError::NotGroupLeader)
      return system_to(s, "Only your group leader can take the group out of the raid. Leave your group (/disband) to leave the raid.");
    return system_to(s, raid_error_text(c.error));
  }
  system_to(s, "Your group leaves the raid.");
  raid_changed(c, c.disbanded ? "The raid has disbanded." : s.char_name + "'s group has left the raid.", s.char_id);
}

void GameServer::raid_kick(Session& s, const std::string& name) {
  const Raid* r = raids_.raid_of(groups_, s.char_id);
  if (!r) return system_to(s, raid_error_text(RaidError::NotInRaid));
  if (name.empty()) return system_to(s, "Remove whose group? /raidkick NAME");
  // Anyone in the group names it: the whole group goes.
  std::uint32_t group = 0;
  std::string target_name;
  for (const std::uint32_t gid : r->groups)
    if (const Group* g = groups_.find(gid))
      for (const auto& m : g->members)
        if (lower(m.name) == lower(name)) {
          group = gid;
          target_name = m.name;
        }
  if (group == 0) return system_to(s, raid_error_text(RaidError::NotMember));
  const Group* g = groups_.find(group);
  const std::vector<GroupMemberRef> removed = g ? g->members : std::vector<GroupMemberRef>{};
  const RaidChange c = raids_.kick(groups_, s.char_id, group);
  if (!c.ok()) {
    if (c.error == RaidError::SameGroup) return system_to(s, "That is your own group. Disband the raid (/raiddisband) or hand it over first.");
    return system_to(s, raid_error_text(c.error));
  }
  for (const auto& m : removed)
    if (Session* t = in_world_by_char(m.char_id)) system_to(*t, "Your group has been removed from the raid.");
  raid_changed(c, c.disbanded ? "The raid has disbanded." : target_name + "'s group has been removed from the raid.", s.char_id);
}

void GameServer::raid_make_leader(Session& s, const std::string& name) {
  const Raid* r = raids_.raid_of(groups_, s.char_id);
  if (!r) return system_to(s, raid_error_text(RaidError::NotInRaid));
  std::int64_t target = 0;
  std::string target_name;
  for (const std::int64_t cid : RaidRegistry::members(*r, groups_))
    if (const Group* g = groups_.group_of(cid))
      if (const GroupMemberRef* m = g->member(cid); m && lower(m->name) == lower(name)) {
        target = cid;
        target_name = m->name;
      }
  if (target == 0) return system_to(s, raid_error_text(RaidError::NotMember));
  const RaidChange c = raids_.make_leader(groups_, s.char_id, target);
  if (!c.ok()) return system_to(s, raid_error_text(c.error));
  raid_changed(c, target_name + " now leads the raid.");
}

void GameServer::raid_loot(Session& s, RaidLoot loot) {
  const RaidChange c = raids_.set_loot(groups_, s.char_id, loot);
  if (!c.ok()) return system_to(s, raid_error_text(c.error));
  raid_changed(c, loot_text(loot));
}

void GameServer::raid_disband(Session& s) {
  const RaidChange c = raids_.disband(groups_, s.char_id);
  if (!c.ok()) return system_to(s, raid_error_text(c.error));
  system_to(s, "You disband the raid.");
  raid_changed(c, "The raid has disbanded.", s.char_id);
}

void GameServer::raid_info(Session& s) {
  const Raid* r = raids_.raid_of(groups_, s.char_id);
  if (!r) return system_to(s, "You are not in a raid. Group leaders form one with /raidinvite NAME (another group's leader).");
  std::size_t members = 0;
  std::size_t online = 0;
  for (const std::uint32_t gid : r->groups)
    if (const Group* g = groups_.find(gid)) {
      members += g->members.size();
      online += g->online_count();
    }
  const Group* lg = groups_.group_of(r->leader);
  const GroupMemberRef* lead = lg ? lg->member(r->leader) : nullptr;
  send_raid_state(s, true);
  system_to(s, "Raid: " + std::to_string(r->groups.size()) + " of " + std::to_string(raids_.max_groups()) + " groups, " + std::to_string(online) +
                   " of " + std::to_string(members) + " members online. Leader: " + (lead ? lead->name : std::string("?")) + ". " +
                   loot_text(r->loot));
}

// ------------------------------------------------------------------------- state, saving

void GameServer::raid_changed(const RaidChange& c, const std::string& message, std::int64_t except) {
  if (c.ok()) persist_raid(c.id());
  for (const std::uint32_t gid : c.groups)
    if (const Group* g = groups_.find(gid))
      for (const auto& m : g->members)
        if (Session* ms = in_world_by_char(m.char_id)) {
          if (!message.empty() && m.char_id != except) send_chat(*ms, ChatChannel::Raid, "", message);
          send_raid_state(*ms, true);
        }
}

void GameServer::raids_reconcile() {
  for (const RaidChange& c : raids_.reconcile(groups_)) {
    std::string message;
    if (c.disbanded) message = "The raid has disbanded.";
    else if (const Raid* r = raids_.find(c.raid))
      if (const Group* g = groups_.group_of(r->leader))
        if (const GroupMemberRef* m = g->member(r->leader)) message = m->name + " now leads the raid.";
    raid_changed(c, message);
  }
}

void GameServer::send_raid_state(Session& s, bool force) {
  proto::RaidState st;
  st.max_groups = static_cast<std::uint8_t>(raids_.max_groups());
  if (const Raid* r = raids_.raid_of(groups_, s.char_id)) {
    st.raid_id = r->id;
    st.loot_rule = static_cast<std::uint8_t>(r->loot);
    if (const Group* lg = groups_.group_of(r->leader))
      if (const GroupMemberRef* lead = lg->member(r->leader)) st.leader = lead->name;
    for (std::size_t gi = 0; gi < r->groups.size(); ++gi) {
      const Group* g = groups_.find(r->groups[gi]);
      if (!g) continue;
      for (const auto& m : g->members) {
        if (st.members.size() >= 72) break;
        proto::RaidMember rm;
        rm.group = static_cast<std::uint8_t>(gi + 1);
        rm.name = m.name;
        rm.class_id = m.class_id;
        rm.level = static_cast<std::uint8_t>(std::clamp(m.level, 0, 255));
        rm.group_leader = g->leader == m.char_id;
        rm.online = m.online;
        if (Session* ms = m.online ? in_world_by_char(m.char_id) : nullptr)
          if (const PlayerEntity* p = player_of(*ms)) {
            rm.class_id = p->class_id;
            rm.level = static_cast<std::uint8_t>(std::clamp(p->level, 1, 255));
            rm.entity_id = ms->zone_id == s.zone_id ? p->id : 0;
            rm.hp_pct = p->dead ? 0 : pct(p->hp, p->stats.max_hp);
            rm.dead = p->dead;
          }
        st.members.push_back(std::move(rm));
      }
    }
  }
  proto::encode_packet_into(st, send_buf_);
  const std::uint64_t h = hash_bytes(send_buf_);
  if (!force && h == s.raid_hash) return;
  s.raid_hash = h;
  transport_.send(s.peer, proto::RaidState::kChannel, send_buf_, true);
}

void GameServer::persist_raid(std::uint32_t id) {
  if (id == 0) return;
  const Raid* r = raids_.find(id);
  raid_write_failed_.erase(id);
  // As with groups: a failed write is retried from the raid as it is by then, never an older snapshot.
  if (!r) {
    db_.post([this, id](db::Repository& repo, db::DbWorker& w) {
      if (repo.delete_raid(id) != db::DbStatus::Error) return;
      spdlog::error("forgetting raid {} failed; retrying", id);
      w.complete([this, id] { raid_write_failed_.insert(id); });
    });
    return;
  }
  db::RaidRecord rec;
  rec.id = r->id;
  rec.leader = r->leader;
  rec.loot_rule = static_cast<int>(r->loot);
  for (const std::uint32_t g : r->groups) rec.groups.push_back(g);
  db_.post([this, rec = std::move(rec)](db::Repository& repo, db::DbWorker& w) {
    if (repo.save_raid(rec) != db::DbStatus::Error) return;
    spdlog::error("saving raid {} failed; retrying", rec.id);
    w.complete([this, id = static_cast<std::uint32_t>(rec.id)] { raid_write_failed_.insert(id); });
  });
}

void GameServer::load_raids() {
  raids_loading_ = true;
  db_.post([this](db::Repository& repo, db::DbWorker& w) {
    std::optional<std::vector<db::RaidRecord>> rows;
    try {
      rows = repo.load_raids();
    } catch (const std::exception& e) {
      spdlog::error("load_raids: {}", e.what());
    }
    w.complete([this, rows = std::move(rows)] {
      raids_loading_ = false;
      if (!rows) {
        // Tried again shortly (raid_tick); nobody forms a raid meanwhile, so no saved id is taken.
        spdlog::error("loading raids failed; retrying in 10 s");
        raid_retry_tick_ = tick_ + 10 * std::max<std::uint32_t>(1, cfg_.tick_rate);
        return;
      }
      raids_loaded_ = true;
      int restored = 0;
      for (const auto& rec : *rows) {
        Raid r;
        r.id = static_cast<std::uint32_t>(rec.id);
        r.leader = rec.leader;
        r.loot = rec.loot_rule == static_cast<int>(RaidLoot::NeedGreed) ? RaidLoot::NeedGreed : RaidLoot::Master;
        for (const std::int64_t g : rec.groups)
          if (g > 0 && g <= 0xFFFFFFFFLL) r.groups.push_back(static_cast<std::uint32_t>(g));
        if (rec.id <= 0 || rec.id > 0xFFFFFFFFLL || !raids_.restore(std::move(r), groups_)) {
          const std::int64_t rid = rec.id;
          db_.post([rid](db::Repository& rp, db::DbWorker&) { rp.delete_raid(rid); });
          continue;
        }
        persist_raid(static_cast<std::uint32_t>(rec.id));  // as restored (groups that are gone dropped)
        ++restored;
      }
      if (restored > 0) spdlog::info("restored {} raid(s)", restored);
      raids_reconcile();
      for (auto& [peer, s] : sessions_)
        if (s.state == State::InWorld && raids_.raid_of(groups_, s.char_id)) send_raid_state(s, true);
    });
  });
}

void GameServer::raid_tick() {
  if (groups_loaded_ && !raids_loaded_ && !raids_loading_ && raid_retry_tick_ != 0 && tick_ >= raid_retry_tick_) load_raids();
  if (!raid_write_failed_.empty()) {
    const std::set<std::uint32_t> retry = std::move(raid_write_failed_);
    raid_write_failed_.clear();
    for (const std::uint32_t id : retry) persist_raid(id);
  }
  for (const auto& [to, inv] : raids_.expire_invites(now_)) {
    if (Session* t = in_world_by_char(to)) system_to(*t, "The raid invitation from " + inv.from_name + " has expired.");
    if (Session* f = in_world_by_char(inv.from)) system_to(*f, "Your raid invitation was not answered.");
  }
  for (auto& [peer, s] : sessions_)
    if (s.state == State::InWorld && raids_.raid_of(groups_, s.char_id)) send_raid_state(s, false);
}

// ------------------------------------------------------------------------- master loot

void GameServer::handle(Session& s, const proto::LootGive& m) {
  if (Zone* z = item_action(s, "loot give in wrong state")) z->loot_give(s.entity, m.corpse_id, m.slot, m.to, *this);
}

}  // namespace ef::game
