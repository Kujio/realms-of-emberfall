// Phase 7 pure rules: chat filter and rate limits, groups, group XP, guild ranks, commands, /who, bans.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <numeric>

#include "auth/bans.hpp"
#include "game/chat.hpp"
#include "game/commands.hpp"
#include "game/groups.hpp"
#include "game/guilds.hpp"
#include "game/rules.hpp"
#include "zone_support.hpp"

using namespace ef::game;

namespace {

const SocialRules& social() { return zt::content().social; }

}  // namespace

TEST_CASE("the shipped social content loads", "[social]") {
  const auto& s = social();
  CHECK(s.group_max == 6);
  REQUIRE(s.guild_ranks.size() >= 2);
  CHECK(s.guild_ranks.front().id == "leader");
  CHECK_FALSE(s.filter_words.empty());
  CHECK(s.rates[chat_id(ChatChannel::Ooc)].rate < s.rates[chat_id(ChatChannel::Say)].rate);
}

TEST_CASE("the word filter masks listed words at the start of words only", "[social][chat]") {
  WordFilter f({"darn", "heck"}, '*');
  CHECK(f.clean("Well DARN it") == "Well **** it");
  CHECK(f.clean("darned heckling") == "****** ********");
  CHECK(f.clean("Lindarn of Heckford") == "Lindarn of ********");  // a listed word inside a word is left alone
  CHECK(f.clean("nothing to see") == "nothing to see");
  CHECK(f.offensive("TheDarnGuild"));
  CHECK_FALSE(f.offensive("The Iron Vow"));
  auto shipped = make_filter(social());
  CHECK(shipped->clean("what the fuck") == "what the ****");
}

TEST_CASE("chat is rate limited per channel and repeated lines are refused", "[social][chat]") {
  SocialRules r = social();
  r.rates[chat_id(ChatChannel::Ooc)] = {0.2, 2.0};
  r.rates[chat_id(ChatChannel::Say)] = {1.0, 5.0};
  r.repeat_limit = 3;
  r.repeat_window_seconds = 30.0f;
  ChatGuard g(r);
  double t = 100.0;
  CHECK(g.check(ChatChannel::Ooc, "one", t) == ChatVerdict::Ok);
  CHECK(g.check(ChatChannel::Ooc, "two", t) == ChatVerdict::Ok);
  CHECK(g.check(ChatChannel::Ooc, "three", t) == ChatVerdict::RateLimited);  // OOC's burst is 2
  CHECK(g.check(ChatChannel::Say, "still fine to say", t) == ChatVerdict::Ok);  // say has its own bucket
  t += 5.0;  // 0.2/s refills one OOC message
  CHECK(g.check(ChatChannel::Ooc, "three", t) == ChatVerdict::Ok);
  CHECK(g.check(ChatChannel::Ooc, "three", t) == ChatVerdict::RateLimited);

  ChatGuard spam(r);
  t = 0.0;
  for (int i = 0; i < 3; ++i) {
    CHECK(spam.check(ChatChannel::Say, "WTS  Rusty Sword", t) == ChatVerdict::Ok);
    t += 2.0;
  }
  CHECK(spam.check(ChatChannel::Shout, "wts rusty sword", t) == ChatVerdict::Repeated);  // any channel, any case
  CHECK(spam.check(ChatChannel::Say, "something else", t) == ChatVerdict::Ok);
  t += 31.0;  // the window passes
  CHECK(spam.check(ChatChannel::Say, "WTS Rusty Sword", t) == ChatVerdict::Ok);
  CHECK(std::string(chat_refusal(ChatVerdict::RateLimited)).find("too quickly") != std::string::npos);
}

