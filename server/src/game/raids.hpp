// Raids (Phase 14), pure and unit-tested: several groups joined into one force for the raid dungeons.
// A raid is made of whole groups (up to SocialRules::raid_max_groups); a player is in a raid through
// their group, so joining or leaving a group in a raid joins or leaves the raid. Invitations go from the
// raid leader (or a group leader founding a raid) to the leader of another group, whose whole group
// joins. The raid leader is a character in one of its groups; when they leave, go offline or their
// group goes, the lead passes to the first member online (reconcile). A raid of one group dissolves.
// The game server owns one registry beside the groups and saves every raid, so raids survive restarts.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "game/groups.hpp"

namespace ef::game {

// How a raid's kills are looted. Wire values (RaidState.loot_rule, /raidloot).
enum class RaidLoot : std::uint8_t {
  Master = 0,     // the raid leader takes every item and hands each to a raider who was at the kill
  NeedGreed = 1,  // everyone at the kill rolls need before greed, as a group does
};

struct Raid {
  std::uint32_t id = 0;
  std::int64_t leader = 0;
  std::vector<std::uint32_t> groups;  // in joining order (group 1 first)
  RaidLoot loot = RaidLoot::Master;
  bool has_group(std::uint32_t g) const;
};

enum class RaidError : std::uint8_t {
  Ok,
  NotInGroup,        // you are not in a group (raids are made of groups)
  NotGroupLeader,    // only a group leader can do that (invite to found a raid, take the group out)
  NotInRaid,         // you are not in a raid
  NotRaidLeader,     // only the raid leader can do that
  Full,              // the raid already has the most groups
  TargetNotGrouped,  // they are not in a group
  TargetNotLeader,   // invite their group's leader instead
  TargetInRaid,      // their group is already in a raid
  SameGroup,         // they are in your own group
  AlreadyInvited,    // they are considering another raid invitation
  NoInvite,          // nothing to accept, or it is no longer good
  NotMember,         // they are not in your raid
  TargetOffline,     // the lead cannot go to someone offline
};

const char* raid_error_text(RaidError e);

struct RaidInvite {
  std::int64_t from = 0;
  std::string from_name;
  double expires_at = 0.0;
  std::uint32_t raid = 0;   // the raid it invites to (0 = founding one with the inviter's group)
  std::uint32_t group = 0;  // the group invited: its leader must still lead it to accept
};

// What a change touched: every group whose view of the raid changed (before and after), so the game
// server can refresh their members.
struct RaidChange {
  RaidError error = RaidError::Ok;
  std::uint32_t raid = 0;               // the raid involved (0 if it is gone)
  std::vector<std::uint32_t> groups;    // groups to refresh, including any that left
  bool disbanded = false;
  std::uint32_t gone = 0;               // a raid that dissolved (to forget it in the database)
  bool ok() const { return error == RaidError::Ok; }
  std::uint32_t id() const { return raid != 0 ? raid : gone; }
};

class RaidRegistry {
 public:
  // A raid always has room for two groups (a smaller limit could never form one).
  explicit RaidRegistry(int max_groups = 12) : max_(max_groups < 2 ? 2 : max_groups) {}

  // `from` invites the leader of another group. `from` must be the raid leader, or (not in a raid) the
  // leader of their group, which founds the raid on acceptance with `from` as its leader.
  RaidChange invite(const GroupRegistry& groups, std::int64_t from, const std::string& from_name, std::int64_t to, double expires_at);
  // `who`'s group joins the inviter's raid (founding it if needed).
  RaidChange accept(const GroupRegistry& groups, std::int64_t who);
  // Drops the invitation; returns the inviter (0 if there was none).
  std::int64_t decline(std::int64_t who);
  // A group leader takes their group out of the raid.
  RaidChange leave(const GroupRegistry& groups, std::int64_t who);
  // The raid leader removes a group (not their own: they disband or leave instead).
  RaidChange kick(const GroupRegistry& groups, std::int64_t leader, std::uint32_t group);
  RaidChange make_leader(const GroupRegistry& groups, std::int64_t leader, std::int64_t target);
  RaidChange set_loot(const GroupRegistry& groups, std::int64_t leader, RaidLoot loot);
  RaidChange disband(const GroupRegistry& groups, std::int64_t leader);
  // After any group change: groups that are gone leave their raid, a leader who is no longer in the raid
  // or is offline hands the lead to the first member online, and a raid left with one group dissolves.
  std::vector<RaidChange> reconcile(const GroupRegistry& groups);
  // Puts back a saved raid. Groups that no longer exist (or are in another raid) are dropped; refused
  // (false) if fewer than two remain, there are more than the maximum, or the id is taken. A leader who
  // is no longer in it is replaced by the first member.
  bool restore(Raid r, const GroupRegistry& groups);
  // Invitations older than `now`, removed (invitee, invitation).
  std::vector<std::pair<std::int64_t, RaidInvite>> expire_invites(double now);
  // A character leaving the world: every invitation to or from them is dropped.
  void drop_invites(std::int64_t who);

  const Raid* raid_of_group(std::uint32_t group) const;
  const Raid* raid_of(const GroupRegistry& groups, std::int64_t char_id) const;
  const Raid* find(std::uint32_t id) const;
  const RaidInvite* invite_for(std::int64_t char_id) const;
  const std::map<std::uint32_t, Raid>& all() const { return raids_; }
  int max_groups() const { return max_; }

  // Every member of the raid's groups, group by group in joining order.
  static std::vector<std::int64_t> members(const Raid& r, const GroupRegistry& groups);

 private:
  Raid* mutable_raid_of_group(std::uint32_t group);
  RaidChange remove_group(Raid& r, std::uint32_t group, const GroupRegistry& groups);
  RaidChange dissolve(Raid& r);
  static std::int64_t first_online(const Raid& r, const GroupRegistry& groups);
  static bool in_raid(const Raid& r, const GroupRegistry& groups, std::int64_t char_id);

  int max_;
  std::uint32_t next_id_ = 1;
  std::map<std::uint32_t, Raid> raids_;
  std::map<std::uint32_t, std::uint32_t> raid_of_;  // group id -> raid id
  std::map<std::int64_t, RaidInvite> invites_;      // invitee -> invitation
};

}  // namespace ef::game
