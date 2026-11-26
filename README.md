# bpa_discord

Discord integration for the Betrock++ addon system.

It bridges your Minecraft server and one or more Discord channels:

- In-game chat is posted to Discord, via a webhook (player's own name and skin face) or as the bot
- Messages in the Discord channels are broadcast in-game
- **Multiple channels, webhooks and bots**, also on different Discord servers. Players choose which
  channel they talk through with `/discord select`
- A configurable prefix/postfix around Discord users' names in-game, globally and per channel
- Small `[user] reacted with :emoji:` notices for Discord reactions (can be turned off)
- Join/leave embeds and server started/stopped notices
- Slash commands: `/status` (players, TPS, MSPT, uptime, versions), `/list`, `/tps`, `/version`, and
  `/execute` (opt-in, see below)
- Crash reports: when the server crashes, the crash log is posted to the channels as a file attachment

## Compilation

Requires a C++20 compiler, CMake 3.16+ and OpenSSL development files
(`libssl-dev` / `openssl-devel` / `openssl` via vcpkg).

```bash
cmake -B build
cmake --build build
```

Or directly:

```bash
g++ -std=c++20 -shared -fPIC -fvisibility=hidden bpa_discord.cpp -o bpa_discord.so -ldpp
```

`addon_api.h` must match the `include/addon_api.h` of the Betrock++ build you run it on.

## Installation

1. Copy `bpa_discord.so` (`.dll` on Windows) into the server's `addons/` folder
2. Make sure `libdpp` can be found by the loader (system install, `LD_LIBRARY_PATH`, or next to
   the addon, which the CMake build supports through `$ORIGIN`).
3. Start the server once. `bpa_discord.json` is generated in the server's working directory; fill it in and restart.

## Configuration

Everything is configured in one file, `bpa_discord.json`, in the server's working directory (a
template with explanations is generated on first start). It is JSON with `//` and `/* */` comments
allowed: global settings at the top, then the `channels` list.

```json
{
  "token": "default bot token",
  "guild_id": "",
  "prefix": "[",
  "postfix": "]",
  "reactions": true,
  "receive": "all",
  "topic_status": false,
  "topic_online": "Online | Players: {count}",
  "topic_offline": "Offline",
  "crash_reports": true,
  "execute_role_ids": [],
  "execute_user_ids": [],
  "channels": [
    {
      "label": "main",
      "channel_id": "123456789012345678",
      "webhook_url": "https://discord.com/api/webhooks/...",
      "bot_token": "bot token for this channel"
    },
    {
      "label": "Friends",
      "channel_id": "234567890123456789",
      "guild_id": "345678901234567890",
      "bot_token": "a different bot, for a different Discord server",
      "prefix": "&aF-",
      "postfix": ":",
      "reactions": false
    }
  ]
}
```

### Global settings

| key                                    | default                      | meaning                                                                                                                                                                                      |
| -------------------------------------- | ---------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `token`                                |                              | default bot token (enable the Message Content Intent). Channels can bring their own `bot_token`.                                                                                             |
| `guild_id`                             |                              | makes slash commands appear instantly in that server instead of globally (which can take up to an hour). Channels can set their own.                                                         |
| `prefix`, `postfix`                    | `[` and `]`                  | text around a Discord user's name in-game, max 4 visible characters each, see below. Channels can override both.                                                                             |
| `reactions`                            | `true`                       | show `[name] reacted with :emoji:` in-game, see below. Channels can override it.                                                                                                             |
| `receive`                              | `"all"`                      | which channels players hear: `"all"`, or `"selected"` (only the channel they picked with `/discord select`)                                                                                  |
| `topic_status`                         | `false`                      | show live status in the channel topics. **Overwrites** the topic and needs the Manage Channels permission. Updates are rate-limited (at most one every 5 minutes). Channels can override it. |
| `topic_online`                         | `Online \| Players: {count}` | topic while running; `{count}` = players online                                                                                                                                              |
| `topic_offline`                        | `Offline`                    | topic set on shutdown                                                                                                                                                                        |
| `crash_reports`                        | `true`                       | post the crash log to the channels when the server crashes                                                                                                                                   |
| `execute_role_ids`, `execute_user_ids` | none                         | Discord ids (an array of strings or numbers, or one comma-separated string) allowed to use `/execute`. The command is only enabled when one is set.                                          |

