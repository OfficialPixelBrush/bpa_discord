# bpa_discord

Discord integration for the Betrock++ addon system.

It bridges your Minecraft server and a Discord channel:

- In-game chat is posted to Discord, via a webhook (player's own name and skin face) or as the bot.
- Messages in the Discord channel are broadcast in-game
- Join/leave embeds and server started/stopped notices
- Slash commands: `/status`, `/list`, `/version`

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
```
