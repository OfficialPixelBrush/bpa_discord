# bpa_discord

Discord integration for the Betrock++ addon system.

It bridges your Minecraft server and a Discord channel:

- In-game chat is posted to Discord, via a webhook (player's own name and skin face) or as the bot.
- Messages in the Discord channel are broadcast in-game
- Join/leave embeds and server started/stopped notices
- Slash commands: `/status` (players, TPS, MSPT, uptime, versions), `/list`, `/tps`, `/version`, and
  `/execute` (opt-in, see below)
- Crash reports: when the server crashes, the crash log is posted to the channel as a file attachment

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
3. Start the server once. `bpa_discord.properties` is generated in the server's working
   directory; fill it in and restart.

```properties
discord-token=             # bot token (enable the Message Content Intent)
discord-channel-id=        # channel to bridge
discord-guild-id=          # optional, makes slash commands appear instantly
discord-webhook-url=       # optional, webhook of the same channel

# optional, show live status in the channel topic (default: false)
discord-topic-status=false
# topic while running; {count} = players online
discord-topic-online=Online | Players: {count}
# topic set on shutdown
discord-topic-offline=Offline
```

# optional, post the crash log to the channel when the server crashes (default: true)
discord-crash-reports=true

# optional, enables /execute. Comma-separated Discord role / user IDs (a match on either is enough)
discord-execute-role-ids=
discord-execute-user-ids=
```

## Crash reports

On a server crash the bridge posts an embed (server version, players online, log path, a short
preview) with the full crash log attached as `crash-log.txt`. If the host doesn't pass the log text
but does pass a path, the file is read from there. Logs over 8 MiB are cut off. The crash handler waits
up to 8 seconds for Discord to accept the report. With `discord-topic-status=true`, the channel
topic is also set to the offline text.

## `/execute`

`/execute command:<command>` lets moderators run server commands from Discord. It is **disabled
unless** `discord-execute-role-ids` or `discord-execute-user-ids` is set, and then it is only
registered with Discord when enabled. Permissions are checked by the addon itself, not just by
Discord's command permissions, and it only works in the bridged channel. Every use (and every
denied attempt) is logged on the server.

Notes:

- The command runs on the server thread with no player as sender. Discord only shows whether the server
  accepted the command; command output isn't relayed back.
- Anyone who can use it can run any server command, including `stop`. Only grant it to people you
  trust with console access.
