# Game masters and moderation

Phase 7 tools for running a server: game master rights, the `/gm` commands, the audit log, and account bans and mutes.

## Granting game master rights

Rights are a flag on the account (`accounts.is_admin`). The operator sets it from the server host. The server does not need to be stopped; the change takes effect at the player's next login.

```sh
emberfall-server --config server.json --grant-admin USERNAME
emberfall-server --config server.json --revoke-admin USERNAME
```

In Docker:

```sh
docker compose -f deploy/docker-compose.yml exec emberfall emberfall-server --grant-admin USERNAME
```

Both commands are written to the audit log as `(console)`.

There is deliberately no in-game password. Game traffic has been encrypted since Phase 10, but a password typed into chat can still end up somewhere it shouldn't: in the chat history, in a screenshot, or in the wrong channel. Rights stay with whoever runs the server host.

## Commands

Type these in the chat box. Game masters see their usage with `/gm help`. NAME is a character name; without one, `setlevel` and `item` act on your player target (or yourself).

| Command | What it does |
|---|---|
| `/gm setlevel N [NAME]` | Sets the level (experience goes to the start of that level). |
| `/gm spawn MOB_ID [COUNT]` | Places up to 20 copies of a mob next to you. They never respawn. |
| `/gm item ITEM_ID [COUNT] [NAME]` | Puts items in a pack. |
| `/gm tp ZONE_ID [X Z]` | Takes you to a zone's safe spot, or to a point in it. |
| `/gm goto NAME`, `/gm summon NAME` | Takes you to a player, or brings them to you, across zones. |
| `/gm kick NAME [REASON]` | Disconnects a player. |
| `/gm ban NAME DURATION [REASON]` | Bans the character's account; online sessions are kicked at once. |
| `/gm unban NAME` | Lifts a ban. |
| `/gm mute NAME DURATION [REASON]` | Mutes the account on every chat channel. |
| `/gm unmute NAME` | Lifts a mute. |
| `/gm announce TEXT` | Sends a GM notice to everyone online. |
| `/gm audit [N]` | Shows the last N audit entries (10 by default, at most 50). |

Durations take a number and a unit: `30m`, `12h`, `7d` or `2w`. A bare number means minutes; `perm` means forever.

## Bans and mutes

- **Bans:** a banned account cannot log in. The login screen shows how long the ban has left and the reason. The message appears only after a correct password, so it tells someone guessing passwords nothing. Bans expire on their own.
- **Mutes:** a muted player can still play, but every chat channel answers with how long the mute has left.

Both are stored on the account, so they cover every character on it. They survive restarts.

## The audit log

Every game master action is recorded twice:

- **The `audit_log` table:** time, account, character, command, target and details. Read it with `/gm audit` or SQL.
- **`logs/audit.log`:** the same lines, rotating at 10 MB with 20 files kept (longer than the main log).

For example:

```sh
sqlite3 data/emberfall.db "SELECT datetime(at, 'unixepoch'), account, character, command, target, detail FROM audit_log ORDER BY id DESC LIMIT 20;"
```

## Player tools for reference

Players have their own moderation without a game master:

- `/ignore NAME`: hides everything that character says, and makes you look offline to them.
- `/channel NAME off`: stops shout, OOC, emotes, say or guild chat from reaching you.

The chat filter word list is in `content/social.json` (`filter.words`). Server operators can extend it, or swap in a different `ChatFilter` (server/src/game/chat.hpp).
