// GameServer, Phase 7 game master tools: /gm commands for accounts flagged admin (is_admin, set with
// `emberfall-server --grant-admin USER`), each one written to the audit log (the audit_log table and the
// "audit" log file) with who did it, to whom, and how.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <optional>
#include <thread>

#include <spdlog/spdlog.h>

#include "auth/bans.hpp"
#include "auth/names.hpp"
#include "game/game_server.hpp"
#include "game/rules.hpp"

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

std::optional<int> read_int(const std::string& s) {
  int v = 0;
  if (s.empty() || s.size() > 9) return std::nullopt;
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
  return v;
}

std::optional<float> read_float(const std::string& s) {
  if (s.empty() || s.size() > 16) return std::nullopt;
  try {
    std::size_t used = 0;
    const float v = std::stof(s, &used);
    if (used != s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::shared_ptr<spdlog::logger> audit_log() {
  auto lg = spdlog::get("audit");
  return lg ? lg : spdlog::default_logger();
}

// Each line fits one chat message (ChatMessage.text carries 256 bytes).
const char* const kGmHelp[] = {
    "GM: /gm setlevel N [NAME] | spawn MOB [COUNT] | item ITEM [COUNT] [NAME] | tp ZONE [X Z] | goto NAME | summon NAME",
    "GM: /gm kick NAME [REASON] | ban NAME DURATION [REASON] | unban NAME | mute NAME DURATION [REASON] | unmute NAME",
    "GM: /gm announce TEXT | audit [N]. Durations: 30m, 12h, 7d, 2w, perm (a bare number is minutes).",
};

}  // namespace

void GameServer::audit(const Session& s, const std::string& command, const std::string& target, const std::string& detail) {
  audit_as(s.username, s.char_name, command, target, detail);
}

void GameServer::audit_as(const std::string& account, const std::string& character, const std::string& command, const std::string& target,
                          const std::string& detail) {
  audit_log()->info("[audit] {} ({}) /gm {} {} {}", character, account, command, target, detail);
  db::AuditRecord e{unix_now(), account, character, command, target, detail};
  db_.post([e = std::move(e)](db::Repository& repo, db::DbWorker&) {
    // A busy database gets a few tries (the action itself already happened; the file log has it either way).
    for (int attempt = 0; attempt < 3; ++attempt) {
      if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));
      if (repo.append_audit(e) == db::DbStatus::Ok) return;
    }
    spdlog::error("writing the audit log failed three times ({} {}); the entry is only in audit.log", e.command, e.target);
  });
}

