// Phase 7 over the wire: chat channels and filters, groups, guilds (persisting across a restart),
// friends, /who, and the game master tools with their audit log and bans enforced at login.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <random>

#include <unistd.h>

#include "server_support.hpp"

using namespace ef;
using namespace ef::game;
using namespace wire;

namespace {

std::vector<proto::ChatMessage> chats(const Harness& h, const Client& c, ChatChannel ch) {
  std::vector<proto::ChatMessage> out;
  for (auto& m : h.received<proto::ChatMessage>(c.peer))
    if (m.channel == chat_id(ch)) out.push_back(m);
  return out;
}

bool heard(const Harness& h, const Client& c, ChatChannel ch, const std::string& needle, const std::string& from = "") {
  for (const auto& m : chats(h, c, ch))
    if (m.text.find(needle) != std::string::npos && (from.empty() || m.from_name == from)) return true;
  return false;
}

bool told(const Harness& h, const Client& c, const std::string& needle) { return heard(h, c, ChatChannel::System, needle); }

// Logs into an existing account and enters its first character.
void return_to_world(Harness& h, Client& c, const std::string& user) {
  c.login(user, "password123", false);
  auto list = h.last<proto::CharList>(c.peer);
  REQUIRE(list);
  REQUIRE_FALSE(list->characters.empty());
  c.send(proto::EnterWorld{list->characters.front().id});
  h.tick();
  auto ew = h.last<proto::EnterWorldResult>(c.peer);
  REQUIRE(ew);
  REQUIRE(ew->ok);
}

const PlayerEntity* in_zone(Harness& h, const std::string& name) {
  Zone* z = h.server->zone("proving_flats");
  if (!z) return nullptr;
  const PlayerEntity* found = nullptr;
  z->for_each_player([&](const PlayerEntity& p) {
    if (p.name == name) found = &p;
  });
  return found;
}

int count_of_item(const PlayerEntity& p, const std::string& item) {
  int n = 0;
  p.inv.for_each([&](SlotCode, const ItemStackState& st) {
    if (st.item_id == item) n += st.count;
  });
  return n;
}

}  // namespace

TEST_CASE("chat channels: say, shout, ooc, tells and replies, emotes, ignore, channel mutes, filter", "[social][chat][server]") {
  Harness h;
  Client a(h);
  Client b(h);
  Client c(h);
  a.login_and_enter("alice", "Aldric");
  b.login_and_enter("bobby", "Brenna");
  c.login_and_enter("corvy", "Corvin");
  h.tick(2);

  a.say_on(ChatChannel::Shout, "Anyone near the flats?");
  a.say_on(ChatChannel::Ooc, "Hello world");
  CHECK(heard(h, b, ChatChannel::Shout, "Anyone near", "Aldric"));
  CHECK(heard(h, c, ChatChannel::Ooc, "Hello world", "Aldric"));

  a.say_on(ChatChannel::Tell, "psst", "brenna");
  CHECK(heard(h, b, ChatChannel::Tell, "psst", "Aldric"));
  CHECK(heard(h, a, ChatChannel::TellEcho, "psst", "Brenna"));  // the echo names the recipient
  CHECK_FALSE(heard(h, c, ChatChannel::Tell, "psst"));
  b.cmd("r right back at you");
  CHECK(heard(h, a, ChatChannel::Tell, "right back", "Brenna"));
  a.cmd("tell Nobodyhere hi");
  CHECK(told(h, a, "Nobodyhere is not online."));

  a.cmd("me waves.");
  CHECK(heard(h, c, ChatChannel::Emote, "waves.", "Aldric"));

  // Ignoring: Brenna stops hearing Aldric, and his tells bounce as if she were away.
  b.cmd("ignore aldric");
  CHECK(told(h, b, "You are now ignoring Aldric."));
  a.send(proto::ChatSay{"can you hear me"});
  CHECK_FALSE(heard(h, b, ChatChannel::Say, "can you hear me"));
  CHECK(heard(h, c, ChatChannel::Say, "can you hear me", "Aldric"));
  a.say_on(ChatChannel::Tell, "hello?", "Brenna");
  CHECK(told(h, a, "Brenna is not online."));
  auto fl = h.last<proto::FriendsList>(b.peer);
  REQUIRE(fl);
  CHECK(fl->ignored == std::vector<std::string>{"Aldric"});

  // A channel turned off stays quiet.
  c.cmd("channel ooc off");
  h.now += 10.0;  // OOC budget refills
  h.tick();
  a.say_on(ChatChannel::Ooc, "second ooc line");
  CHECK_FALSE(heard(h, c, ChatChannel::Ooc, "second ooc line"));
  CHECK(heard(h, a, ChatChannel::Ooc, "second ooc line"));

  // The filter masks, the rate limit and the repeat guard refuse.
  a.send(proto::ChatSay{"what the fuck"});
  CHECK(heard(h, c, ChatChannel::Say, "what the ****"));
  for (int i = 0; i < 3; ++i) a.say_on(ChatChannel::Ooc, "spam number " + std::to_string(i));
  CHECK(told(h, a, "too quickly"));
  h.now += 30.0;
  h.tick();
  for (int i = 0; i < 4; ++i) {
    h.now += 1.5;
    a.send(proto::ChatSay{"buy my sword"});
  }
  CHECK(told(h, a, "too many times"));

  // An unknown channel is a protocol error, not a crash.
  a.send(proto::ChatSend{200, "", "x"});
  CHECK_FALSE(h.transport.disconnecting.contains(a.peer));
}