TEST_CASE("friend and ignore lists keep proper names, sorted and capped", "[social][chat]") {
  CHECK(list_name("aLDRIC") == "Aldric");
  CHECK(list_name("x").empty());
  CHECK(list_name("bad name").empty());
  std::vector<std::string> list;
  bool added = false;
  CHECK(toggle_name(list, "Brenna", 2, added));
  CHECK(added);
  CHECK(toggle_name(list, "Aldric", 2, added));
  CHECK_FALSE(toggle_name(list, "Corvin", 2, added));  // full
  CHECK(list == std::vector<std::string>{"Aldric", "Brenna"});
  CHECK(has_name(list, "Brenna"));
  CHECK(toggle_name(list, "Brenna", 2, added));
  CHECK_FALSE(added);
  CHECK_FALSE(has_name(list, "Brenna"));
}

TEST_CASE("groups form on acceptance and hold at most six", "[social][group]") {
  GroupRegistry reg(6);
  CHECK(reg.invite(1, "Aldric", 1, 10.0).error == GroupError::InviteSelf);
  REQUIRE(reg.invite(1, "Aldric", 2, 10.0).ok());
  CHECK(reg.invite(3, "Corvin", 2, 10.0).error == GroupError::AlreadyInvited);
  CHECK(reg.group_of(1) == nullptr);  // nothing forms until they accept
  auto joined = reg.accept(2, "Brenna");
  REQUIRE(joined.ok());
  const Group* g = reg.group_of(1);
  REQUIRE(g);
  CHECK(g->leader == 1);
  CHECK(g->members.size() == 2);
  CHECK(joined.touched == std::vector<std::int64_t>{1, 2});
  CHECK(reg.invite(2, "Brenna", 3, 10.0).error == GroupError::NotLeader);
  CHECK(reg.accept(9, "Nobody").error == GroupError::NoInvite);
  for (std::int64_t c = 3; c <= 6; ++c) {
    REQUIRE(reg.invite(1, "Aldric", c, 10.0).ok());
    REQUIRE(reg.accept(c, "M" + std::to_string(c)).ok());
  }
  CHECK(reg.group_of(1)->members.size() == 6);
  CHECK(reg.invite(1, "Aldric", 7, 10.0).error == GroupError::Full);
  CHECK(reg.invite(7, "Grim", 3, 10.0).error == GroupError::TargetGrouped);
}

TEST_CASE("leaving, kicking and leadership", "[social][group]") {
  GroupRegistry reg(6);
  for (std::int64_t c = 2; c <= 4; ++c) {
    REQUIRE(reg.invite(1, "A", c, 10.0).ok());
    REQUIRE(reg.accept(c, "M").ok());
  }
  CHECK(reg.kick(2, 3).error == GroupError::NotLeader);
  CHECK(reg.kick(1, 9).error == GroupError::NotMember);
  CHECK(reg.make_leader(1, 3).ok());
  CHECK(reg.group_of(1)->leader == 3);
  CHECK(reg.set_loot_rule(1, LootRule::RoundRobin).error == GroupError::NotLeader);
  CHECK(reg.set_loot_rule(3, LootRule::RoundRobin).ok());
  CHECK(reg.group_of(2)->loot_rule == LootRule::RoundRobin);
  auto kicked = reg.kick(3, 4);
  REQUIRE(kicked.ok());
  CHECK(std::find(kicked.touched.begin(), kicked.touched.end(), 4) != kicked.touched.end());  // the kicked one hears too
  CHECK(reg.group_of(4) == nullptr);
  auto left = reg.leave(3);  // the leader leaves: the first remaining member leads
  REQUIRE(left.ok());
  CHECK(reg.group_of(1)->leader == 1);
  auto last = reg.leave(2);  // two became one: disbanded
  CHECK(last.disbanded);
  CHECK(reg.group_of(1) == nullptr);
  CHECK(reg.group_count() == 0);
  CHECK(reg.leave(1).error == GroupError::NotInGroup);
}

TEST_CASE("a group always has room for two", "[social][group]") {
  GroupRegistry tiny(1);
  CHECK(tiny.max_members() == 2);
  REQUIRE(tiny.invite(1, "A", 2, 10.0).ok());
  REQUIRE(tiny.accept(2, "B").ok());
  CHECK(tiny.invite(1, "A", 3, 10.0).error == GroupError::Full);
}

