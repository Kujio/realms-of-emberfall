// Groups (Phase 7), pure and unit-tested: invitations, membership (up to SocialRules::group_max),
// leadership and the loot rule. Members are characters, wherever they are in the world; the game server
// owns one registry and zones read it (shared XP, loot, quest credit, group spells).
// Phase 12: groups outlast logouts. A member who leaves the world stays in the group, offline, for a
// grace period (social.group.offline_grace_seconds) and is back in it on their return; leadership
// passes to someone online. The game server saves every group, so they survive a restart too (every
// member restored offline, with the grace period from the restart).
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
  std::string class_id;       // last known, shown while they are offline
  int level = 0;              // last known
  bool online = true;
  double offline_since = 0.0;  // when they left the world (online == false)
};

struct Group {
  std::uint32_t id = 0;
  std::int64_t leader = 0;
  std::vector<GroupMemberRef> members;  // in joining order (the leader is among them)
  LootRule loot_rule = LootRule::NeedGreed;
  bool has(std::int64_t char_id) const;
  const GroupMemberRef* member(std::int64_t char_id) const;
  std::size_t online_count() const;
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
  TargetOffline,   // they are offline (making an offline member leader)
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
  std::uint32_t gone = 0;               // the id of a group that disbanded (to forget it in the database)
  std::int64_t removed = 0;             // expire_offline: the member who was dropped
  std::string removed_name;
  bool ok() const { return error == GroupError::Ok; }
  // The group that changed, whether it is still there (group) or disbanded (gone).
  std::uint32_t id() const { return group != 0 ? group : gone; }
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
  // Phase 12. A character leaving the world stays in their group, offline (invitations to or from them
  // are dropped); a leader going offline hands the lead to the first member still online.
  GroupChange go_offline(std::int64_t who, double now);
  // Back in the world: online in their group again (taking the lead if no leader is online).
  GroupChange come_online(std::int64_t who, const std::string& name);
  // Remembers what to show for a member while they are offline.
  void set_member_info(std::int64_t who, const std::string& class_id, int level);
  // Members offline for longer than `grace` seconds leave their groups (one change each).
  std::vector<GroupChange> expire_offline(double now, double grace);
  // Puts back a saved group with every member offline since `now`. Refused (false) if it has fewer than
  // two members, more than the maximum, a member twice or already grouped, or a taken id.
  bool restore(Group g, double now);
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
  static std::int64_t first_online(const Group& g);
  void drop_invites(std::int64_t who);
  static std::vector<std::int64_t> ids(const Group& g);
  static GroupChange touched(const Group& g);

  int max_;
  std::uint32_t next_id_ = 1;
  std::map<std::uint32_t, Group> groups_;
  std::map<std::int64_t, std::uint32_t> member_of_;  // char -> group id
  std::map<std::int64_t, GroupInvite> invites_;      // invitee -> invitation
};

}  // namespace ef::game