TEST_CASE("groups over the wire: six join, the seventh is refused, leaders change, logging out keeps your place for a while", "[social][group][server]") {
  Harness h;
  std::vector<std::unique_ptr<Client>> cs;
  const char* names[] = {"Leader", "Membera", "Memberb", "Memberc", "Memberd", "Membere", "Seventh"};
  for (int i = 0; i < 7; ++i) {
    cs.push_back(std::make_unique<Client>(h));
    cs.back()->login_and_enter("user" + std::to_string(i), names[i]);
  }
  h.tick(2);
  Client& lead = *cs[0];
  for (int i = 1; i <= 5; ++i) {
    lead.cmd(std::string("invite ") + names[i]);
    auto inv = h.last<proto::SocialInvite>(cs[static_cast<std::size_t>(i)]->peer);
    REQUIRE(inv);
    CHECK(inv->kind == 0);
    CHECK(inv->from_name == "Leader");
    cs[static_cast<std::size_t>(i)]->cmd("join");
  }
  h.tick(20);
  auto st = h.last<proto::GroupState>(cs[3]->peer);
  REQUIRE(st);
  CHECK(st->members.size() == 6);
  CHECK(st->leader == "Leader");
  for (const auto& m : st->members) {
    CHECK(m.entity_id != 0);  // all in the same zone: their bars can be clicked
    CHECK(m.hp_pct == 100);
    CHECK(m.level == 1);
  }
  lead.cmd("invite Seventh");
  CHECK(told(h, lead, "The group is full."));
  CHECK(h.server->groups().group_count() == 1);

  // Group chat reaches the six, not the seventh.
  cs[2]->say_on(ChatChannel::Group, "pull the next one");
  CHECK(heard(h, *cs[5], ChatChannel::Group, "pull the next one", "Memberb"));
  CHECK_FALSE(heard(h, *cs[6], ChatChannel::Group, "pull the next one"));

  // Only the leader kicks or hands over; the loot rule is the group's.
  cs[1]->cmd("kick Memberb");
  CHECK(told(h, *cs[1], "Only the group leader"));
  lead.cmd("lootrule rr");
  CHECK(h.server->groups().all().begin()->second.loot_rule == LootRule::RoundRobin);
  lead.cmd("makeleader Membera");
  CHECK(told(h, *cs[4], "Membera is now the group leader."));
  cs[1]->cmd("kick Memberb");
  CHECK(told(h, *cs[2], "You have been removed from the group."));
  h.tick(10);
  CHECK(h.last<proto::GroupState>(cs[2]->peer)->group_id == 0);
  CHECK(h.last<proto::GroupState>(cs[0]->peer)->members.size() == 5);

  // Logging out keeps your place, offline (Phase 12); the lead passes to someone online. After the grace
  // period you leave the group.
  cs[1]->send(proto::Logout{});
  h.tick(10);
  CHECK(told(h, lead, "Membera has gone offline."));
  auto mid = h.last<proto::GroupState>(lead.peer);
  REQUIRE(mid);
  CHECK(mid->members.size() == 5);
  CHECK(mid->leader == "Leader");  // the first member still online
  for (const auto& m : mid->members) {
    CHECK(m.online == (m.name != "Membera"));
    if (m.name == "Membera") {
      CHECK(m.entity_id == 0);
      CHECK(m.level == 1);  // last known
      CHECK(m.class_id == "warrior");
    }
  }
  h.now += static_cast<double>(h.content.social.group_offline_grace_seconds) + 1.0;
  h.tick(10);
  CHECK(told(h, lead, "Membera has been offline too long and leaves the group."));
  auto after = h.last<proto::GroupState>(lead.peer);
  REQUIRE(after);
  CHECK(after->members.size() == 4);
  CHECK(after->leader == "Leader");
  for (int i : {3, 4, 5}) cs[static_cast<std::size_t>(i)]->cmd("disband");
  CHECK(h.server->groups().group_count() == 0);
  CHECK(told(h, lead, "Your group has disbanded."));

  // Invitations expire.
  lead.cmd("invite Seventh");
  h.tick(20 * 61 + 20);
  CHECK(told(h, *cs[6], "has expired"));
  cs[6]->cmd("join");
  CHECK(told(h, *cs[6], "You have not been invited to a group."));
}

