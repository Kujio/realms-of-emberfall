// Groups (Phase 7), pure and unit-tested: invitations, membership (up to SocialRules::group_max),
// leadership and the loot rule. Members are characters, wherever they are in the world; the game server
// owns one registry and zones read it (shared XP, loot, quest credit, group spells).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/loot_rules.hpp"

namespace ef::game {

struct GroupMemberRef {
  std::int64_t char_id = 0;
  std::string name;
};

struct Group {
  std::uint32_t id = 0;
  std::int64_t leader = 0;
  std::vector<GroupMemberRef> members;  // in joining order (the leader is among them)
  LootRule loot_rule = LootRule::NeedGreed;
  bool has(std::int64_t char_id) const;
  const GroupMemberRef* member(std::int64_t char_id) const;
};

enum class GroupError : std::uint8_t {
  Ok,
  NotInGroup,      // you are not in a group
  NotLeader,       // only the leader may do that
  Full,            // the group already has the maximum
  TargetGrouped,   // they are already in a group
  AlreadyGrouped,  // you are already in a group (accepting an invite)
  NoInvite,        // nothing to accept or decline
  InviteSelf,
  AlreadyInvited,  // they already have an invitation waiting
  NotMember,       // they are not in your group
};

const char* group_error_text(GroupError e);

struct GroupInvite {
  std::int64_t from = 0;
  std::string from_name;
  double expires_at = 0.0;
};

// Who a change touched: everyone whose view of their group changed (members old and new).
struct GroupChange {
  GroupError error = GroupError::Ok;
  std::uint32_t group = 0;              // the group involved (0 if it is gone)
  std::vector<std::int64_t> touched;    // characters to refresh (including those who left)
  bool disbanded = false;
  bool ok() const { return error == GroupError::Ok; }
};

class GroupRegistry {
 public:
  // A group always has room for two (a smaller limit could never form one).
  explicit GroupRegistry(int max_members = 6) : max_(max_members < 2 ? 2 : max_members) {}

  // `from` invites `to`. The inviter must be ungrouped (a group forms on acceptance) or the leader.
  GroupChange invite(std::int64_t from, const std::string& from_name, std::int64_t to, double expires_at);
  // Joins the inviter's group (forming it if the inviter is still on their own).
  GroupChange accept(std::int64_t who, const std::string& who_name);
  // Drops the invitation; returns the inviter (0 if there was none).
  std::int64_t decline(std::int64_t who);
  // Leaving hands leadership to the next member; a group of one disbands.
  GroupChange leave(std::int64_t who);
  GroupChange kick(std::int64_t leader, std::int64_t target);
  GroupChange make_leader(std::int64_t leader, std::int64_t target);
  GroupChange set_loot_rule(std::int64_t leader, LootRule rule);
  // A character leaving the world: out of their group, and every invitation to or from them dropped.
  GroupChange remove_character(std::int64_t who);
  // Invitations older than `now`, removed (invitee, invitation).
  std::vector<std::pair<std::int64_t, GroupInvite>> expire_invites(double now);

  const Group* group_of(std::int64_t char_id) const;
  const Group* find(std::uint32_t id) const;
  const GroupInvite* invite_for(std::int64_t char_id) const;
  std::size_t group_count() const { return groups_.size(); }
  const std::map<std::uint32_t, Group>& all() const { return groups_; }
  int max_members() const { return max_; }

 private:
  Group* mutable_group_of(std::int64_t char_id);
  GroupChange remove_member(Group& g, std::int64_t who);
  static std::vector<std::int64_t> ids(const Group& g);

  int max_;
  std::uint32_t next_id_ = 1;
  std::map<std::uint32_t, Group> groups_;
  std::map<std::int64_t, std::uint32_t> member_of_;  // char -> group id
  std::map<std::int64_t, GroupInvite> invites_;      // invitee -> invitation
};

}  // namespace ef::game