### Channels

One entry per bridged Discord channel.

| key                                                          | required | meaning                                                                                                                          |
| ------------------------------------------------------------ | -------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `label`                                                      | yes      | name shown in-game and typed in `/discord select <label>`. Unique (case-insensitive), max 32 characters, and not `id`.           |
| `channel_id`                                                 | yes      | the Discord channel to bridge                                                                                                    |
| `webhook_url`                                                | no       | webhook of the **same** channel. Chat is relayed under each player's name and skin face. Without it, chat is relayed as the bot. |
| `bot_token`                                                  | no       | bot for this channel. Falls back to the global `token`. Channels with the same token share one bot connection.                   |
| `guild_id`, `prefix`, `postfix`, `reactions`, `topic_status` | no       | per-channel overrides of the global settings; leave a key out to use the global value                                            |

The **first** entry is the default channel: players talk through it when they join.
A bad entry (duplicate label, invalid id, no token, ...) is skipped with a warning in the server log;
the other entries still start.

### Upgrading from older versions

Older versions were configured with `bpa_discord.properties` (and `discord-*` keys in
`server.properties`). The first time the new version starts without a `bpa_discord.json`, those
settings are converted into one automatically (an old single `discord-channel-id` / `discord-webhook-url`
becomes a channel labelled `main`), the new file is read back to check it, and the old settings are
then **removed** from the files they came from: `bpa_discord.properties` is deleted when nothing else
is in it, and only the `discord-*` lines are taken out of `server.properties`. The old files are never
written to again, and `bpa_discord.json` is the only place to edit from then on. If anything goes
wrong, the old files are left untouched and the server log says why.

## Choosing a channel in-game

| command                           | who       | what                                                      |
| --------------------------------- | --------- | --------------------------------------------------------- |
| `/discord`                        | everyone  | shows the channel your chat goes to                       |
| `/discord list`                   | everyone  | lists the channels; `>` marks yours                       |
| `/discord select <label>`         | everyone  | send your chat to that channel (label may contain spaces) |
| `/discord select id <channel id>` | operators | the same, by Discord channel id                           |

Your selection lasts until you leave; on the next join you are back on the default channel.
Join/leave messages, server start/stop notices and crash reports go to every channel.
By default players hear every channel (use the prefix/postfix to tell them apart); set
`"receive": "selected"` to only hear the channel they are talking through.

## Prefix and postfix

Discord users' names are shown in-game as `<prefix>name<postfix>`, e.g. `[Steve]` with the defaults.
Each is at most 4 visible characters. Minecraft colour codes can be written as `&9`, `&c`, ... and
don't count towards the limit; the whole tag is blue unless a code changes it. In the JSON file,
leaving a key out (or `null`) uses the global setting, `""` means "none". This only affects how Discord
messages and reactions look in-game, not what is posted to Discord.

## Reactions

When someone adds a reaction in a bridged channel, players see a grey `[user] reacted with :emoji:` line
(removals are not announced, and bots are ignored). Beta's font can't draw emoji, so they are shown by
label: custom emoji by their name, common Unicode emoji by name (`:thumbsup:`, `:heart:`, ...), anything
else as `:emoji:`. Turn it off globally with `"reactions": false`, or per channel with
`"reactions": false` in that channel's entry; a per-channel value wins over the global one either way.

## Crash reports

On a server crash the bridge posts an embed (server version, players online, log path, a short
preview) with the full crash log attached as `crash-log.txt` to every channel. If the host doesn't pass
the log text but does pass a path, the file is read from there. Logs over 8 MiB are cut off. The crash
handler waits up to 8 seconds for Discord to accept the reports. With topic status on, the channel
topics are also set to the offline text.

## `/execute`

`/execute command:<command>` lets moderators run server commands from Discord. It is **disabled
unless** `execute_role_ids` or `execute_user_ids` is set, and then it is only
registered with Discord when enabled. Permissions are checked by the addon itself, not just by
Discord's command permissions, and it only works in bridged channels. Every use (and every
denied attempt) is logged on the server.

Notes:

- The command runs on the server thread with no player as sender. Discord only shows whether the server
  accepted the command; command output isn't relayed back.
- Anyone who can use it can run any server command, including `stop`. Only grant it to people you
  trust with console access.
