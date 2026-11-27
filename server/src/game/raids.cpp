#include "game/raids.hpp"

#include <algorithm>

namespace ef::game {

bool Raid::has_group(std::uint32_t g) const { return std::find(groups.begin(), groups.end(), g) != groups.end(); }

const char* raid_error_text(RaidError e) {
  switch (e) {
    case RaidError::Ok: return "";
    case RaidError::NotInGroup: return "You must be in a group to raid. Raids are made of whole groups.";
    case RaidError::NotGroupLeader: return "Only your group leader can do that.";
    case RaidError::NotInRaid: return "You are not in a raid.";
    case RaidError::NotRaidLeader: return "Only the raid leader can do that.";
    case RaidError::Full: return "The raid is full.";
    case RaidError::TargetNotGrouped: return "They are not in a group. Raids are made of whole groups: invite them to a group in the raid.";
    case RaidError::TargetNotLeader: return "They do not lead their group. Invite their group leader.";
    case RaidError::TargetInRaid: return "Their group is already in a raid.";
    case RaidError::SameGroup: return "They are in your own group.";
    case RaidError::AlreadyInvited: return "They are considering another raid invitation.";
    case RaidError::NoInvite: return "You have no raid invitation (or it is no longer good).";
    case RaidError::NotMember: return "They are not in your raid.";
    case RaidError::TargetOffline: return "They are offline.";
    case RaidError::NotMarker: return "Only the raid leader or main assist can mark targets.";
    case RaidError::BadMark: return "Marks are numbered 1 to 3.";
  }
  return "";
}

namespace {

RaidChange fail(RaidError e) {
  RaidChange c;
  c.error = e;
  return c;
}

RaidChange touched(const Raid& r) {
  RaidChange c;
  c.raid = r.id;
  c.groups = r.groups;
  return c;
}

void add_group(std::vector<std::uint32_t>& to, std::uint32_t g) {
  if (std::find(to.begin(), to.end(), g) == to.end()) to.push_back(g);
}

}  // namespace

std::vector<std::int64_t> RaidRegistry::members(const Raid& r, const GroupRegistry& groups) {
  std::vector<std::int64_t> out;
  for (const std::uint32_t gid : r.groups)
    if (const Group* g = groups.find(gid))
      for (const auto& m : g->members) out.push_back(m.char_id);
  return out;
}

bool RaidRegistry::in_raid(const Raid& r, const GroupRegistry& groups, std::int64_t char_id) {
  const Group* g = groups.group_of(char_id);
  return g && r.has_group(g->id);
}

std::int64_t RaidRegistry::first_online(const Raid& r, const GroupRegistry& groups) {
  for (const std::uint32_t gid : r.groups)
    if (const Group* g = groups.find(gid)) {
      // A group's own leader first, then its other members.
      if (const GroupMemberRef* lead = g->member(g->leader); lead && lead->online) return lead->char_id;
      for (const auto& m : g->members)
        if (m.online) return m.char_id;
    }
  return 0;
}

Raid* RaidRegistry::mutable_raid_of_group(std::uint32_t group) {
  auto it = raid_of_.find(group);
  if (it == raid_of_.end()) return nullptr;
  auto r = raids_.find(it->second);
  return r == raids_.end() ? nullptr : &r->second;
}

const Raid* RaidRegistry::raid_of_group(std::uint32_t group) const {
  auto it = raid_of_.find(group);
  if (it == raid_of_.end()) return nullptr;
  auto r = raids_.find(it->second);
  return r == raids_.end() ? nullptr : &r->second;
}

const Raid* RaidRegistry::raid_of(const GroupRegistry& groups, std::int64_t char_id) const {
  const Group* g = groups.group_of(char_id);
  return g ? raid_of_group(g->id) : nullptr;
}

const Raid* RaidRegistry::find(std::uint32_t id) const {
  auto it = raids_.find(id);
  return it == raids_.end() ? nullptr : &it->second;
}

const RaidInvite* RaidRegistry::invite_for(std::int64_t char_id) const {
  auto it = invites_.find(char_id);
  return it == invites_.end() ? nullptr : &it->second;
}

RaidChange RaidRegistry::invite(const GroupRegistry& groups, std::int64_t from, const std::string& from_name, std::int64_t to, double expires_at) {
  const Group* mine = groups.group_of(from);
  if (!mine) return fail(RaidError::NotInGroup);
  RaidChange c;
  if (const Raid* r = raid_of_group(mine->id)) {
    if (r->leader != from) return fail(RaidError::NotRaidLeader);
    if (static_cast<int>(r->groups.size()) >= max_) return fail(RaidError::Full);
    c.raid = r->id;
  } else if (mine->leader != from) {
    return fail(RaidError::NotGroupLeader);
  }
  const Group* theirs = groups.group_of(to);
  if (!theirs) return fail(RaidError::TargetNotGrouped);
  if (theirs->id == mine->id) return fail(RaidError::SameGroup);
  if (theirs->leader != to) return fail(RaidError::TargetNotLeader);
  if (raid_of_group(theirs->id)) return fail(RaidError::TargetInRaid);
  if (invites_.contains(to)) return fail(RaidError::AlreadyInvited);
  invites_[to] = {from, from_name, expires_at, c.raid, theirs->id};
  return c;
}

RaidChange RaidRegistry::accept(const GroupRegistry& groups, std::int64_t who, double now) {
  auto it = invites_.find(who);
  if (it == invites_.end()) return fail(RaidError::NoInvite);
  const RaidInvite inv = it->second;
  invites_.erase(it);
  if (inv.expires_at <= now) return fail(RaidError::NoInvite);
  // Both sides are checked again: groups and raids may have changed while the invitation waited.
  // The invitation was for the group they led then: they must still lead that same group.
  const Group* theirs = groups.group_of(who);
  if (!theirs) return fail(RaidError::NotInGroup);
  if (theirs->id != inv.group) return fail(RaidError::NoInvite);
  if (theirs->leader != who) return fail(RaidError::NotGroupLeader);
  if (raid_of_group(theirs->id)) return fail(RaidError::TargetInRaid);
  const Group* mine = groups.group_of(inv.from);
  if (!mine || mine->id == theirs->id) return fail(RaidError::NoInvite);
  Raid* r = mutable_raid_of_group(mine->id);
  // An invitation to a raid is good only while that raid stands with the inviter leading it; one sent
  // to found a raid only while the inviter is still in none (a raid that disbanded is not refounded).
  if (inv.raid != 0) {
    if (!r || r->id != inv.raid || r->leader != inv.from) return fail(RaidError::NoInvite);
    if (static_cast<int>(r->groups.size()) >= max_) return fail(RaidError::Full);
  } else {
    if (r || mine->leader != inv.from) return fail(RaidError::NoInvite);
    Raid fresh;
    while (next_id_ == 0 || raids_.contains(next_id_)) ++next_id_;  // restored raids keep their ids
    fresh.id = next_id_++;
    fresh.leader = inv.from;
    fresh.groups.push_back(mine->id);
    r = &raids_.emplace(fresh.id, std::move(fresh)).first->second;
    raid_of_[mine->id] = r->id;
  }
  r->groups.push_back(theirs->id);
  raid_of_[theirs->id] = r->id;
  return touched(*r);
}

std::int64_t RaidRegistry::decline(std::int64_t who) {
  auto it = invites_.find(who);
  if (it == invites_.end()) return 0;
  const std::int64_t from = it->second.from;
  invites_.erase(it);
  return from;
}

RaidChange RaidRegistry::dissolve(Raid& r) {
  RaidChange c;
  c.groups = r.groups;
  c.disbanded = true;
  c.gone = r.id;
  for (const std::uint32_t g : r.groups) raid_of_.erase(g);
  const std::uint32_t id = r.id;  // a copy: `r` lives in the node being erased
  raids_.erase(id);
  return c;
}

RaidChange RaidRegistry::remove_group(Raid& r, std::uint32_t group, const GroupRegistry& groups) {
  std::vector<std::uint32_t> before = r.groups;
  r.groups.erase(std::remove(r.groups.begin(), r.groups.end(), group), r.groups.end());
  raid_of_.erase(group);
  if (r.groups.size() < 2) {
    RaidChange c = dissolve(r);
    c.groups = before;
    return c;
  }
  if (!in_raid(r, groups, r.leader)) {
    const std::int64_t next = first_online(r, groups);
    if (next != 0) r.leader = next;
    else if (const Group* g = groups.find(r.groups.front())) r.leader = g->leader;
  }
  if (r.assist != 0 && !in_raid(r, groups, r.assist)) r.assist = 0;
  RaidChange c = touched(r);
  add_group(c.groups, group);
  return c;
}

RaidChange RaidRegistry::leave(const GroupRegistry& groups, std::int64_t who) {
  const Group* g = groups.group_of(who);
  if (!g) return fail(RaidError::NotInRaid);
  Raid* r = mutable_raid_of_group(g->id);
  if (!r) return fail(RaidError::NotInRaid);
  if (g->leader != who) return fail(RaidError::NotGroupLeader);
  return remove_group(*r, g->id, groups);
}

RaidChange RaidRegistry::kick(const GroupRegistry& groups, std::int64_t leader, std::uint32_t group) {
  const Group* mine = groups.group_of(leader);
  Raid* r = mine ? mutable_raid_of_group(mine->id) : nullptr;
  if (!r) return fail(RaidError::NotInRaid);
  if (r->leader != leader) return fail(RaidError::NotRaidLeader);
  if (!r->has_group(group)) return fail(RaidError::NotMember);
  if (group == mine->id) return fail(RaidError::SameGroup);
  return remove_group(*r, group, groups);
}

RaidChange RaidRegistry::make_leader(const GroupRegistry& groups, std::int64_t leader, std::int64_t target) {
  const Group* mine = groups.group_of(leader);
  Raid* r = mine ? mutable_raid_of_group(mine->id) : nullptr;
  if (!r) return fail(RaidError::NotInRaid);
  if (r->leader != leader) return fail(RaidError::NotRaidLeader);
  if (!in_raid(*r, groups, target)) return fail(RaidError::NotMember);
  const Group* tg = groups.group_of(target);
  const GroupMemberRef* m = tg ? tg->member(target) : nullptr;
  if (!m || !m->online) return fail(RaidError::TargetOffline);
  r->leader = target;
  return touched(*r);
}

RaidChange RaidRegistry::set_loot(const GroupRegistry& groups, std::int64_t leader, RaidLoot loot) {
  const Group* mine = groups.group_of(leader);
  Raid* r = mine ? mutable_raid_of_group(mine->id) : nullptr;
  if (!r) return fail(RaidError::NotInRaid);
  if (r->leader != leader) return fail(RaidError::NotRaidLeader);
  r->loot = loot;
  return touched(*r);
}

RaidChange RaidRegistry::disband(const GroupRegistry& groups, std::int64_t leader) {
  const Group* mine = groups.group_of(leader);
  Raid* r = mine ? mutable_raid_of_group(mine->id) : nullptr;
  if (!r) return fail(RaidError::NotInRaid);
  if (r->leader != leader) return fail(RaidError::NotRaidLeader);
  return dissolve(*r);
}

RaidChange RaidRegistry::set_assist(const GroupRegistry& groups, std::int64_t leader, std::int64_t target) {
  const Group* mine = groups.group_of(leader);
  Raid* r = mine ? mutable_raid_of_group(mine->id) : nullptr;
  if (!r) return fail(RaidError::NotInRaid);
  if (r->leader != leader) return fail(RaidError::NotRaidLeader);
  if (target != 0 && !in_raid(*r, groups, target)) return fail(RaidError::NotMember);
  r->assist = target;
  return touched(*r);
}

Raid* RaidRegistry::marker_raid(const GroupRegistry& groups, std::int64_t who, RaidError& error) {
  const Group* mine = groups.group_of(who);
  Raid* r = mine ? mutable_raid_of_group(mine->id) : nullptr;
  error = !r ? RaidError::NotInRaid : (r->leader != who && r->assist != who ? RaidError::NotMarker : RaidError::Ok);
  return error == RaidError::Ok ? r : nullptr;
}

RaidChange RaidRegistry::set_mark(const GroupRegistry& groups, std::int64_t who, int slot, const std::string& zone, std::uint32_t entity) {
  RaidError error = RaidError::Ok;
  Raid* r = marker_raid(groups, who, error);
  if (!r) return fail(error);
  if (slot < 1 || slot > kRaidMarks) return fail(RaidError::BadMark);
  if (entity != 0)
    for (RaidMark& m : r->marks)
      if (m.entity == entity && m.zone == zone) m = {};
  r->marks[static_cast<std::size_t>(slot - 1)] = entity != 0 ? RaidMark{zone, entity} : RaidMark{};
  return touched(*r);
}

RaidChange RaidRegistry::clear_marks(const GroupRegistry& groups, std::int64_t who) {
  RaidError error = RaidError::Ok;
  Raid* r = marker_raid(groups, who, error);
  if (!r) return fail(error);
  r->marks = {};
  return touched(*r);
}

std::size_t RaidRegistry::forget_zone_marks(const std::string& zone) {
  std::size_t n = 0;
  for (auto& [id, r] : raids_)
    for (RaidMark& m : r.marks)
      if (m.entity != 0 && m.zone == zone) {
        m = {};
        ++n;
      }
  return n;
}

std::vector<RaidChange> RaidRegistry::forget_marks_on(const std::string& zone, std::uint32_t entity) {
  std::vector<RaidChange> out;
  if (entity == 0) return out;
  for (auto& [id, r] : raids_) {
    bool hit = false;
    for (RaidMark& m : r.marks)
      if (m.entity == entity && m.zone == zone) {
        m = {};
        hit = true;
      }
    if (hit) out.push_back(touched(r));
  }
  return out;
}

std::vector<RaidChange> RaidRegistry::reconcile(const GroupRegistry& groups) {
  std::vector<RaidChange> out;
  std::vector<std::uint32_t> ids;
  for (const auto& [id, r] : raids_) ids.push_back(id);
  for (const std::uint32_t id : ids) {
    auto it = raids_.find(id);
    if (it == raids_.end()) continue;
    Raid& r = it->second;
    const std::vector<std::uint32_t> before = r.groups;
    bool changed = false;
    // Groups that disbanded leave the raid.
    for (const std::uint32_t g : before)
      if (!groups.find(g)) {
        r.groups.erase(std::remove(r.groups.begin(), r.groups.end(), g), r.groups.end());
        raid_of_.erase(g);
        changed = true;
      }
    if (r.groups.size() < 2) {
      RaidChange c = dissolve(r);
      c.groups = before;
      out.push_back(std::move(c));
      continue;
    }
    // The leader must be in the raid and online; otherwise the lead passes (it stays put while nobody
    // at all is online, so a raid kept across a restart keeps its leader).
    const Group* lg = groups.group_of(r.leader);
    const GroupMemberRef* lm = lg && r.has_group(lg->id) ? lg->member(r.leader) : nullptr;
    if (!lm || !lm->online) {
      std::int64_t next = first_online(r, groups);
      if (next == 0 && !lm)
        if (const Group* g = groups.find(r.groups.front())) next = g->leader;
      if (next != 0 && next != r.leader) {
        r.leader = next;
        changed = true;
      }
    }
    // A main assist who left the raid is no longer one (one who is offline keeps the job).
    if (r.assist != 0 && !in_raid(r, groups, r.assist)) {
      r.assist = 0;
      changed = true;
    }
    if (changed) {
      RaidChange c = touched(r);
      for (const std::uint32_t g : before) add_group(c.groups, g);
      out.push_back(std::move(c));
    }
  }
  prune_invites(groups);
  return out;
}

bool RaidRegistry::restore(Raid r, const GroupRegistry& groups) {
  if (r.id == 0 || raids_.contains(r.id)) return false;
  std::vector<std::uint32_t> kept;
  for (const std::uint32_t g : r.groups)
    if (groups.find(g) && !raid_of_.contains(g) && std::find(kept.begin(), kept.end(), g) == kept.end()) kept.push_back(g);
  if (kept.size() < 2 || static_cast<int>(kept.size()) > max_) return false;
  r.groups = std::move(kept);
  if (r.loot != RaidLoot::Master && r.loot != RaidLoot::NeedGreed) r.loot = RaidLoot::Master;
  if (!in_raid(r, groups, r.leader))
    if (const Group* g = groups.find(r.groups.front())) r.leader = g->leader;
  if (r.assist != 0 && !in_raid(r, groups, r.assist)) r.assist = 0;
  r.marks = {};
  for (const std::uint32_t g : r.groups) raid_of_[g] = r.id;
  next_id_ = std::max(next_id_, r.id + 1);
  raids_.emplace(r.id, std::move(r));
  return true;
}

std::vector<std::pair<std::int64_t, RaidInvite>> RaidRegistry::expire_invites(double now) {
  std::vector<std::pair<std::int64_t, RaidInvite>> out;
  for (auto it = invites_.begin(); it != invites_.end();) {
    if (it->second.expires_at <= now) {
      out.emplace_back(it->first, it->second);
      it = invites_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

void RaidRegistry::drop_invites(std::int64_t who) {
  invites_.erase(who);
  for (auto it = invites_.begin(); it != invites_.end();) it = it->second.from == who ? invites_.erase(it) : std::next(it);
}

void RaidRegistry::prune_invites(const GroupRegistry& groups) {
  for (auto it = invites_.begin(); it != invites_.end();) {
    const RaidInvite& inv = it->second;
    const Group* theirs = groups.find(inv.group);
    bool stands = theirs && theirs->leader == it->first;
    if (stands) {
      const Group* mine = groups.group_of(inv.from);
      const Raid* r = mine ? raid_of_group(mine->id) : nullptr;
      stands = mine && (inv.raid != 0 ? r && r->id == inv.raid && r->leader == inv.from : !r && mine->leader == inv.from);
    }
    it = stands ? std::next(it) : invites_.erase(it);
  }
}

}  // namespace ef::game
