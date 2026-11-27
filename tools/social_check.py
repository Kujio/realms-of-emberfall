"""Phase 7 content checks for content/social.json (mirrors server/src/game/content_social.cpp).

Used by tools/validate_content.py; kept separate so the main validator stays readable.
"""
from __future__ import annotations

from pathlib import Path

CHANNELS = ("say", "shout", "ooc", "tell", "group", "guild", "emote", "raid")
PERMISSIONS = {"invite", "remove", "promote", "demote", "motd", "speak", "disband"}


def _num(c, d: dict, key: str, where: str) -> float:
    return c.number(d, key, where)


def check_social(c, doc, rules: dict) -> None:
    """Every section and field the server reads, with the same ranges."""
    if not isinstance(doc, dict):
        c.err("social.json: must be a JSON object")
        return
    group = c.obj(doc.get("group"), "social.group")
    if isinstance(group, dict):  # an empty section still reports its missing fields
        w = "social.group"
        gmax = c.integer(group, "max_members", w)
        if not 2 <= gmax <= 6:
            c.err("social.group.max_members must be 2..6 (the wire carries six)")
        econ = rules.get("economy") if isinstance(rules.get("economy"), dict) else {}
        lpm = econ.get("loot_party_max")
        if isinstance(lpm, int) and lpm < gmax:
            c.err("rules.economy.loot_party_max must be at least social.group.max_members")
        if not 5 <= _num(c, group, "invite_seconds", w) <= 600:
            c.err("social.group.invite_seconds must be 5..600")
        if not 0.05 <= _num(c, group, "status_seconds", w) <= 10:
            c.err("social.group.status_seconds must be 0.05..10")
        if not 10 <= _num(c, group, "xp_range", w) <= 1000:
            c.err("social.group.xp_range must be 10..1000")
        if not 0 <= c.integer(group, "level_window", w) <= 59:
            c.err("social.group.level_window must be 0..59")
        if not 0 <= _num(c, group, "level_fraction", w) <= 1:
            c.err("social.group.level_fraction must be 0..1")
    raid = c.obj(doc.get("raid"), "social.raid")
    if isinstance(raid, dict):  # Phase 14; an empty section still reports its missing fields
        if not 2 <= c.integer(raid, "max_groups", "social.raid") <= 12:
            c.err("social.raid.max_groups must be 2..12 (the wire carries twelve)")
        if not 5 <= _num(c, raid, "invite_seconds", "social.raid") <= 600:
            c.err("social.raid.invite_seconds must be 5..600")
        if not 0 <= _num(c, raid, "lockout_hours", "social.raid") <= 336:
            c.err("social.raid.lockout_hours must be 0..336 (two weeks)")
        if not 5 <= _num(c, raid, "evict_seconds", "social.raid") <= 600:
            c.err("social.raid.evict_seconds must be 5..600")
    chat = c.obj(doc.get("chat"), "social.chat")
    if isinstance(chat, dict):  # an empty section still reports its missing fields
        rates = c.obj(chat.get("channels"), "social.chat.channels")
        for name in CHANNELS:
            r = rates.get(name)
            if not isinstance(r, dict):
                c.err(f"social.chat.channels.{name}: missing (every channel players speak on needs a rate)")
                continue
            rate = _num(c, r, "rate", f"social.chat.channels.{name}")
            burst = _num(c, r, "burst", f"social.chat.channels.{name}")
            if not (0 < rate <= 100 and 1 <= burst <= 100):
                c.err(f"social.chat.channels.{name}: rate must be in (0, 100] and burst in [1, 100]")
        for k in rates:
            if k not in CHANNELS:
                c.err(f"social.chat.channels: unknown channel '{k}'")
        if not 0 <= _num(c, chat, "repeat_window_seconds", "social.chat") <= 3600:
            c.err("social.chat.repeat_window_seconds must be 0..3600")
        if not 1 <= c.integer(chat, "repeat_limit", "social.chat") <= 16:
            c.err("social.chat.repeat_limit must be 1..16")
        if not 1 <= _num(c, chat, "emote_radius", "social.chat") <= 500:
            c.err("social.chat.emote_radius must be 1..500")
    flt = c.obj(doc.get("filter"), "social.filter")
    if isinstance(flt, dict):  # an empty section still reports its missing fields
        mask = c.string(flt, "mask", "social.filter")
        if len(mask) != 1 or not 0x21 <= ord(mask) <= 0x7E:
            c.err("social.filter.mask must be one printable ASCII character")
        seen: set[str] = set()
        for wd in c.str_list(flt, "words", "social.filter"):
            low = wd.lower()
            if not (1 <= len(low) <= 32 and low.isascii() and low.isalpha()):
                c.err(f"social.filter.words: '{wd}' must be 1-32 ASCII letters")
            if low in seen:
                c.err(f"social.filter.words: '{low}' is listed twice")
            seen.add(low)
    guild = c.obj(doc.get("guild"), "social.guild")
    if isinstance(guild, dict):  # an empty section still reports its missing fields
        w = "social.guild"
        nmin = c.integer(guild, "name_min", w)
        nmax = c.integer(guild, "name_max", w)
        if not (3 <= nmin <= nmax <= 32):
            c.err("social.guild: name_min/name_max must satisfy 3 <= name_min <= name_max <= 32")
        if not 2 <= c.integer(guild, "max_members", w) <= 256:
            c.err("social.guild.max_members must be 2..256 (the roster carries 256)")
        if not 5 <= _num(c, guild, "invite_seconds", w) <= 600:
            c.err("social.guild.invite_seconds must be 5..600")
        if not 1 <= c.integer(guild, "motd_max", w) <= 256:
            c.err("social.guild.motd_max must be 1..256")
        ranks = guild.get("ranks")
        if not isinstance(ranks, list):
            c.err("social.guild: 'ranks' must be an array")
            ranks = []
        if not 2 <= len(ranks) <= 8:
            c.err("social.guild.ranks must list 2..8 ranks (leader first)")
        ids: set[str] = set()
        for i, r in enumerate(ranks):
            wr = f"social.guild.ranks[{i}]"
            r = c.obj(r, wr)
            rid = c.string(r, "id", wr)
            name = c.string(r, "name", wr)
            perms = c.str_list(r, "permissions", wr)
            if not rid or not name or len(name) > 24:
                c.err(f"social.guild.ranks: rank '{rid}' needs an id and a name of 1-24 characters")
            if rid in ids:
                c.err(f"social.guild.ranks: '{rid}' is listed twice")
            ids.add(rid)
            for p in perms:
                if p not in PERMISSIONS:
                    c.err(f"{wr}: unknown permission '{p}'")
            if i == 0 and set(perms) != PERMISSIONS:
                c.err("social.guild.ranks[0] (the leader) must hold every permission")
            if i > 0 and "disband" in perms:
                c.err(f"social.guild.ranks: only the leader may disband ('{rid}')")
    friends = c.obj(doc.get("friends"), "social.friends")
    if isinstance(friends, dict):  # an empty section still reports its missing fields
        mf = c.integer(friends, "max_friends", "social.friends")
        mi = c.integer(friends, "max_ignored", "social.friends")
        if not (0 <= mf <= 64 and 0 <= mi <= 64):
            c.err("social.friends: max_friends and max_ignored must be 0..64 (the list carries 64)")
    who = c.obj(doc.get("who"), "social.who")
    if isinstance(who, dict) and not 1 <= c.integer(who, "max_results", "social.who") <= 64:
        c.err("social.who.max_results must be 1..64")


def validate_social(c, content_dir: Path, rules: dict, load) -> None:
    check_social(c, load(content_dir / "social.json", c), rules)