TEST_CASE("offline members keep their place, the lead moves, and the grace period ends it", "[social][group]") {
  GroupRegistry r(6);
  r.invite(1, "Ann", 2, 100.0);
  r.accept(2, "Bo");
  r.invite(1, "Ann", 3, 100.0);
  r.accept(3, "Cy");
  r.set_member_info(2, "cleric", 12);
  // The leader goes offline: the lead passes to the first member online.
  auto c = r.go_offline(1, 10.0);
  REQUIRE(c.ok());
  CHECK(c.touched.size() == 3);
  const Group* g = r.group_of(1);
  REQUIRE(g);
  CHECK(g->leader == 2);
  CHECK(g->online_count() == 2);
  CHECK_FALSE(g->member(1)->online);
  // Offline members cannot be made leader, but can be removed.
  CHECK(r.make_leader(2, 1).error == GroupError::TargetOffline);
  // Back online: in the group again (the lead stays with Bo, who is online).
  CHECK(r.come_online(1, "Ann").ok());
  CHECK(r.group_of(1)->leader == 2);
  CHECK(r.group_of(1)->member(1)->online);
  // Everyone offline: whoever returns first leads.
  r.go_offline(1, 20.0);
  r.go_offline(2, 20.0);
  r.go_offline(3, 20.0);
  r.come_online(3, "Cy");
  CHECK(r.group_of(3)->leader == 3);
  CHECK(r.group_of(2)->member(2)->class_id == "cleric");
  // The grace period: nothing yet at 30 s, then Ann and Bo leave; the group of one disbands.
  CHECK(r.expire_offline(49.0, 30.0).empty());
  auto out = r.expire_offline(51.0, 30.0);
  REQUIRE(out.size() == 2);
  CHECK(out[0].removed == 1);
  CHECK_FALSE(out[0].disbanded);
  CHECK(out[1].removed == 2);
  CHECK(out[1].disbanded);
  CHECK(out[1].gone != 0);
  CHECK(out[1].id() == out[1].gone);
  CHECK(r.group_count() == 0);
  // Invitations to or from someone going offline are dropped.
  r.invite(5, "Eve", 6, 100.0);
  r.go_offline(5, 1.0);
  CHECK(r.invite_for(6) == nullptr);
}

TEST_CASE("saved groups are restored offline and checked", "[social][group]") {
  GroupRegistry r(3);
  Group g;
  g.id = 40;
  g.leader = 2;
  g.members = {{1, "Ann", "warrior", 5, true, 0.0}, {2, "Bo", "cleric", 6, true, 0.0}};
  REQUIRE(r.restore(g, 100.0));
  const Group* got = r.find(40);
  REQUIRE(got);
  CHECK(got->online_count() == 0);
  CHECK(got->leader == 2);
  CHECK(r.group_of(1) == got);
  // New groups take ids past the restored ones.
  r.invite(7, "Gil", 8, 200.0);
  auto c = r.accept(8, "Hu");
  CHECK(c.group > 40);
  // Refused: a taken id, a member already grouped, too few, too many, a member twice.
  CHECK_FALSE(r.restore(g, 100.0));
  Group dup = g;
  dup.id = 41;
  CHECK_FALSE(r.restore(dup, 100.0));
  Group lone;
  lone.id = 42;
  lone.members = {{9, "Ida", "", 1, true, 0.0}};
  CHECK_FALSE(r.restore(lone, 100.0));
  Group big;
  big.id = 43;
  big.members = {{10, "J", "", 1, true, 0.0}, {11, "K", "", 1, true, 0.0}, {12, "L", "", 1, true, 0.0}, {13, "M", "", 1, true, 0.0}};
  CHECK_FALSE(r.restore(big, 100.0));
  Group twice;
  twice.id = 44;
  twice.members = {{14, "N", "", 1, true, 0.0}, {14, "N", "", 1, true, 0.0}};
  CHECK_FALSE(r.restore(twice, 100.0));
  // A restored group with the highest id: new groups still get ids no group holds.
  GroupRegistry wrap(6);
  Group top;
  top.id = 0xFFFFFFFFu;
  top.members = {{20, "Q", "", 1, true, 0.0}, {21, "R", "", 1, true, 0.0}};
  Group one;
  one.id = 1;
  one.members = {{22, "S", "", 1, true, 0.0}, {23, "T", "", 1, true, 0.0}};
  REQUIRE(wrap.restore(top, 1.0));
  REQUIRE(wrap.restore(one, 1.0));
  wrap.invite(30, "U", 31, 50.0);
  auto fresh = wrap.accept(31, "V");
  REQUIRE(fresh.ok());
  CHECK(fresh.group != 1);
  CHECK(fresh.group != 0xFFFFFFFFu);
  CHECK(wrap.find(1)->members.size() == 2);  // the restored group is untouched
  CHECK(wrap.find(fresh.group)->members.size() == 2);
  // A leader who is not a member is replaced by the first member.
  Group odd;
  odd.id = 45;
  odd.leader = 99;
  odd.members = {{15, "O", "", 1, true, 0.0}, {16, "P", "", 1, true, 0.0}};
  REQUIRE(r.restore(odd, 100.0));
  CHECK(r.find(45)->leader == 15);
}