void GameServer::gm_command(Session& s, const Command& c) {
  if (!s.is_admin) {
    spdlog::warn("'{}' ({}) tried a game master command: /gm {}", s.char_name, s.username, c.rest);
    return system_to(s, "You are not a game master.");
  }
  Zone* z = zone_for(s);
  PlayerEntity* me = player_of(s);
  if (!z || !me) return;
  const std::string sub = lower(c.arg(0));
  // The session a GM command names (argument `i`), else the GM's player target, else the GM.
  auto subject = [&](std::size_t i) -> Session* {
    if (c.args.size() > i && !c.args[i].empty()) return in_world_by_name(c.args[i]);
    if (me->target != 0)
      if (const PlayerEntity* t = z->find(me->target))
        if (Session* ts = find_session(t->session)) return ts;
    return &s;
  };
  auto help = [&] {
    for (const char* line : kGmHelp) system_to(s, line);
  };
  if (sub.empty() || sub == "help") return help();

  if (sub == "setlevel" || sub == "level") {
    const auto level = read_int(c.arg(1));
    if (!level || *level < 1 || *level > content_.rules.max_level)
      return system_to(s, "Usage: /gm setlevel 1-" + std::to_string(content_.rules.max_level) + " [NAME]");
    Session* t = subject(2);
    if (!t) return system_to(s, "No one named " + c.arg(2) + " is in the world.");
    Zone* tz = zone_for(*t);
    if (!tz) return;
    tz->gm_set_level(t->entity, *level, *this);
    mark_dirty(*t);
    flush_item_saves();
    audit(s, "setlevel", t->char_name, std::to_string(*level));
    if (t != &s) system_to(*t, "A game master sets your level to " + std::to_string(*level) + ".");
    return system_to(s, t->char_name + " is now level " + std::to_string(*level) + ".");
  }
  if (sub == "spawn") {
    const MobDef* def = content_.find_mob(c.arg(1));
    if (!def) return system_to(s, "Usage: /gm spawn MOB_ID [COUNT] (unknown mob '" + c.arg(1) + "').");
    const int count = std::clamp(read_int(c.arg(2)).value_or(1), 1, 20);
    for (int i = 0; i < count; ++i) z->spawn_mob(*def, {me->pos.x + 3.0f + 1.5f * static_cast<float>(i), me->pos.y, me->pos.z + 1.0f});
    audit(s, "spawn", def->id, std::to_string(count) + " in " + s.zone_id);
    return system_to(s, "You summon " + std::to_string(count) + " " + def->name + ".");
  }
  if (sub == "item" || sub == "give") {
    const ItemDef* def = content_.find_item(c.arg(1));
    if (!def) return system_to(s, "Usage: /gm item ITEM_ID [COUNT] [NAME] (unknown item '" + c.arg(1) + "').");
    // COUNT is optional: "/gm item sword Brenna" names the recipient in its place.
    const auto given = read_int(c.arg(2));
    const int count = std::clamp(given.value_or(1), 1, 1000);
    const std::size_t who_arg = given || c.arg(2).empty() ? 3 : 2;
    Session* t = subject(who_arg);
    if (!t) return system_to(s, "No one named " + c.arg(who_arg) + " is in the world.");
    Zone* tz = zone_for(*t);
    if (!tz) return;
    const bool all = tz->give(t->entity, def->id, count, *this);
    mark_dirty(*t);
    flush_item_saves();
    audit(s, "item", t->char_name, def->id + " x" + std::to_string(count) + (all ? "" : " (some did not fit)"));
    if (t != &s) system_to(*t, "A game master gives you " + def->name + ".");
    return system_to(s, (all ? "Given: " : "Some did not fit. Given: ") + def->name + " x" + std::to_string(count) + " to " + t->char_name + ".");
  }
  if (sub == "tp" || sub == "zone") {
    const ZoneDef* zd = content_.find_zone(c.arg(1));
    if (!zd) return system_to(s, "Usage: /gm tp ZONE_ID [X Z] (unknown zone '" + c.arg(1) + "').");
    Destination d{zd->id, {zd->safe_spawn.x, zd->safe_spawn.y, zd->safe_spawn.z}, zd->safe_spawn.yaw};
    if (c.args.size() >= 4) {
      const auto x = read_float(c.arg(2));
      const auto zz = read_float(c.arg(3));
      if (!x || !zz) return system_to(s, "Usage: /gm tp ZONE_ID [X Z]");
      d.pos = {std::clamp(*x, zd->min_x, zd->max_x), 0.0f, std::clamp(*zz, zd->min_z, zd->max_z)};
      d.pos.y = zd->ground_at(d.pos.x, d.pos.z);
    }
    z->gm_move(s.entity, d, *this);
    audit(s, "tp", zd->id, std::to_string(std::lround(d.pos.x)) + "," + std::to_string(std::lround(d.pos.z)));
    return system_to(s, "You travel to " + zd->name + ".");
  }
  if (sub == "goto" || sub == "summon") {
    Session* t = c.args.size() > 1 ? in_world_by_name(c.arg(1)) : nullptr;
    if (!t) return system_to(s, "Usage: /gm " + sub + " NAME (they must be in the world).");
    if (t == &s) return system_to(s, "You are already here.");
    Zone* tz = zone_for(*t);
    const PlayerEntity* tp = player_of(*t);
    if (!tz || !tp) return;
    if (sub == "goto") {
      z->gm_move(s.entity, {t->zone_id, tp->pos, tp->yaw}, *this);
      audit(s, "goto", t->char_name, t->zone_id);
      return system_to(s, "You go to " + t->char_name + ".");
    }
    tz->gm_move(t->entity, {s.zone_id, me->pos, me->yaw}, *this);
    audit(s, "summon", t->char_name, s.zone_id);
    system_to(*t, "A game master summons you.");
    return system_to(s, "You summon " + t->char_name + ".");
  }
  if (sub == "kick") {
    Session* t = c.args.size() > 1 ? in_world_by_name(c.arg(1)) : nullptr;
    if (!t) return system_to(s, "Usage: /gm kick NAME [REASON] (they must be in the world).");
    const std::string reason = auth::sanitize_chat(c.rest_after(2));
    audit(s, "kick", t->char_name, reason);
    system_to(s, "You remove " + t->char_name + " from the game.");
    return kick(*t, "You have been removed from the game by a game master." + (reason.empty() ? std::string() : " Reason: " + reason));
  }
  if (sub == "ban") return gm_restrict(s, c, true, false);
  if (sub == "unban") return gm_restrict(s, c, true, true);
  if (sub == "mute") return gm_restrict(s, c, false, false);
  if (sub == "unmute") return gm_restrict(s, c, false, true);
  if (sub == "announce" || sub == "broadcast") {
    const std::string text = auth::sanitize_chat(c.rest_after(1));
    if (text.empty()) return system_to(s, "Usage: /gm announce TEXT");
    for (const auto& [peer, o] : sessions_)
      if (o.state == State::InWorld) send_chat(o, ChatChannel::Gm, s.char_name, text);
    audit(s, "announce", "", text);
    return;
  }
  if (sub == "audit") {
    const int n = std::clamp(read_int(c.arg(1)).value_or(10), 1, 50);
    const net::PeerId peer = s.peer;
    db_.post([this, peer, n](db::Repository& repo, db::DbWorker& w) {
      auto rows = repo.recent_audit(n);
      w.complete([this, peer, rows = std::move(rows)] {
        Session* sp = find_session(peer);
        if (!sp || sp->state != State::InWorld) return;
        if (rows.empty()) return system_to(*sp, "The audit log is empty.");
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
          system_to(*sp, "[audit] " + it->character + " (" + it->account + ") " + it->command + " " + it->target + " " + it->detail);
      });
    });
    return;
  }
  system_to(s, "Unknown GM command '" + sub + "'.");
  help();
}