TEST_CASE("groups outlast logouts and a server restart, then lapse", "[social][group][server]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_group_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->groups_loaded());
    Client a(h);
    Client b(h);
    a.login_and_enter("gwen", "Gwenna");
    b.login_and_enter("hal", "Halric", "cleric");
    h.tick(2);
    a.cmd("invite Halric");
    b.cmd("join");
    h.tick(10);
    REQUIRE(h.server->groups().group_count() == 1);

    // Halric drops out and comes back: still in the group.
    b.send(proto::Logout{});
    h.tick(10);
    CHECK(told(h, a, "Halric has gone offline."));
    CHECK(h.server->groups().group_count() == 1);
    auto list = h.last<proto::CharList>(b.peer);  // back at character select
    REQUIRE(list);
    b.send(proto::EnterWorld{list->characters.at(0).id});
    h.tick(10);
    CHECK(told(h, b, "You are back with your group."));
    CHECK(told(h, a, "Halric is back."));
    auto st = h.last<proto::GroupState>(b.peer);
    REQUIRE(st);
    CHECK(st->members.size() == 2);
    for (const auto& m : st->members) CHECK(m.online);

    // The leader leaves: the lead passes to the member still online.
    a.send(proto::Logout{});
    h.tick(10);
    auto st2 = h.last<proto::GroupState>(b.peer);
    REQUIRE(st2);
    CHECK(st2->leader == "Halric");
    b.send(proto::Logout{});
    h.tick(10);
    CHECK(h.server->groups().group_count() == 1);  // both offline, the group waits for them
  }
  {
    // A restart: the group is back, every member offline until they log in.
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->groups_loaded());
    REQUIRE(h.server->groups().group_count() == 1);
    const Group& g = h.server->groups().all().begin()->second;
    CHECK(g.members.size() == 2);
    CHECK(g.online_count() == 0);
    Client a(h);
    return_to_world(h, a, "gwen");
    h.tick(10);
    CHECK(told(h, a, "You are back with your group."));
    auto st = h.last<proto::GroupState>(a.peer);
    REQUIRE(st);
    REQUIRE(st->members.size() == 2);
    for (const auto& m : st->members) {
      CHECK(m.online == (m.name == "Gwenna"));
      if (m.name == "Halric") CHECK(m.class_id == "cleric");  // from the saved character
    }
    // Halric never returns: once the grace period is over the group lapses, for good.
    h.now += static_cast<double>(h.content.social.group_offline_grace_seconds) + 1.0;
    h.tick(10);
    CHECK(told(h, a, "Your group has disbanded."));
    CHECK(h.server->groups().group_count() == 0);
  }
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->groups_loaded());
    CHECK(h.server->groups().group_count() == 0);
  }
  std::filesystem::remove(path);
}