TEST_CASE("invitations expire and vanish with their characters", "[social][group]") {
  GroupRegistry reg(6);
  REQUIRE(reg.invite(1, "A", 2, 10.0).ok());
  REQUIRE(reg.invite(1, "A", 3, 50.0).ok());
  auto gone = reg.expire_invites(20.0);
  REQUIRE(gone.size() == 1);
  CHECK(gone.front().first == 2);
  CHECK(reg.accept(2, "B").error == GroupError::NoInvite);
  reg.remove_character(1);  // the inviter logs out
  CHECK(reg.invite_for(3) == nullptr);
  // An inviter who joined someone else's group cannot pull people into it.
  GroupRegistry r2(6);
  REQUIRE(r2.invite(1, "A", 2, 99.0).ok());
  REQUIRE(r2.invite(5, "E", 1, 99.0).ok());
  REQUIRE(r2.accept(1, "A").ok());  // 1 joins 5's group
  CHECK(r2.accept(2, "B").error == GroupError::NotLeader);
  CHECK(r2.decline(2) == 0);  // already used up
}

TEST_CASE("group XP: con by the highest member, level floor, bonus and level-weighted split", "[social][group][xp]") {
  const auto& c = zt::content();
  const auto& xp = c.rules.xp;
  const auto& con = c.rules.con;
  const auto& s = c.social;
  // One member is exactly a solo kill.
  auto solo = group_xp_shares(20, {{20, true}}, xp, con, s.group_level_window, s.group_level_fraction);
  CHECK(solo.front().xp == kill_xp(20, 20, xp, con));
  // Six equal members share the kill plus the group bonus.
  std::vector<GroupXpMember> six(6, {20, true});
  auto shares = group_xp_shares(20, six, xp, con, s.group_level_window, s.group_level_fraction);
  const double pool = static_cast<double>(kill_xp(20, 20, xp, con)) * (1.0 + xp.group_bonus_per_member * 5);
  for (const auto& sh : shares) CHECK(sh.xp == static_cast<std::uint64_t>(std::floor(pool / 6.0)));
  // Mixed levels: higher levels take a bigger share; a member far below gets nothing.
  auto mixed = group_xp_shares(30, {{30, true}, {27, true}, {12, true}, {30, false}}, xp, con, 5, 0.25f);
  CHECK(group_level_floor(30, 5, 0.25f) == 23);
  CHECK(mixed[0].xp > mixed[1].xp);
  CHECK(mixed[1].xp > 0);
  CHECK(mixed[2].too_low);
  CHECK(mixed[2].xp == 0);
  CHECK(mixed[3].out_of_range);
  CHECK(mixed[3].xp == 0);
  // The floor widens with level: at 60, fifteen levels; at 8, five (never below 1).
  CHECK(group_level_floor(60, 5, 0.25f) == 45);
  CHECK(group_level_floor(8, 5, 0.25f) == 3);
  CHECK(group_level_floor(3, 5, 0.25f) == 1);
  // A trivial mob for the highest member gives the whole group nothing (no power levelling).
  auto grey = group_xp_shares(10, {{50, true}, {10, true}}, xp, con, 50, 1.0f);
  CHECK(grey[0].xp == 0);
  CHECK(grey[1].xp == 0);
  // Nobody in range: nothing.
  auto none = group_xp_shares(10, {{10, false}}, xp, con, 5, 0.25f);
  CHECK(none[0].xp == 0);
}