void GameServer::gm_restrict(Session& s, const Command& c, bool ban, bool lift) {
  const char* what = ban ? (lift ? "unban" : "ban") : (lift ? "unmute" : "mute");
  const std::string name = auth::normalize_character_name(c.arg(1));
  if (name.empty() || auth::validate_character_name(name))
    return system_to(s, std::string("Usage: /gm ") + what + " NAME" + (lift ? "" : " DURATION [REASON]") + " (NAME is a character).");
  std::int64_t seconds = 0;
  std::string reason;
  if (!lift) {
    const auto d = parse_duration(c.arg(2));
    if (!d) return system_to(s, std::string("Usage: /gm ") + what + " NAME DURATION [REASON]; DURATION like 30m, 12h, 7d, 2w or perm.");
    seconds = *d;
    reason = auth::sanitize_chat(c.rest_after(3));
  }
  const std::int64_t until = lift ? 0 : auth::restriction_until(seconds, unix_now());
  const net::PeerId peer = s.peer;
  const std::string gm_account = s.username;
  const std::string gm_char = s.char_name;
  db_.post([this, peer, name, until, reason, ban, lift, seconds, gm_account, gm_char, what = std::string(what)](db::Repository& repo, db::DbWorker& w) {
    auto acct = repo.find_account_by_character(name);
    db::DbStatus st = db::DbStatus::NotFound;
    if (acct) st = ban ? repo.set_account_ban(acct->id, until, reason) : repo.set_account_mute(acct->id, until);
    w.complete([this, peer, name, until, reason, ban, lift, seconds, gm_account, gm_char, what, acct, st] {
      Session* gm = find_session(peer);
      if (!acct) {
        if (gm) system_to(*gm, "There is no character named " + name + ".");
        return;
      }
      if (st != db::DbStatus::Ok) {
        if (gm) system_to(*gm, "That could not be saved. Please try again.");
        return;
      }
      const std::string detail = lift ? std::string("account ") + acct->username
                                      : "account " + acct->username + " for " + describe_duration(seconds) + (reason.empty() ? "" : ": " + reason);
      audit_as(gm_account, gm_char, what, name, detail);
      // Online sessions of that account feel it at once.
      for (auto& [p, o] : sessions_) {
        if (o.account_id != acct->id || o.closing) continue;
        if (ban && !lift) {
          // A short ban may already have run out by the time it is stored: then there is nothing to enforce.
          if (auto refusal = auth::ban_refusal(until, reason, unix_now())) kick(o, *refusal);
        } else if (!ban) {
          o.muted_until = until;
          if (o.state == State::InWorld)
            system_to(o, lift ? std::string("A game master has lifted your mute.") : auth::mute_notice(until, unix_now()).value_or(""));
        }
      }
      if (!gm) return;
      if (lift) system_to(*gm, name + (ban ? "'s account is no longer banned." : " may speak again."));
      else system_to(*gm, name + "'s account (" + acct->username + ") is " + (ban ? "banned " : "muted ") +
                              (seconds == kPermanent ? std::string("permanently") : "for " + describe_duration(seconds)) + ".");
    });
  });
}

}  // namespace ef::game