TEST_CASE("a group save that fails is retried with the group as it is", "[social][group][server]") {
  Harness h(200, true);
  h.tick();
  REQUIRE(h.server->groups_loaded());
  Client a(h);
  Client b(h);
  Client c(h);
  a.login_and_enter("ina", "Inara");
  b.login_and_enter("jon", "Jorund");
  c.login_and_enter("kai", "Kaelin");
  h.tick(2);
  h.flaky->fail_groups = 1;
  a.cmd("invite Jorund");
  b.cmd("join");
  CHECK(h.flaky->real.load_groups()->empty());  // the first write failed
  // A third member joins before the retry: what is written is the group as it is now.
  h.flaky->fail_groups = 1;
  a.cmd("invite Kaelin");
  c.cmd("join");
  CHECK(h.flaky->real.load_groups()->empty());
  h.tick(20);  // the next social tick writes it again
  auto saved = h.flaky->real.load_groups();
  REQUIRE(saved);
  REQUIRE(saved->size() == 1);
  CHECK((*saved)[0].members.size() == 3);
}

TEST_CASE("guilds: founded, joined, ranked and chatted in, and all of it survives a server restart", "[social][guild][server]") {
  const auto path = std::filesystem::temp_directory_path() / ("emberfall_guild_" + std::to_string(::getpid()) + ".db");
  std::filesystem::remove(path);
  EntityId aldric_entity = 0;
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->guilds_loaded());
    Client a(h);
    Client b(h);
    aldric_entity = a.login_and_enter("alice", "Aldric").entity_id;
    b.login_and_enter("bobby", "Brenna");
    h.tick(2);
    a.cmd("guildcreate shit lords");
    CHECK(told(h, a, "not allowed"));
    a.cmd("guildcreate   the iron   VOW ");
    h.tick();
    auto info = h.last<proto::GuildInfo>(a.peer);
    REQUIRE(info);
    CHECK(info->name == "The Iron Vow");
    CHECK(info->my_rank == 0);
    REQUIRE(info->members.size() == 1);
    // Others see the guild under the founder's name.
    bool tagged = false;
    for (const auto& sp : h.received<proto::EntitySpawn>(b.peer))
      if (sp.entity_id == aldric_entity && sp.guild == "The Iron Vow") tagged = true;
    CHECK(tagged);
    b.cmd("guildcreate The Iron Vow");
    CHECK(told(h, b, "already taken"));

    a.cmd("guildinvite Brenna");
    auto inv = h.last<proto::SocialInvite>(b.peer);
    REQUIRE(inv);
    CHECK(inv->kind == 1);
    CHECK(inv->guild_name == "The Iron Vow");
    b.cmd("guildjoin");
    h.tick(20);
    auto binfo = h.last<proto::GuildInfo>(b.peer);
    REQUIRE(binfo);
    CHECK(binfo->members.size() == 2);
    CHECK(binfo->my_rank == 3);  // a recruit
    b.say_on(ChatChannel::Guild, "glad to be here");
    CHECK(heard(h, a, ChatChannel::Guild, "glad to be here", "Brenna"));
    b.cmd("guildmotd I rule now");
    CHECK(told(h, b, "rank does not allow"));
    a.cmd("guildpromote Brenna");
    a.cmd("guildpromote Brenna");  // the first is still being stored
    CHECK(told(h, a, "roster is being updated"));
    h.tick();
    a.cmd("guildpromote Brenna");
    h.tick();
    a.cmd("guildmotd Raid at dusk");
    CHECK_FALSE(heard(h, b, ChatChannel::Guild, "Raid at dusk"));  // announced only once it is saved
    h.tick();
    CHECK(heard(h, b, ChatChannel::Guild, "Raid at dusk"));
    h.tick(20);
    CHECK(h.last<proto::GuildInfo>(b.peer)->my_rank == 1);  // an officer
    h.server->shutdown("Restarting.");
    h.tick();
  }
  {
    Harness h(200, false, "proving_flats", 1, path.string());
    h.tick();
    REQUIRE(h.server->guilds_loaded());
    const Guild* g = h.server->guilds().by_name("the iron vow");
    REQUIRE(g);
    CHECK(g->motd == "Raid at dusk");
    REQUIRE(g->members.size() == 2);
    CHECK(g->member_named("Brenna")->rank == 1);
    Client a(h);
    Client b(h);
    return_to_world(h, a, "alice");
    return_to_world(h, b, "bobby");
    h.tick(20);
    CHECK(heard(h, a, ChatChannel::Guild, "Guild message of the day: Raid at dusk"));
    auto info = h.last<proto::GuildInfo>(a.peer);
    REQUIRE(info);
    CHECK(info->members.size() == 2);
    for (const auto& m : info->members) CHECK(m.online);
    a.say_on(ChatChannel::Guild, "welcome back");
    CHECK(heard(h, b, ChatChannel::Guild, "welcome back", "Aldric"));
    // The leader must hand over before leaving; the officer can then take the guild.
    a.cmd("guildleave");
    CHECK(told(h, a, "Pass leadership"));
    a.cmd("guildleader Brenna");
    h.tick();
    a.cmd("guildleave");
    h.tick();
    CHECK(h.server->guilds().of(in_zone(h, "Aldric")->char_id) == nullptr);
    b.cmd("guilddisband");
    h.tick();
    CHECK(h.server->guilds().size() == 0);
  }
  std::filesystem::remove(path);
}