TEST_CASE("guild ranks: who may do what to whom", "[social][guild]") {
  const auto& r = social();
  CHECK(guild_allows(r, 0, GuildPerm::Disband));
  CHECK(guild_allows(r, 1, GuildPerm::Promote));
  CHECK_FALSE(guild_allows(r, 1, GuildPerm::Disband));
  CHECK(guild_allows(r, 2, GuildPerm::Invite));
  CHECK_FALSE(guild_allows(r, 3, GuildPerm::Invite));
  CHECK(guild_allows(r, 3, GuildPerm::Speak));
  CHECK_FALSE(guild_allows(r, 99, GuildPerm::Speak));

  Guild g;
  g.id = 1;
  g.name = "The Iron Vow";
  g.members = {{1, "Leader", 0, 30, "warrior"}, {2, "Officer", 1, 30, "cleric"}, {3, "Member", 2, 20, "rogue"}, {4, "Recruit", 3, 5, "wizard"}};
  CHECK(guild_can_invite(g, r, 3) == GuildError::Ok);
  CHECK(guild_can_invite(g, r, 4) == GuildError::NoPermission);
  CHECK(guild_can_invite(g, r, 9) == GuildError::NotInGuild);
  // Officers manage the ranks below them, never each other or the leader.
  CHECK(guild_promote(g, r, 2, 4) == GuildError::Ok);  // recruit -> member
  CHECK(g.member(4)->rank == 2);
  CHECK(guild_promote(g, r, 2, 4) == GuildError::TopRank);  // not to the officer's own rank
  CHECK(guild_promote(g, r, 2, 1) == GuildError::RankTooHigh);
  CHECK(guild_promote(g, r, 3, 4) == GuildError::NoPermission);
  CHECK(guild_promote(g, r, 1, 3) == GuildError::Ok);  // the leader makes officers
  CHECK(g.member(3)->rank == 1);
  CHECK(guild_promote(g, r, 1, 3) == GuildError::TopRank);  // leader is /guildleader, not a promotion
  CHECK(guild_demote(g, r, 2, 3) == GuildError::RankTooHigh);  // officer cannot demote officer
  CHECK(guild_demote(g, r, 1, 3) == GuildError::Ok);
  CHECK(guild_demote(g, r, 1, 3) == GuildError::Ok);
  CHECK(guild_demote(g, r, 1, 3) == GuildError::BottomRank);
  CHECK(guild_demote(g, r, 1, 1) == GuildError::Self);
  CHECK(guild_remove(g, r, 3, 4) == GuildError::NoPermission);
  CHECK(guild_remove(g, r, 2, 1) == GuildError::RankTooHigh);
  CHECK(guild_remove(g, r, 2, 4) == GuildError::Ok);
  CHECK(g.member(4) == nullptr);
  CHECK(guild_set_motd(g, r, 3, "hi") == GuildError::NoPermission);
  CHECK(guild_set_motd(g, r, 2, "Raid at dusk") == GuildError::Ok);
  CHECK(g.motd == "Raid at dusk");
  // The leader hands over before leaving.
  CHECK(guild_leave(g, 1) == GuildError::LeaderMustPass);
  CHECK(guild_pass_leadership(g, 2, 3) == GuildError::NoPermission);
  CHECK(guild_pass_leadership(g, 1, 2) == GuildError::Ok);
  CHECK(g.member(2)->rank == 0);
  CHECK(g.member(1)->rank == 1);
  CHECK(guild_leave(g, 1) == GuildError::Ok);
  CHECK(guild_leave(g, 3) == GuildError::Ok);
  CHECK(guild_leave(g, 2) == GuildError::Ok);  // the last member may leave (the guild disbands)
  CHECK(g.members.empty());
}

