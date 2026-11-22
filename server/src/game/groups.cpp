#include "game/groups.hpp"

#include <algorithm>

namespace ef::game {

bool Group::has(std::int64_t char_id) const { return member(char_id) != nullptr; }

const GroupMemberRef* Group::member(std::int64_t char_id) const {
  for (const auto& m : members)
    if (m.char_id == char_id) return &m;
  return nullptr;
}

const char* group_error_text(GroupError e) {
  switch (e) {
    case GroupError::Ok: return "";
    case GroupError::NotInGroup: return "You are not in a group.";
    case GroupError::NotLeader: return "Only the group leader can do that.";
    case GroupError::Full: return "The group is full.";
    case GroupError::TargetGrouped: return "They are already in a group.";
    case GroupError::AlreadyGrouped: return "You are already in a group.";
    case GroupError::NoInvite: return "You have not been invited to a group.";
    case GroupError::InviteSelf: return "You cannot invite yourself.";
    case GroupError::AlreadyInvited: return "They are considering another invitation.";
    case GroupError::NotMember: return "They are not in your group.";
  }
  return "";
}

std::vector<std::int64_t> GroupRegistry::ids(const Group& g) {
  std::vector<std::int64_t> out;
  out.reserve(g.members.size());
  for (const auto& m : g.members) out.push_back(m.char_id);
  return out;
}

Group* GroupRegistry::mutable_group_of(std::int64_t char_id) {
  auto it = member_of_.find(char_id);
  if (it == member_of_.end()) return nullptr;
  auto g = groups_.find(it->second);
  return g == groups_.end() ? nullptr : &g->second;
}

const Group* GroupRegistry::group_of(std::int64_t char_id) const {
  auto it = member_of_.find(char_id);
  if (it == member_of_.end()) return nullptr;
  auto g = groups_.find(it->second);
  return g == groups_.end() ? nullptr : &g->second;
}

const Group* GroupRegistry::find(std::uint32_t id) const {
  auto it = groups_.find(id);
  return it == groups_.end() ? nullptr : &it->second;
}

const GroupInvite* GroupRegistry::invite_for(std::int64_t char_id) const {
  auto it = invites_.find(char_id);
  return it == invites_.end() ? nullptr : &it->second;
}

GroupChange GroupRegistry::invite(std::int64_t from, const std::string& from_name, std::int64_t to, double expires_at) {
  GroupChange c;
  if (from == to) return c.error = GroupError::InviteSelf, c;
  if (const Group* g = group_of(from)) {
    if (g->leader != from) return c.error = GroupError::NotLeader, c;
    if (static_cast<int>(g->members.size()) >= max_) return c.error = GroupError::Full, c;
    c.group = g->id;
  }
  if (group_of(to)) return c.error = GroupError::TargetGrouped, c;
  if (invites_.contains(to)) return c.error = GroupError::AlreadyInvited, c;
  invites_[to] = {from, from_name, expires_at};
  return c;
}

GroupChange GroupRegistry::accept(std::int64_t who, const std::string& who_name) {
  GroupChange c;
  auto it = invites_.find(who);
  if (it == invites_.end()) return c.error = GroupError::NoInvite, c;
  const GroupInvite inv = it->second;
  invites_.erase(it);
  if (group_of(who)) return c.error = GroupError::AlreadyGrouped, c;
  Group* g = mutable_group_of(inv.from);
  if (g) {
    // The inviter may have joined someone else's group meanwhile (they are not its leader then).
    if (g->leader != inv.from) return c.error = GroupError::NotLeader, c;
    if (static_cast<int>(g->members.size()) >= max_) return c.error = GroupError::Full, c;
  } else {
    Group fresh;
    fresh.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    fresh.leader = inv.from;
    fresh.members.push_back({inv.from, inv.from_name});
    g = &groups_.emplace(fresh.id, std::move(fresh)).first->second;
    member_of_[inv.from] = g->id;
    // The inviter is now grouped: any invitation waiting for them is moot.
    invites_.erase(inv.from);
  }
  g->members.push_back({who, who_name});
  member_of_[who] = g->id;
  c.group = g->id;
  c.touched = ids(*g);
  return c;
}

std::int64_t GroupRegistry::decline(std::int64_t who) {
  auto it = invites_.find(who);
  if (it == invites_.end()) return 0;
  const std::int64_t from = it->second.from;
  invites_.erase(it);
  return from;
}

GroupChange GroupRegistry::remove_member(Group& g, std::int64_t who) {
  GroupChange c;
  c.group = g.id;
  c.touched = ids(g);
  std::erase_if(g.members, [&](const GroupMemberRef& m) { return m.char_id == who; });
  member_of_.erase(who);
  if (g.members.size() <= 1) {
    for (const auto& m : g.members) member_of_.erase(m.char_id);
    c.disbanded = true;
    c.group = 0;
    groups_.erase(g.id);
    return c;
  }
  if (g.leader == who) g.leader = g.members.front().char_id;
  return c;
}

GroupChange GroupRegistry::leave(std::int64_t who) {
  Group* g = mutable_group_of(who);
  if (!g) return {GroupError::NotInGroup, 0, {}, false};
  return remove_member(*g, who);
}

GroupChange GroupRegistry::kick(std::int64_t leader, std::int64_t target) {
  Group* g = mutable_group_of(leader);
  if (!g) return {GroupError::NotInGroup, 0, {}, false};
  if (g->leader != leader) return {GroupError::NotLeader, g->id, {}, false};
  if (target == leader || !g->has(target)) return {GroupError::NotMember, g->id, {}, false};
  return remove_member(*g, target);
}

GroupChange GroupRegistry::make_leader(std::int64_t leader, std::int64_t target) {
  Group* g = mutable_group_of(leader);
  if (!g) return {GroupError::NotInGroup, 0, {}, false};
  if (g->leader != leader) return {GroupError::NotLeader, g->id, {}, false};
  if (!g->has(target)) return {GroupError::NotMember, g->id, {}, false};
  g->leader = target;
  return {GroupError::Ok, g->id, ids(*g), false};
}

GroupChange GroupRegistry::set_loot_rule(std::int64_t leader, LootRule rule) {
  Group* g = mutable_group_of(leader);
  if (!g) return {GroupError::NotInGroup, 0, {}, false};
  if (g->leader != leader) return {GroupError::NotLeader, g->id, {}, false};
  g->loot_rule = rule;
  return {GroupError::Ok, g->id, ids(*g), false};
}

GroupChange GroupRegistry::remove_character(std::int64_t who) {
  invites_.erase(who);
  std::erase_if(invites_, [&](const auto& kv) { return kv.second.from == who; });
  if (Group* g = mutable_group_of(who)) return remove_member(*g, who);
  return {GroupError::NotInGroup, 0, {}, false};
}

std::vector<std::pair<std::int64_t, GroupInvite>> GroupRegistry::expire_invites(double now) {
  std::vector<std::pair<std::int64_t, GroupInvite>> out;
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

}  // namespace ef::game