TEST_CASE("friends: online notices, the list, and it is kept with the character", "[social][friends][server]") {
  Harness h;
  Client a(h);
  a.login_and_enter("alice", "Aldric");
  a.cmd("friend brenna");
  auto fl = h.last<proto::FriendsList>(a.peer);
  REQUIRE(fl);
  REQUIRE(fl->friends.size() == 1);
  CHECK(fl->friends[0].name == "Brenna");
  CHECK_FALSE(fl->friends[0].online);
  Client b(h);
  b.login_and_enter("bobby", "Brenna");
  h.tick(20);
  CHECK(told(h, a, "Your friend Brenna has entered the world."));
  fl = h.last<proto::FriendsList>(a.peer);
  REQUIRE(fl);
  CHECK(fl->friends[0].online);
  CHECK(fl->friends[0].level == 1);
  // The explicit forms never toggle: adding twice keeps the friend, unignoring a stranger ignores no one.
  a.cmd("addfriend Brenna");
  CHECK(told(h, a, "Brenna is already on your friends list."));
  CHECK(h.last<proto::FriendsList>(a.peer)->friends.size() == 1);
  a.cmd("unignore Corvin");
  CHECK(told(h, a, "You are not ignoring Corvin."));
  CHECK(h.last<proto::FriendsList>(a.peer)->ignored.empty());
  // Someone ignoring you shows as offline, in the list and in the /friend text alike.
  b.cmd("ignore Aldric");
  h.tick(20);
  CHECK_FALSE(h.last<proto::FriendsList>(a.peer)->friends[0].online);
  a.cmd("friend");
  CHECK_FALSE(told(h, a, "Brenna (online)"));
  b.cmd("unignore Aldric");
  // Relogging keeps the list.
  a.send(proto::Logout{});
  h.tick(25);
  Client a2(h);
  return_to_world(h, a2, "alice");
  fl = h.last<proto::FriendsList>(a2.peer);
  REQUIRE(fl);
  REQUIRE(fl->friends.size() == 1);
  CHECK(fl->friends[0].name == "Brenna");
}