TEST_CASE("guild names are checked and normalized", "[social][guild]") {
  const auto& r = social();
  auto f = make_filter(r);
  CHECK(normalize_guild_name("  the   iron  VOW ") == "The Iron Vow");
  CHECK_FALSE(validate_guild_name("The Iron Vow", r, f.get()));
  CHECK_FALSE(validate_guild_name("Knights O'Dawn", r, f.get()));
  CHECK(validate_guild_name("Abc", r, f.get()));  // too short
  CHECK(validate_guild_name(std::string(40, 'A'), r, f.get()));
  CHECK(validate_guild_name("Guild 42", r, f.get()));
  CHECK(validate_guild_name("'Quoted", r, f.get()));
  CHECK(validate_guild_name("Shitlords Inc", r, f.get()));
  GuildRegistry reg;
  Guild g;
  g.id = 7;
  g.name = "The Iron Vow";
  g.members = {{1, "A", 0, 1, "warrior"}};
  reg.add(g);
  CHECK(reg.by_name("the iron vow"));
  CHECK(reg.of(1)->id == 7);
  reg.invite(2, {7, 1, "A", 10.0});
  reg.invite(3, {7, 1, "A", 99.0});
  CHECK(reg.expire_invites(20.0).size() == 1);
  CHECK(reg.take_invite(3).has_value());
  CHECK_FALSE(reg.take_invite(3).has_value());
  reg.remove(7);
  CHECK(reg.of(1) == nullptr);
}

TEST_CASE("slash commands split into a name and arguments", "[social][commands]") {
  auto c = parse_command("/GM ban Aldric 7d \"spamming the ooc\" channel");
  REQUIRE(c);
  CHECK(c->name == "gm");
  REQUIRE(c->args.size() == 5);
  CHECK(c->args[3] == "spamming the ooc");
  CHECK(c->rest_after(3) == "\"spamming the ooc\" channel");
  CHECK(c->rest_after(2) == "7d \"spamming the ooc\" channel");
  CHECK(c->rest_after(9).empty());
  CHECK(c->arg(42).empty());
  auto motd = parse_command("guildmotd   Raid at dusk!  ");
  REQUIRE(motd);
  CHECK(motd->rest == "Raid at dusk!");
  CHECK_FALSE(parse_command("   "));
  CHECK_FALSE(parse_command("/"));
  auto bare = parse_command("who");
  REQUIRE(bare);
  CHECK(bare->args.empty());
}

TEST_CASE("durations for bans and mutes", "[social][commands]") {
  CHECK(parse_duration("90") == 90 * 60);
  CHECK(parse_duration("30m") == 1800);
  CHECK(parse_duration("12H") == 12 * 3600);
  CHECK(parse_duration("7d") == 7 * 86400);
  CHECK(parse_duration("2w") == 14 * 86400);
  CHECK(parse_duration("perm") == kPermanent);
  CHECK_FALSE(parse_duration("0"));
  CHECK_FALSE(parse_duration("-5m"));
  CHECK_FALSE(parse_duration("soon"));
  CHECK_FALSE(parse_duration("99999w"));
  CHECK(describe_duration(90 * 60) == "1 hour 30 minutes");
  CHECK(describe_duration(8 * 86400 + 5) == "1 week 1 day");
  CHECK(describe_duration(kPermanent) == "permanently");
}