TEST_CASE("/who lists who is online with filters", "[social][who][server]") {
  Harness h;
  Client a(h);
  Client b(h);
  Client c(h);
  a.login_and_enter("alice", "Aldric", "warrior");
  b.login_and_enter("bobby", "Brenna", "cleric");
  c.login_and_enter("corvy", "Corvin", "warrior");
  h.tick(2);
  a.cmd("who");
  auto r = h.last<proto::WhoResults>(a.peer);
  REQUIRE(r);
  CHECK(r->total == 3);
  a.cmd("who all warrior");
  r = h.last<proto::WhoResults>(a.peer);
  REQUIRE(r);
  REQUIRE(r->entries.size() == 2);
  CHECK(r->entries[0].name == "Aldric");
  CHECK(r->entries[1].name == "Corvin");
  a.cmd("who bren");
  CHECK(h.last<proto::WhoResults>(a.peer)->entries.at(0).name == "Brenna");
  a.cmd("who 20-30");
  CHECK(h.last<proto::WhoResults>(a.peer)->total == 0);
  a.cmd("who 30-20");
  CHECK(told(h, a, "Levels are written like"));
}

TEST_CASE("game masters: refused without the flag, every tool audited, bans enforced at login", "[social][admin][server]") {
  Harness h;
  {
    Client setup(h);
    setup.login("keeper");  // registers the account
    h.server->on_disconnect(setup.peer);
  }
  h.db.post([](db::Repository& repo, db::DbWorker&) { REQUIRE(repo.set_account_admin("keeper", true) == db::DbStatus::Ok); });
  Client gm(h);
  gm.login("keeper", "password123", false);
  gm.send(proto::CharCreate{"Keeper", "highlander", "warrior", "emberhold", 0});
  h.tick();
  gm.send(proto::EnterWorld{h.last<proto::CharCreateResult>(gm.peer)->character.id});
  h.tick();
  REQUIRE(h.last<proto::EnterWorldResult>(gm.peer)->ok);
  Client a(h);
  Client b(h);
  a.login_and_enter("alice", "Aldric");
  b.login_and_enter("bobby", "Brenna");
  h.tick(2);

  a.cmd("gm setlevel 50");
  CHECK(told(h, a, "You are not a game master."));
  CHECK(in_zone(h, "Aldric")->level == 1);

  gm.cmd("gm setlevel 12 Aldric");
  CHECK(in_zone(h, "Aldric")->level == 12);
  gm.cmd("gm setlevel 99");
  CHECK(told(h, gm, "Usage: /gm setlevel"));
  const std::string item = h.content.items.front().id;
  gm.cmd("gm item " + item + " 1 Brenna");
  CHECK(count_of_item(*in_zone(h, "Brenna"), item) >= 1);
  const int before_item = count_of_item(*in_zone(h, "Brenna"), item);
  gm.cmd("gm item " + item + " Brenna");  // no count: one, to Brenna (not the GM)
  CHECK(count_of_item(*in_zone(h, "Brenna"), item) == before_item + 1);
  const std::size_t mobs = h.server->zone("proving_flats")->mob_count();
  gm.cmd("gm spawn road_brigand 2");
  CHECK(h.server->zone("proving_flats")->mob_count() == mobs + 2);
  gm.cmd("gm announce Server restart in ten minutes");
  CHECK(heard(h, a, ChatChannel::Gm, "Server restart", "Keeper"));

  // Mute: chat is refused until lifted.
  gm.cmd("gm mute Aldric 30m shouting");
  h.tick();
  a.send(proto::ChatSay{"am I muted"});
  CHECK(told(h, a, "muted by a game master"));
  CHECK_FALSE(heard(h, b, ChatChannel::Say, "am I muted"));
  gm.cmd("gm unmute Aldric");
  h.tick();
  a.send(proto::ChatSay{"free again"});
  CHECK(heard(h, b, ChatChannel::Say, "free again", "Aldric"));

  // Ban: the player is removed at once and refused at login with the reason, until unbanned.
  gm.cmd("gm ban Brenna 7d griefing");
  h.tick(2);
  CHECK(h.transport.disconnecting.contains(b.peer));
  auto kick = h.last<proto::Kick>(b.peer);
  REQUIRE(kick);
  CHECK(kick->reason.find("banned") != std::string::npos);
  CHECK(kick->reason.find("griefing") != std::string::npos);
  h.server->on_disconnect(b.peer);
  Client b2(h);
  b2.hello();
  auto refused = b2.tls_login("bobby", "password123", false);
  CHECK_FALSE(refused.ok);
  CHECK(refused.message.find("banned") != std::string::npos);
  CHECK(refused.message.find("1 week") != std::string::npos);
  gm.cmd("gm unban Brenna");
  h.tick();
  Client b3(h);
  b3.hello();
  CHECK(b3.tls_login("bobby", "password123", false).ok);

  // Kick and teleport.
  h.now += 5.0;  // the command budget refills
  h.tick();
  gm.cmd("gm kick Aldric behave");
  h.tick();
  CHECK(h.transport.disconnecting.contains(a.peer));
  gm.cmd("gm tp emberglade");
  h.tick(2);
  auto moved = h.last<proto::EnterWorldResult>(gm.peer);
  REQUIRE(moved);
  CHECK(moved->zone_id == "emberglade");

  // Everything above is in the audit log.
  std::vector<db::AuditRecord> log;
  h.db.post([&](db::Repository& repo, db::DbWorker&) { log = repo.recent_audit(50); });
  std::set<std::string> seen;
  for (const auto& e : log) {
    CHECK(e.account == "keeper");
    seen.insert(e.command);
  }
  for (const char* cmd : {"setlevel", "item", "spawn", "announce", "mute", "unmute", "ban", "unban", "kick", "tp"}) {
    INFO(cmd);
    CHECK(seen.contains(cmd));
  }
  h.now += 5.0;  // the GM has been typing fast: let the command budget refill
  h.tick();
  gm.cmd("gm audit 3");
  h.tick();
  CHECK(told(h, gm, "[audit] Keeper (keeper)"));
}

TEST_CASE("hostile social traffic never crashes the server", "[social][server][fuzz]") {
  Harness h;
  Client a(h);
  Client b(h);
  a.login_and_enter("alice", "Aldric");
  b.login_and_enter("bobby", "Brenna");
  std::mt19937 rng(7);
  const std::vector<std::uint8_t> kSpeakable{0, 3, 4, 5, 7, 8, 9};
  const std::vector<std::string> words{"invite", "join", "guildcreate", "guildinvite", "guildjoin", "who", "gm", "friend", "ignore", "channel",
                                       "tell", "r", "kick", "makeleader", "lootrule", "guildmotd", "guildpromote", "assist", "\"", "été 日本",
                                       "Brenna", "Aldric", "all", "1-60", "ooc", "off", "perm", "", "    ", std::string(300, 'x')};
  for (int i = 0; i < 1500; ++i) {
    Client& c = (i % 2 == 0) ? a : b;
    std::string text;
    const int n = static_cast<int>(rng() % 5);
    for (int k = 0; k < n; ++k) text += words[rng() % words.size()] + " ";
    if (rng() % 2 == 0) c.cmd(text);
    else c.send(proto::ChatSend{kSpeakable[rng() % kSpeakable.size()], words[rng() % words.size()], text});
    if (i % 50 == 0) {
      h.now += 5.0;
      h.tick();
    }
  }
  h.tick(40);
  // Junk commands and chat are answered, never treated as protocol errors.
  CHECK_FALSE(h.transport.disconnecting.contains(a.peer));
  CHECK_FALSE(h.transport.disconnecting.contains(b.peer));
}