TEST_CASE("/who filters", "[social][who]") {
  const auto& c = zt::content();
  WhoQuery q;
  REQUIRE_FALSE(parse_who({"all", "warrior", "10-20"}, c, q));
  CHECK(q.all);
  CHECK(q.class_id == "warrior");
  CHECK(q.min_level == 10);
  CHECK(q.max_level == 20);
  WhoCandidate w{"Aldric", 15, "warrior", "highlander", "emberglade", "The Iron Vow", false};
  CHECK(who_matches(q, w, "elsewhere"));
  w.level = 25;
  CHECK_FALSE(who_matches(q, w, "elsewhere"));
  WhoQuery here;
  REQUIRE_FALSE(parse_who({"ald"}, c, here));
  w.level = 15;
  CHECK(who_matches(here, w, "emberglade"));
  CHECK_FALSE(who_matches(here, w, "elsewhere"));  // without "all", only your zone
  WhoQuery g;
  REQUIRE_FALSE(parse_who({"guild", "iron"}, c, g));
  g.all = true;
  CHECK(who_matches(g, w, ""));
  WhoQuery race;
  REQUIRE_FALSE(parse_who({"all", "Highlander"}, c, race));
  CHECK(race.race == "highlander");
  WhoQuery bad;
  CHECK(parse_who({"20-10"}, c, bad));
  WhoQuery two;
  CHECK(parse_who({"bob", "ann"}, c, two));
  WhoQuery zone;
  REQUIRE_FALSE(parse_who({c.zones.front().id}, c, zone));
  CHECK(zone.all);
  CHECK(zone.zone == c.zones.front().id);
}

TEST_CASE("bans and mutes", "[social][admin]") {
  using namespace ef::auth;
  CHECK_FALSE(ban_refusal(0, "", 1000));
  CHECK_FALSE(ban_refusal(999, "", 1000));  // over
  auto msg = ban_refusal(1000 + 3600, "spam", 1000);
  REQUIRE(msg);
  CHECK(msg->find("1 hour") != std::string::npos);
  CHECK(msg->find("spam") != std::string::npos);
  CHECK(ban_refusal(kBanForever, "", 1000)->find("banned") != std::string::npos);
  CHECK(restriction_until(kBanForever, 5) == kBanForever);
  CHECK(restriction_until(60, 5) == 65);
  CHECK(mute_notice(2000, 1000));
  CHECK_FALSE(mute_notice(0, 1000));
}

TEST_CASE("social.json is checked at startup", "[social][content]") {
  auto expect = [](const std::function<void(nlohmann::json&)>& breakit, const std::string& needle) {
    ContentDocs d = read_content_docs(EF_CONTENT_DIR);
    breakit(d.social);
    try {
      parse_content(d);
      FAIL("expected a content error mentioning: " << needle);
    } catch (const ContentError& e) {
      INFO(e.what());
      CHECK(std::string(e.what()).find(needle) != std::string::npos);
    }
  };
  expect([](nlohmann::json& s) { s["group"]["max_members"] = 7; }, "max_members must be 2..6");
  expect([](nlohmann::json& s) { s["guild"]["ranks"][1]["permissions"].push_back("disband"); }, "only the leader may disband");
  expect([](nlohmann::json& s) { s["guild"]["ranks"][2]["permissions"].push_back("fly"); }, "unknown permission 'fly'");
  expect([](nlohmann::json& s) { s["chat"]["channels"].erase("ooc"); }, "channels.ooc: missing");
  expect([](nlohmann::json& s) { s["chat"]["channels"]["yell"] = {{"rate", 1}, {"burst", 1}}; }, "unknown channel 'yell'");
  expect([](nlohmann::json& s) { s["filter"]["mask"] = "**"; }, "one printable ASCII character");
  expect([](nlohmann::json& s) { s.erase("guild"); }, "missing object 'guild'");
}
