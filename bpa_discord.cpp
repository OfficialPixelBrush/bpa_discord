/*
 * Copyright (c) 2025-2026, Pixel Brush <pixelbrush.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/**
 * Discord bridge for Betrock++, packaged as a standalone addon.
 *
 * Features:
 *   - Minecraft chat  -> Discord (via webhook with the player's name + skin face, or as the bot)
 *   - Discord chat    -> Minecraft (messages in the configured channels)
 *   - Several channels, webhooks and bots (also on different Discord servers), listed in
 *     "bpa_discord_channels.json". Players choose where their chat goes with /discord select
 *     and see the channels with /discord list.
 *   - A prefix/postfix (max 4 characters each) around Discord users' names in-game, globally and
 *     per channel
 *   - "[user] reacted with :emoji:" notices for Discord reactions (can be turned off)
 *   - Join/leave embeds (with the current player count), server start/stop notices
 *   - Optional: live player count in the channel topic, and "Server offline" on shutdown
 *   - Slash commands: /status, /list, /tps, /version, and (opt-in) /execute
 *   - Crash reports: the crash log is posted to the channel as a file attachment
 *
 * Global configuration lives in "bpa_discord.properties" and the channel list in
 * "bpa_discord_channels.json", both in the server's working directory (templates are generated
 * on first load). For migration convenience, any property missing in the .properties file is
 * also looked up in "server.properties". The old single-channel keys (discord-channel-id,
 * discord-webhook-url) keep working when the JSON file lists no channels.
 *
 * Requires D++ (dpp). The bot needs the Message Content Intent in the Developer Portal.
 */

#include "addon_api.h"

#include <dpp/dpp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

constexpr const char* ADDON_ID = "bpa_discord";
constexpr const char* ADDON_NAME = "Discord Bridge";
constexpr const char* ADDON_VERSION = "0.3.0";
constexpr const char* CONFIG_FILE = "bpa_discord.properties";
constexpr const char* CHANNELS_FILE = "bpa_discord_channels.json";
constexpr const char* SERVER_CONFIG_FILE = "server.properties";

// Players are only sent Discord chat once they've been registered for this many server
// ticks. The join event fires *before* the Login packet is sent, and a chat packet that
// arrives ahead of Login can confuse the client.
constexpr uint64_t CHAT_READY_TICKS = 40;

// Discord rate-limits channel edits harshly (roughly 2 per 10 minutes per channel), so the
// topic is only refreshed this often; changes in between are coalesced into one update.
constexpr std::chrono::minutes TOPIC_MIN_INTERVAL{ 5 };

// How long shutdown waits for the "Server offline" topic edit before giving up.
constexpr std::chrono::milliseconds TOPIC_SHUTDOWN_WAIT{ 3000 };

// How long the crash handler waits for the crash report to reach Discord before letting the
// server finish dying.
constexpr std::chrono::milliseconds CRASH_REPORT_WAIT{ 8000 };

// Discord rejects uploads over its size limit; crash logs beyond this are cut (head kept).
constexpr size_t CRASH_LOG_MAX_BYTES = 8 * 1024 * 1024;

// Server stats (TPS/MSPT) are re-read from the host every this many ticks.
constexpr uint64_t STATS_REFRESH_TICKS = 20;

// Skin face URLs get a "?v=" bucket that changes at most this often. A changed skin then
// shows up in Discord within this window, while repeat messages inside it reuse Discord's
// cached image instead of hammering the image host.
constexpr long long SKIN_REFRESH_SECONDS = 60 * 60; // 1 hour

// /execute input limit.
constexpr size_t EXECUTE_MAX_LENGTH = 256;

// Prefix/postfix around a Discord user's name in-game: at most this many visible characters
// (colour codes are free) and this many bytes in total.
constexpr size_t AFFIX_MAX_CHARS = 4;
constexpr size_t AFFIX_MAX_BYTES = 32;
constexpr const char* DEFAULT_PREFIX = "[";
constexpr const char* DEFAULT_POSTFIX = "]";

// Longest channel label kept (players type these in /discord select).
constexpr size_t LABEL_MAX_BYTES = 32;

constexpr const char* DEFAULT_TOPIC_ONLINE = "Online | Players: {count}";
constexpr const char* DEFAULT_TOPIC_OFFLINE = "Offline";

enum class Level { Info, Warn, Error };

enum class ReceiveMode {
	All,      // players hear every bridged channel
	Selected, // players only hear the channel they currently talk through
};

struct Config {
	std::string token;   // default bot token, for channels that don't have their own bot_token
	std::string guildId; // default guild for slash commands

	// Legacy single-channel keys. Only used when bpa_discord_channels.json lists no channels.
	std::string channelId;
	std::string webhookUrl;

	// Global defaults for how Discord users are shown in-game: prefix + name + postfix.
	// Stored sanitised, so they may contain "§x" colour codes. Every channel can override them.
	std::string prefix = DEFAULT_PREFIX;
	std::string postfix = DEFAULT_POSTFIX;
	// "[name] reacted with :emoji:" notices for Discord reactions. Channels can override this.
	bool reactions = true;
	ReceiveMode receive = ReceiveMode::All;

	// Channel topic status. Off by default: it overwrites the channel's existing topic and
	// needs the bot to have the Manage Channels permission. Channels can override this.
	bool topicEnabled = false;
	std::string topicOnline = DEFAULT_TOPIC_ONLINE; // {count} = players online
	std::string topicOffline = DEFAULT_TOPIC_OFFLINE;

	// Post the crash log to the channels when the server crashes.
	bool crashReports = true;

	// /execute is only registered when at least one role or user is allowed to use it.
	std::vector<dpp::snowflake> executeRoles;
	std::vector<dpp::snowflake> executeUsers;

	bool ExecuteEnabled() const { return !executeRoles.empty() || !executeUsers.empty(); }
};

// One entry of bpa_discord_channels.json as written, before validation. An unset optional
// means "use the global setting".
struct LinkSpec {
	std::string label;
	std::string channelId;
	std::string webhookUrl;
	std::string botToken;
	std::string guildId;
	std::optional<std::string> prefix;
	std::optional<std::string> postfix;
	std::optional<bool> reactions;
	std::optional<bool> topicStatus;
};

struct Session;

// One bridged Discord channel (with its optional webhook and its resolved settings).
struct Link {
	std::string label;       // what players type in /discord select
	dpp::snowflake channel;
	std::string channelText; // the channel id as text, for /discord select id
	std::string guildId;     // empty = slash commands are registered globally
	dpp::webhook webhook;    // base webhook; name/avatar are set on a copy per message
	bool hasWebhook = false;

	std::string prefix;      // resolved (per-channel or global), already sanitised
	std::string postfix;
	bool reactions = true;
	bool topicEnabled = false;

	Session* session = nullptr; // the bot that serves this channel

	// Channel topic bookkeeping.
	bool topicEverSent = false; // server thread only
	std::chrono::steady_clock::time_point lastTopicSend{};
	std::atomic<bool> topicDirty{ false };    // server thread and D++ callbacks
	std::atomic<bool> topicInFlight{ false }; // server thread and D++ callbacks
};

// One Discord bot connection. Channels that share a bot token share a session.
struct Session {
	std::string token;
	std::unique_ptr<dpp::cluster> cluster;
	std::vector<Link*> links;
	// Guilds slash commands are registered to. Empty = register globally.
	std::vector<std::string> guildIds;
	std::atomic<bool> commandsRegistered{ false };
};

struct InboundMessage {
	enum class Kind { Chat, Reaction };
	Kind kind = Kind::Chat;
	Link* link = nullptr;
	std::string author;
	std::string content; // the chat text, or the emoji label for a reaction
};

struct PlayerEntry {
	bp_player* player;
	std::string name;
	uint64_t joinTick;
	Link* link; // the channel this player's chat is sent to
};

// An /execute invocation waiting for the server thread (host API calls are server-thread only).
struct PendingCommand {
	dpp::slashcommand_t event;
	std::string command;
	std::string user;
};

struct Bot {
	Config config;
	std::vector<std::unique_ptr<Session>> sessions;
	std::vector<std::unique_ptr<Link>> links; // never changes after start; the first is the default

	std::mutex mutex; // guards everything below
	std::queue<InboundMessage> inbound;
	std::vector<PlayerEntry> players;
	std::queue<std::pair<Level, std::string>> logs;
	std::queue<PendingCommand> commands;

	std::atomic<bool> running{ false };

	// Cached host stats. Written on the server thread, read by slash-command handlers.
	std::string serverVersion; // set once before the bots start
	std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
	std::atomic<double> tps{ -1.0 }; // < 0 = not sampled yet
	std::atomic<double> mspt{ -1.0 };

	// Server thread only.
	uint64_t tick = 0;
	bool startNoticeSent = false;
	bool crashReported = false;
};

const bp_api* g_api = nullptr;
std::unique_ptr<Bot> g_bot;

// ---------------------------------------------------------------------------------
// Logging. The host logger is only safe to use from the server thread, so anything
// that runs on a D++ thread goes through a queue that is flushed on server ticks.
// ---------------------------------------------------------------------------------

void LogNow(Level _level, const std::string& _msg) {
	if (!g_api)
		return;
	switch (_level) {
	case Level::Info:
		g_api->log.info(g_api, _msg.c_str());
		break;
	case Level::Warn:
		g_api->log.warning(g_api, _msg.c_str());
		break;
	case Level::Error:
		g_api->log.error(g_api, _msg.c_str());
		break;
	}
}

void LogAsync(Bot& _bot, Level _level, std::string _msg) {
	std::lock_guard lock(_bot.mutex);
	_bot.logs.emplace(_level, std::move(_msg));
}

void FlushLogs(Bot& _bot) {
	std::queue<std::pair<Level, std::string>> pending;
	{
		std::lock_guard lock(_bot.mutex);
		pending.swap(_bot.logs);
	}
	while (!pending.empty()) {
		LogNow(pending.front().first, pending.front().second);
		pending.pop();
	}
}

// ---------------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------------

void ReplaceAll(std::string& _haystack, const std::string& _from, const std::string& _to) {
	if (_from.empty())
		return;
	size_t pos = 0;
	while ((pos = _haystack.find(_from, pos)) != std::string::npos) {
		_haystack.replace(pos, _from.size(), _to);
		pos += _to.size();
	}
}

std::string Trim(const std::string& _s) {
	size_t b = 0, e = _s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(_s[b])))
		++b;
	while (e > b && std::isspace(static_cast<unsigned char>(_s[e - 1])))
		--e;
	return _s.substr(b, e - b);
}

bool ParseBool(const std::string& _value, bool _default) {
	std::string v = Trim(_value);
	std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (v == "true" || v == "1" || v == "yes" || v == "on")
		return true;
	if (v == "false" || v == "0" || v == "no" || v == "off")
		return false;
	return _default;
}

std::string FormatTopic(const std::string& _template, size_t _count) {
	std::string topic = _template;
	ReplaceAll(topic, "{count}", std::to_string(_count));
	return topic;
}

// Escapes Discord markdown so names like "my_cool_name" don't render as italics.
std::string EscapeMarkdown(const std::string& _input) {
	std::string out;
	out.reserve(_input.size());
	for (const char c : _input) {
		if (c == '\\' || c == '_' || c == '*' || c == '~' || c == '`' || c == '|' || c == '>')
			out += '\\';
		out += c;
	}
	return out;
}

std::string FormatDuration(std::chrono::seconds _total) {
	const long long secs = _total.count();
	const long long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
	if (d > 0)
		return std::format("{}d {}h {}m", d, h, m);
	if (h > 0)
		return std::format("{}h {}m", h, m);
	return std::format("{}m {}s", m, secs % 60);
}

// Splits "123, 456;789" into snowflakes; invalid entries are skipped.
std::vector<dpp::snowflake> ParseSnowflakeList(const std::string& _value) {
	std::vector<dpp::snowflake> out;
	std::string token;
	auto flush = [&] {
		if (!token.empty()) {
			if (dpp::snowflake id{ token }; id != 0)
				out.push_back(id);
			token.clear();
		}
	};
	for (const char c : _value) {
		if (c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)))
			flush();
		else
			token += c;
	}
	flush();
	return out;
}

std::string RemoveMinecraftFormatting(const std::string& _input) {
	std::string result;
	result.reserve(_input.size());

	for (size_t i = 0; i < _input.size(); ++i) {
		// UTF-8 encoding of § is C2 A7, followed by one format code character
		if (static_cast<unsigned char>(_input[i]) == 0xC2 && i + 1 < _input.size() &&
		    static_cast<unsigned char>(_input[i + 1]) == 0xA7) {
			i += 1;
			if (i + 1 < _input.size())
				i += 1;
			continue;
		}
		result += _input[i];
	}

	return result;
}

// Cuts to at most _maxBytes without splitting a UTF-8 sequence.
std::string TruncateUtf8(const std::string& _s, size_t _maxBytes) {
	if (_s.size() <= _maxBytes)
		return _s;
	size_t end = _maxBytes;
	while (end > 0 && (static_cast<unsigned char>(_s[end]) & 0xC0) == 0x80)
		--end;
	return _s.substr(0, end);
}

std::string ToLower(std::string _s) {
	std::transform(_s.begin(), _s.end(), _s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return _s;
}

bool EqualsIgnoreCase(const std::string& _a, const std::string& _b) {
	return ToLower(_a) == ToLower(_b);
}

// Digits only; anything else (or 0) gives an empty snowflake instead of relying on how D++
// handles garbage input.
dpp::snowflake ParseSnowflake(const std::string& _text) {
	if (_text.empty() || _text.size() > 20 ||
	    !std::all_of(_text.begin(), _text.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
		return dpp::snowflake{};
	return dpp::snowflake{ _text };
}

// Cleans a configured prefix/postfix: "&x" colour codes become "§x", control characters are
// dropped, and the result is cut to AFFIX_MAX_CHARS visible characters (colour codes are free,
// but the whole thing is still capped at AFFIX_MAX_BYTES). Sets *_truncated when text was cut.
std::string SanitizeAffix(const std::string& _input, bool* _truncated = nullptr) {
	const auto isColorCode = [](char c) {
		const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return (l >= '0' && l <= '9') || (l >= 'a' && l <= 'f') || (l >= 'k' && l <= 'o') || l == 'r';
	};

	std::string out;
	size_t visible = 0;
	bool cut = false;

	for (size_t i = 0; i < _input.size();) {
		const unsigned char c = static_cast<unsigned char>(_input[i]);

		// "&x", or a literal "§x" (UTF-8: C2 A7 followed by the code character)
		size_t codeLen = 0;
		if (c == '&' && i + 1 < _input.size() && isColorCode(_input[i + 1]))
			codeLen = 2;
		else if (c == 0xC2 && i + 2 < _input.size() && static_cast<unsigned char>(_input[i + 1]) == 0xA7 &&
		         isColorCode(_input[i + 2]))
			codeLen = 3;

		if (codeLen != 0) {
			std::string code = "\xC2\xA7";
			code += static_cast<char>(std::tolower(static_cast<unsigned char>(_input[i + codeLen - 1])));
			if (out.size() + code.size() > AFFIX_MAX_BYTES) {
				cut = true;
				break;
			}
			out += code;
			i += codeLen;
			continue;
		}

		// One UTF-8 character.
		size_t len = 1;
		if (c >= 0xF0)
			len = 4;
		else if (c >= 0xE0)
			len = 3;
		else if (c >= 0xC0)
			len = 2;
		if (i + len > _input.size())
			len = _input.size() - i;

		if (c < 0x20 || c == 0x7F) {
			i += len;
			continue;
		}
		if (visible >= AFFIX_MAX_CHARS || out.size() + len > AFFIX_MAX_BYTES) {
			cut = true;
			break;
		}
		out.append(_input, i, len);
		++visible;
		i += len;
	}

	if (_truncated)
		*_truncated = cut;
	return out;
}

// Builds "§9<prefix>name<postfix> §f…" lines, each at most 119 bytes (tag included).
// The prefix/postfix are already sanitised and may carry their own colour codes.
std::vector<std::string> FormatDiscordChatLines(const std::string& _author, const std::string& _content,
                                                const std::string& _prefix, const std::string& _postfix) {
	static constexpr size_t MAX_LINE = 119;

	std::string author = RemoveMinecraftFormatting(_author);
	std::string content = RemoveMinecraftFormatting(_content);
	for (char& c : content) {
		if (c == '\n' || c == '\r' || c == '\t')
			c = ' ';
	}

	auto makePrefix = [&](const std::string& name) {
		return "§9" + _prefix + name + _postfix + " §f";
	};

	std::string prefix = makePrefix(author);
	if (prefix.size() >= MAX_LINE) {
		const size_t overhead = makePrefix("").size();
		const size_t maxName = overhead < MAX_LINE ? MAX_LINE - overhead : 0;
		if (author.size() > maxName)
			author = TruncateUtf8(author, maxName);
		prefix = makePrefix(author);
	}

	const size_t contentBudget = MAX_LINE > prefix.size() ? MAX_LINE - prefix.size() : 0;
	std::vector<std::string> lines;

	if (contentBudget == 0) {
		lines.push_back(prefix.substr(0, MAX_LINE));
		return lines;
	}

	if (content.empty()) {
		lines.push_back(prefix);
		return lines;
	}

	size_t offset = 0;
	while (offset < content.size()) {
		size_t end = std::min(offset + contentBudget, content.size());
		// Don't split a multi-byte UTF-8 sequence across two lines.
		if (end < content.size()) {
			size_t safe = end;
			while (safe > offset && (static_cast<unsigned char>(content[safe]) & 0xC0) == 0x80)
				--safe;
			if (safe > offset)
				end = safe;
		}
		lines.push_back(prefix + content.substr(offset, end - offset));
		offset = end;
	}

	return lines;
}

// "<prefix>name<postfix> reacted with :emoji:" as one grey line of at most 119 bytes.
std::string FormatReactionLine(const std::string& _author, const std::string& _label, const std::string& _prefix,
                               const std::string& _postfix) {
	static constexpr size_t MAX_LINE = 119;

	const std::string head = "§7" + _prefix;
	const std::string tail = _postfix + " reacted with " + RemoveMinecraftFormatting(_label);
	const size_t fixed = head.size() + tail.size();
	const size_t nameBudget = fixed < MAX_LINE ? MAX_LINE - fixed : 0;

	return head + TruncateUtf8(RemoveMinecraftFormatting(_author), nameBudget) + tail;
}

// Face-only skin render, cropped server-side by the image host. Discord fetches this
// URL itself when it renders the message, so we never download or decode a skin.
// The "?v=" bucket busts Discord's (and the host CDN's) cache at most once per
// SKIN_REFRESH_SECONDS; it's offset per name so players don't all roll over at once.
std::string BuildSkinAvatarUrl(const std::string& _username, int _size) {
	std::string safe;
	safe.reserve(_username.size());
	for (const char c : _username) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
			safe += c;
	}
	if (safe.empty())
		safe = "MHF_Steve";

	const long long now = std::chrono::duration_cast<std::chrono::seconds>(
	                          std::chrono::system_clock::now().time_since_epoch())
	                          .count();
	const long long offset = static_cast<long long>(std::hash<std::string>{}(safe) % SKIN_REFRESH_SECONDS);
	const long long bucket = (now + offset) / SKIN_REFRESH_SECONDS;

	return "https://mc-heads.net/avatar/" + safe + "/" + std::to_string(_size) + "?v=" + std::to_string(bucket);
}

// Clamps to Discord's webhook username rules (1-80 chars, no "discord"/"clyde").
std::string SanitizeWebhookUsername(const std::string& _username) {
	std::string name = RemoveMinecraftFormatting(_username);

	auto maskSubstring = [&name](const std::string& _needle) {
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(),
		               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		size_t pos = 0;
		while ((pos = lower.find(_needle, pos)) != std::string::npos) {
			for (size_t i = 0; i < _needle.size(); ++i) {
				name[pos + i] = '_';
				lower[pos + i] = '_';
			}
			pos += _needle.size();
		}
	};
	maskSubstring("discord");
	maskSubstring("clyde");

	if (name.size() > 80)
		name = name.substr(0, 80);
	if (name.empty())
		name = "Player";
	return name;
}

std::string DisplayNameForMention(const dpp::user& _user, const dpp::guild_member& _member) {
	const std::string nick = _member.get_nickname();
	if (!nick.empty())
		return nick;
	if (!_user.global_name.empty())
		return _user.global_name;
	return _user.username;
}

// Discord stores mentions as <@id> / <@!id> / <#id> / <@&id>; Minecraft has no renderer
// for those, so resolve them to readable @name / #name before broadcasting.
std::string ResolveDiscordMentions(const dpp::message& _msg) {
	std::string content = _msg.content;

	for (const auto& [user, member] : _msg.mentions) {
		const std::string idStr = std::to_string(static_cast<uint64_t>(user.id));
		const std::string label = "@" + DisplayNameForMention(user, member);
		ReplaceAll(content, "<@!" + idStr + ">", label);
		ReplaceAll(content, "<@" + idStr + ">", label);
	}

	for (const dpp::snowflake roleId : _msg.mention_roles) {
		const std::string idStr = std::to_string(static_cast<uint64_t>(roleId));
		std::string label = "@role";
		if (const dpp::role* role = dpp::find_role(roleId); role && !role->name.empty())
			label = "@" + role->name;
		ReplaceAll(content, "<@&" + idStr + ">", label);
	}

	for (const dpp::channel& channel : _msg.mention_channels) {
		const std::string idStr = std::to_string(static_cast<uint64_t>(channel.id));
		const std::string label = channel.name.empty() ? "#channel" : ("#" + channel.name);
		ReplaceAll(content, "<#" + idStr + ">", label);
	}

	return content;
}

// Beta's font can't draw emoji, so reactions are shown by label. Custom emoji carry their own
// name; Unicode emoji are looked up in this table (skin tones and the VS16 selector are
// ignored). Anything not listed shows as ":emoji:".
const std::map<std::string, std::string>& UnicodeEmojiLabels() {
	static const std::map<std::string, std::string> labels = {
	    { "👍", "thumbsup" },       { "👎", "thumbsdown" },       { "❤", "heart" },
	    { "💔", "broken_heart" },   { "😀", "grinning" },         { "😃", "smiley" },
	    { "😄", "smile" },          { "😁", "grin" },             { "😆", "laughing" },
	    { "😅", "sweat_smile" },    { "😂", "joy" },              { "🤣", "rofl" },
	    { "😊", "blush" },          { "🙂", "slight_smile" },     { "😉", "wink" },
	    { "😍", "heart_eyes" },     { "😘", "kissing_heart" },    { "😎", "sunglasses" },
	    { "🤔", "thinking" },       { "😮", "open_mouth" },       { "😢", "cry" },
	    { "😭", "sob" },            { "😡", "rage" },             { "😠", "angry" },
	    { "😱", "scream" },         { "😴", "sleeping" },         { "🥺", "pleading_face" },
	    { "🤷", "shrug" },          { "🤦", "facepalm" },         { "💀", "skull" },
	    { "💩", "poop" },           { "🎉", "tada" },             { "🔥", "fire" },
	    { "👀", "eyes" },           { "🙏", "pray" },             { "👏", "clap" },
	    { "🙌", "raised_hands" },   { "💪", "muscle" },           { "👋", "wave" },
	    { "🤝", "handshake" },      { "💯", "100" },              { "✅", "white_check_mark" },
	    { "❌", "x" },              { "❓", "question" },         { "❗", "exclamation" },
	    { "➕", "heavy_plus_sign" }, { "➖", "heavy_minus_sign" }, { "⭐", "star" },
	    { "✨", "sparkles" },       { "⚡", "zap" },              { "🚀", "rocket" },
	    { "🎮", "video_game" },     { "⛏", "pick" },
	};
	return labels;
}

std::string EmojiLabel(const dpp::emoji& _emoji) {
	if (_emoji.id != 0) // custom emoji
		return _emoji.name.empty() ? ":emoji:" : ":" + _emoji.name + ":";

	std::string key = _emoji.name;
	ReplaceAll(key, "\xEF\xB8\x8F", ""); // variation selector 16
	for (unsigned char tone = 0xBB; tone <= 0xBF; ++tone) // skin tone modifiers U+1F3FB..1F3FF
		ReplaceAll(key, std::string("\xF0\x9F\x8F") + static_cast<char>(tone), "");

	const auto& labels = UnicodeEmojiLabels();
	if (const auto it = labels.find(key); it != labels.end())
		return ":" + it->second + ":";
	return ":emoji:";
}

// ---------------------------------------------------------------------------------
// Link lookup
// ---------------------------------------------------------------------------------

// Case-insensitive match on the in-game label.
Link* FindLinkByLabel(Bot& _bot, const std::string& _label) {
	const std::string wanted = Trim(_label);
	for (auto& link : _bot.links) {
		if (EqualsIgnoreCase(link->label, wanted))
			return link.get();
	}
	return nullptr;
}

Link* FindLinkByChannelId(Bot& _bot, const std::string& _id) {
	const std::string wanted = Trim(_id);
	for (auto& link : _bot.links) {
		if (link->channelText == wanted)
			return link.get();
	}
	return nullptr;
}

// The link a Discord event belongs to, among the channels this bot serves.
Link* FindLinkForChannel(Session& _session, dpp::snowflake _channel) {
	for (Link* link : _session.links) {
		if (link->channel == _channel)
			return link;
	}
	return nullptr;
}

std::string LabelList(const Session& _session) {
	std::string out;
	for (const Link* link : _session.links) {
		if (!out.empty())
			out += ", ";
		out += link->label;
	}
	return out;
}

bool CanSend(const Bot& _bot, const Link& _link) {
	return _bot.running.load() && _link.session && _link.session->cluster;
}

// ---------------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------------

std::map<std::string, std::string> ReadProperties(const char* _path) {
	std::map<std::string, std::string> props;
	std::ifstream file(_path);
	if (!file)
		return props;

	std::string line;
	while (std::getline(file, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == '#' || line[0] == '!')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		props[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
	}
	return props;
}

void WriteConfigTemplate() {
	std::ofstream file(CONFIG_FILE);
	if (!file) {
		LogNow(Level::Warn, std::string("Couldn't create ") + CONFIG_FILE);
		return;
	}
	file << "# bpa_discord configuration\n"
	        "#\n"
	        "# Which channels are bridged, and with which bot / webhook, is set in\n"
	        "# bpa_discord_channels.json. This file holds the defaults and the global options.\n"
	        "\n"
	        "# Default bot token from the Discord Developer Portal (enable the Message Content Intent).\n"
	        "# Entries in bpa_discord_channels.json can use their own \"bot_token\" instead.\n"
	        "discord-token=\n"
	        "# Optional: register slash commands to one guild instantly. Leave empty for global\n"
	        "# (global registration can take up to an hour to appear). Entries can set their own \"guild_id\".\n"
	        "discord-guild-id=\n"
	        "\n"
	        "# How Discord users are shown in-game: <prefix>name<postfix>. Each is at most 4 visible\n"
	        "# characters; colour codes such as &9 don't count towards that. Entries in\n"
	        "# bpa_discord_channels.json can override both. Defaults: [ and ]\n"
	        "discord-prefix=[\n"
	        "discord-postfix=]\n"
	        "\n"
	        "# Show \"[name] reacted with :thumbsup:\" in-game when someone reacts to a message in a\n"
	        "# bridged channel. Entries can override this with \"reactions\".\n"
	        "discord-reactions=true\n"
	        "\n"
	        "# Which Discord channels players hear in-game when several are bridged:\n"
	        "#   all      - every bridged channel\n"
	        "#   selected - only the channel they picked with /discord select\n"
	        "discord-receive=all\n"
	        "\n"
	        "# Optional: show live server status in the channel topics. This OVERWRITES the topic and\n"
	        "# requires the bot to have the Manage Channels permission. Updates are rate-limited\n"
	        "# (at most one every 5 minutes), so the count may lag slightly behind.\n"
	        "# Entries can override this with \"topic_status\".\n"
	        "discord-topic-status=false\n"
	        "# Topic while the server is running. {count} is replaced with the player count.\n"
	        "discord-topic-online=Online | Players: {count}\n"
	        "# Topic set when the server shuts down.\n"
	        "discord-topic-offline=Offline\n"
	        "\n"
	        "# Post the crash log to the channels (as a file) when the server crashes.\n"
	        "discord-crash-reports=true\n"
	        "\n"
	        "# Optional: let Discord moderators run server commands with /execute. The command is\n"
	        "# only registered when at least one of these is set (comma-separated Discord IDs).\n"
	        "# Only works in the bridged channels. A role match OR a user match is enough.\n"
	        "discord-execute-role-ids=\n"
	        "discord-execute-user-ids=\n";
}

void WriteChannelsTemplate() {
	std::ofstream file(CHANNELS_FILE);
	if (!file) {
		LogNow(Level::Warn, std::string("Couldn't create ") + CHANNELS_FILE);
		return;
	}
	file << "// bpa_discord channels. One entry per bridged Discord channel; // comments are allowed.\n"
	        "// Entries can point at different channels, webhooks, bots and even different Discord servers.\n"
	        "//\n"
	        "// label        what players type in /discord select <label>. Required, unique, max 32 chars.\n"
	        "//              The FIRST entry is the default channel players talk through when they join.\n"
	        "// channel_id   channel to bridge (required)\n"
	        "// webhook_url  optional webhook of the SAME channel: chat is relayed under each player's own\n"
	        "//              name + skin face. Leave out to relay chat as the bot.\n"
	        "// bot_token    optional bot for this channel; falls back to discord-token in bpa_discord.properties.\n"
	        "//              Channels with the same token share one bot connection.\n"
	        "//\n"
	        "// More optional keys (leave them out to use the global setting from bpa_discord.properties):\n"
	        "//   \"guild_id\"     register slash commands to this server instantly\n"
	        "//   \"prefix\"       text before a Discord user's name in-game, max 4 characters (colour codes like &9 are free)\n"
	        "//   \"postfix\"      text after it, max 4 characters. \"\" means none.\n"
	        "//   \"reactions\"    true/false: show \"[name] reacted with :emoji:\" in-game\n"
	        "//   \"topic_status\" true/false: show the live status in this channel's topic\n"
	        "[\n"
	        "\t{\n"
	        "\t\t\"label\": \"main\",\n"
	        "\t\t\"channel_id\": \"\",\n"
	        "\t\t\"webhook_url\": \"\",\n"
	        "\t\t\"bot_token\": \"\"\n"
	        "\t}\n"
	        "]\n";
}

// Text value of a JSON key; numbers are accepted too (channel ids are often pasted as numbers).
std::string JsonText(const nlohmann::json& _obj, const char* _key) {
	const auto it = _obj.find(_key);
	if (it == _obj.end())
		return {};
	if (it->is_string())
		return Trim(it->get<std::string>());
	if (it->is_number_unsigned())
		return std::to_string(it->get<uint64_t>());
	if (it->is_number_integer())
		return std::to_string(it->get<int64_t>());
	return {};
}

// Only a string counts as "set"; a missing key or null means "use the global setting".
std::optional<std::string> JsonOptionalText(const nlohmann::json& _obj, const char* _key) {
	const auto it = _obj.find(_key);
	if (it == _obj.end() || !it->is_string())
		return std::nullopt;
	return it->get<std::string>();
}

std::optional<bool> JsonOptionalBool(const nlohmann::json& _obj, const char* _key) {
	const auto it = _obj.find(_key);
	if (it == _obj.end())
		return std::nullopt;
	if (it->is_boolean())
		return it->get<bool>();
	if (it->is_string()) {
		const std::string text = it->get<std::string>();
		const bool asTrue = ParseBool(text, true);
		if (asTrue == ParseBool(text, false)) // recognised, so both defaults agree
			return asTrue;
	}
	return std::nullopt;
}

// Reads bpa_discord_channels.json (comments allowed). Returns false when the file doesn't exist.
bool LoadChannelSpecs(std::vector<LinkSpec>& _out) {
	std::ifstream file(CHANNELS_FILE, std::ios::binary);
	if (!file)
		return false;
	std::stringstream buffer;
	buffer << file.rdbuf();

	nlohmann::json root;
	try {
		root = nlohmann::json::parse(buffer.str(), nullptr, true, /*ignore_comments=*/true);
	} catch (const std::exception& e) {
		LogNow(Level::Error, std::string("Couldn't parse ") + CHANNELS_FILE + ": " + e.what());
		return true;
	}
	if (!root.is_array()) {
		LogNow(Level::Error, std::string(CHANNELS_FILE) + " must contain a JSON array of channel entries");
		return true;
	}

	for (const auto& item : root) {
		if (!item.is_object()) {
			LogNow(Level::Warn, std::string("Ignoring an entry in ") + CHANNELS_FILE + " that is not an object");
			continue;
		}
		LinkSpec spec;
		spec.label = JsonText(item, "label");
		spec.channelId = JsonText(item, "channel_id");
		spec.webhookUrl = JsonText(item, "webhook_url");
		spec.botToken = JsonText(item, "bot_token");
		if (spec.botToken.empty())
			spec.botToken = JsonText(item, "token");
		spec.guildId = JsonText(item, "guild_id");
		spec.prefix = JsonOptionalText(item, "prefix");
		spec.postfix = JsonOptionalText(item, "postfix");
		spec.reactions = JsonOptionalBool(item, "reactions");
		spec.topicStatus = JsonOptionalBool(item, "topic_status");
		_out.push_back(std::move(spec));
	}
	return true;
}

Config LoadConfig() {
	auto own = ReadProperties(CONFIG_FILE);
	if (own.empty()) {
		if (!std::ifstream(CONFIG_FILE).good()) {
			WriteConfigTemplate();
			if (!std::ifstream(CHANNELS_FILE).good())
				WriteChannelsTemplate();
			LogNow(Level::Warn, std::string("Generated ") + CONFIG_FILE + " and " + CHANNELS_FILE +
			                        "; fill them in and restart the server.");
		}
	}
	const auto fallback = ReadProperties(SERVER_CONFIG_FILE);

	auto get = [&](const char* _key) {
		if (auto it = own.find(_key); it != own.end() && !it->second.empty())
			return it->second;
		if (auto it = fallback.find(_key); it != fallback.end())
			return it->second;
		return std::string{};
	};

	Config cfg;
	cfg.token = get("discord-token");
	cfg.channelId = get("discord-channel-id");
	cfg.guildId = get("discord-guild-id");
	cfg.webhookUrl = get("discord-webhook-url");

	// An empty prefix/postfix is a valid choice ("none"), so only a missing key means "default".
	auto readAffix = [&](const char* _key, std::string& _target) {
		const auto it = own.find(_key);
		if (it == own.end())
			return;
		bool cut = false;
		_target = SanitizeAffix(it->second, &cut);
		if (cut)
			LogNow(Level::Warn, std::format("{} is longer than {} characters and was cut to \"{}\"", _key,
			                                AFFIX_MAX_CHARS, RemoveMinecraftFormatting(_target)));
	};
	readAffix("discord-prefix", cfg.prefix);
	readAffix("discord-postfix", cfg.postfix);

	cfg.reactions = ParseBool(get("discord-reactions"), true);
	cfg.receive = ToLower(Trim(get("discord-receive"))) == "selected" ? ReceiveMode::Selected : ReceiveMode::All;

	cfg.topicEnabled = ParseBool(get("discord-topic-status"), false);
	if (const std::string v = get("discord-topic-online"); !v.empty())
		cfg.topicOnline = v;
	if (const std::string v = get("discord-topic-offline"); !v.empty())
		cfg.topicOffline = v;
	cfg.crashReports = ParseBool(get("discord-crash-reports"), true);
	cfg.executeRoles = ParseSnowflakeList(get("discord-execute-role-ids"));
	cfg.executeUsers = ParseSnowflakeList(get("discord-execute-user-ids"));
	return cfg;
}

// The channel entries to bridge: bpa_discord_channels.json if it lists any, otherwise the single
// channel from the old discord-channel-id / discord-webhook-url keys.
std::vector<LinkSpec> LoadLinkSpecs(const Config& _cfg) {
	std::vector<LinkSpec> specs;
	LoadChannelSpecs(specs);

	if (!specs.empty()) {
		if (!_cfg.channelId.empty())
			LogNow(Level::Warn, std::string("discord-channel-id is ignored because ") + CHANNELS_FILE +
			                        " lists channels; add it there instead.");
		return specs;
	}

	if (!_cfg.channelId.empty()) {
		LinkSpec legacy;
		legacy.label = "main";
		legacy.channelId = _cfg.channelId;
		legacy.webhookUrl = _cfg.webhookUrl;
		specs.push_back(std::move(legacy));
	}
	return specs;
}

// ---------------------------------------------------------------------------------
// Discord output (safe to call from the server thread; callbacks log asynchronously)
// ---------------------------------------------------------------------------------

dpp::message MakeMessage(const Link& _link, const std::string& _content) {
	dpp::message msg(_link.channel, _content);
	// Player-controlled text must never be able to ping @everyone, roles or users.
	msg.set_allowed_mentions(false, false, false, false, {}, {});
	return msg;
}

void SendConfirmCallbackLog(Bot* _bot, const Link* _link, const char* _what,
                            const dpp::confirmation_callback_t& _result) {
	if (_result.is_error())
		LogAsync(*_bot, Level::Warn,
		         std::string("Failed to send ") + _what + " to '" + _link->label + "': " + _result.get_error().message);
}

// Sends one player's chat message to the channel they are currently talking through.
void SendPlayerChat(Bot& _bot, Link& _link, const std::string& _username, const std::string& _message) {
	if (!CanSend(_bot, _link))
		return;

	const std::string content = RemoveMinecraftFormatting(_message);
	if (content.empty())
		return;

	Bot* bot = &_bot;
	Link* link = &_link;
	dpp::cluster& cluster = *_link.session->cluster;

	if (_link.hasWebhook) {
		dpp::webhook wh = _link.webhook;
		wh.name = SanitizeWebhookUsername(_username);
		wh.avatar_url = BuildSkinAvatarUrl(_username, 128);

		dpp::message msg(content);
		msg.set_allowed_mentions(false, false, false, false, {}, {});
		cluster.execute_webhook(wh, msg, false, 0, "", [bot, link](const dpp::confirmation_callback_t& result) {
			SendConfirmCallbackLog(bot, link, "chat via webhook", result);
		});
		return;
	}

	// No webhook configured for this channel: plain message under the bot's identity.
	cluster.message_create(
	    MakeMessage(_link, std::format("[{}] {}", RemoveMinecraftFormatting(_username), content)),
	    [bot, link](const dpp::confirmation_callback_t& result) { SendConfirmCallbackLog(bot, link, "chat message", result); });
}

// Join/leave embeds go to every bridged channel.
void SendPlayerEvent(Bot& _bot, const std::string& _rawUsername, bool _joined, size_t _onlineCount) {
	if (!_bot.running.load())
		return;

	const std::string username = RemoveMinecraftFormatting(_rawUsername);

	dpp::embed embed;
	embed.set_color(_joined ? 0x55FF55 : 0xFF5555)
	    .set_author(username, "", BuildSkinAvatarUrl(username, 256))
	    .set_description(std::format("**{}** {} the game", username, _joined ? "joined" : "left"))
	    .set_footer(dpp::embed_footer().set_text(std::format("{} player{} online", _onlineCount, _onlineCount == 1 ? "" : "s")));

	Bot* bot = &_bot;
	for (auto& linkPtr : _bot.links) {
		Link* link = linkPtr.get();
		if (!CanSend(_bot, *link))
			continue;

		dpp::message msg(link->channel, embed);
		msg.set_allowed_mentions(false, false, false, false, {}, {});
		link->session->cluster->message_create(msg, [bot, link](const dpp::confirmation_callback_t& result) {
			SendConfirmCallbackLog(bot, link, "join/leave embed", result);
		});
	}
}

// Server started/stopped style notices go to every bridged channel.
void SendNotice(Bot& _bot, const std::string& _text, uint32_t _color) {
	if (!_bot.running.load())
		return;

	dpp::embed embed;
	embed.set_color(_color).set_description(_text);

	Bot* bot = &_bot;
	for (auto& linkPtr : _bot.links) {
		Link* link = linkPtr.get();
		if (!CanSend(_bot, *link))
			continue;

		dpp::message msg(link->channel, embed);
		link->session->cluster->message_create(
		    msg, [bot, link](const dpp::confirmation_callback_t& result) { SendConfirmCallbackLog(bot, link, "notice", result); });
	}
}

// Sets the channel topic. Fetches the channel first and edits that object, so only the topic
// changes and the channel's other settings are written back untouched. The callback runs on
// a D++ thread. Needs the Manage Channels permission.
void SetChannelTopic(dpp::cluster& _cluster, dpp::snowflake _channel, const std::string& _topic,
                     std::function<void(bool ok, const std::string& error)> _done) {
	nlohmann::json body = { { "topic", _topic } };
	const std::string url =
	    "https://discord.com/api/v10/channels/" + std::to_string(static_cast<uint64_t>(_channel));

	_cluster.request(
	    url, dpp::m_patch,
	    [_done](const dpp::http_request_completion_t& r) {
		    if (r.status >= 200 && r.status < 300)
			    _done(true, "");
		    else
			    _done(false, "HTTP " + std::to_string(r.status) + ": " + r.body);
	    },
	    body.dump(), "application/json",
	    { { "Authorization", "Bot " + _cluster.token },
	      { "User-Agent", "DiscordBot (bpa_discord, 1.0)" } });
}

// ---------------------------------------------------------------------------------
// Player registry (shared between the server thread and D++ threads)
// ---------------------------------------------------------------------------------

std::vector<std::string> OnlineNames(Bot& _bot) {
	std::lock_guard lock(_bot.mutex);
	std::vector<std::string> names;
	names.reserve(_bot.players.size());
	for (const auto& p : _bot.players)
		names.push_back(p.name);
	return names;
}

// Server thread only. Every channel with topic status on gets its own rate-limit window, since
// Discord limits edits per channel.
void MarkTopicsDirty(Bot& _bot) {
	for (auto& link : _bot.links) {
		if (link->topicEnabled)
			link->topicDirty.store(true);
	}
}

// Server thread only. Pushes the current player count to the channel topic when it changed
// and the rate-limit window allows. Several joins/leaves in a row collapse into one edit.
void PumpLinkTopic(Bot& _bot, Link& _link) {
	if (!_link.topicEnabled || !_link.topicDirty.load() || !CanSend(_bot, _link))
		return;
	if (_link.topicInFlight.load())
		return;

	const auto now = std::chrono::steady_clock::now();
	if (_link.topicEverSent && now - _link.lastTopicSend < TOPIC_MIN_INTERVAL)
		return;

	_link.topicDirty.store(false);
	_link.topicInFlight.store(true);
	_link.topicEverSent = true;
	_link.lastTopicSend = now;

	Bot* bot = &_bot;
	Link* link = &_link;
	SetChannelTopic(*_link.session->cluster, _link.channel,
	                FormatTopic(_bot.config.topicOnline, OnlineNames(_bot).size()),
	                [bot, link](bool ok, const std::string& error) {
		                if (!ok) {
			                LogAsync(*bot, Level::Warn,
			                         "Failed to update the topic of '" + link->label +
			                             "' (does the bot have Manage Channels?): " + error);
			                link->topicDirty.store(true); // retry on a later tick, still rate-limited
		                }
		                link->topicInFlight.store(false);
	                });
}

void PumpTopicUpdates(Bot& _bot) {
	for (auto& link : _bot.links)
		PumpLinkTopic(_bot, *link);
}

// ---------------------------------------------------------------------------------
// Slash command replies (run on D++ threads; they only touch the cached stats)
// ---------------------------------------------------------------------------------

uint32_t TpsColor(double _tps) {
	if (_tps < 0)
		return 0x99AAB5; // not sampled yet
	if (_tps >= 19.0)
		return 0x55FF55;
	if (_tps >= 15.0)
		return 0xFFAA00;
	return 0xFF5555;
}

std::string FormatTps(double _tps) {
	return _tps < 0 ? "n/a" : std::format("{:.2f}", _tps);
}

std::string FormatMspt(double _mspt) {
	return _mspt < 0 ? "n/a" : std::format("{:.2f} ms", _mspt);
}

std::string ServerVersionOrUnknown(const Bot& _bot) {
	return _bot.serverVersion.empty() ? "unknown" : _bot.serverVersion;
}

dpp::message EmbedMessage(const dpp::embed& _embed) {
	dpp::message msg;
	msg.add_embed(_embed);
	msg.set_allowed_mentions(false, false, false, false, {}, {});
	return msg;
}

dpp::message BuildStatusReply(Bot& _bot) {
	const size_t online = OnlineNames(_bot).size();
	const double tps = _bot.tps.load();
	const double mspt = _bot.mspt.load();
	const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - _bot.startedAt);

	dpp::embed embed;
	embed.set_title("Server status")
	    .set_color(TpsColor(tps))
	    .add_field("Players", std::to_string(online), true)
	    .add_field("TPS", FormatTps(tps), true)
	    .add_field("MSPT", FormatMspt(mspt), true)
	    .add_field("Uptime", FormatDuration(uptime), true)
	    .add_field("Server", ServerVersionOrUnknown(_bot), true)
	    .add_field("Bridge", std::string(ADDON_NAME) + " " + ADDON_VERSION, true);
	return EmbedMessage(embed);
}

dpp::message BuildListReply(Bot& _bot) {
	const auto names = OnlineNames(_bot);

	dpp::embed embed;
	embed.set_title(std::format("Players online ({})", names.size())).set_color(names.empty() ? 0x99AAB5 : 0x55FF55);

	if (names.empty()) {
		embed.set_description("No players online.");
	} else {
		std::string body;
		size_t shown = 0;
		for (const std::string& name : names) {
			const std::string line = "• " + EscapeMarkdown(RemoveMinecraftFormatting(name)) + "\n";
			if (body.size() + line.size() > 3800)
				break; // embed descriptions cap at 4096 chars
			body += line;
			++shown;
		}
		if (shown < names.size())
			body += std::format("…and {} more", names.size() - shown);
		embed.set_description(body);
	}
	return EmbedMessage(embed);
}

dpp::message BuildTpsReply(Bot& _bot) {
	const double tps = _bot.tps.load();
	dpp::embed embed;
	embed.set_title("Performance")
	    .set_color(TpsColor(tps))
	    .add_field("TPS", FormatTps(tps), true)
	    .add_field("MSPT", FormatMspt(_bot.mspt.load()), true);
	return EmbedMessage(embed);
}

dpp::message BuildVersionReply(Bot& _bot) {
	dpp::embed embed;
	embed.set_title("Versions")
	    .set_color(0x5865F2)
	    .add_field("Discord bridge", std::string(ADDON_NAME) + " " + ADDON_VERSION, true)
	    .add_field("Server", ServerVersionOrUnknown(_bot), true);
	return EmbedMessage(embed);
}

// ---------------------------------------------------------------------------------
// /execute: lets configured Discord roles/users run server commands. The permission
// check happens here (not only via Discord's own command permissions, which a guild
// admin can override), and the command itself runs on the server thread.
// ---------------------------------------------------------------------------------

bool IsExecuteAuthorized(const Config& _cfg, const dpp::interaction& _interaction) {
	const dpp::snowflake userId = _interaction.get_issuing_user().id;
	if (std::find(_cfg.executeUsers.begin(), _cfg.executeUsers.end(), userId) != _cfg.executeUsers.end())
		return true;

	for (const dpp::snowflake role : _interaction.member.get_roles()) {
		if (std::find(_cfg.executeRoles.begin(), _cfg.executeRoles.end(), role) != _cfg.executeRoles.end())
			return true;
	}
	return false;
}

void HandleExecuteCommand(Bot& _bot, Session& _session, const dpp::slashcommand_t& _event) {
	auto deny = [&](const std::string& _why) { _event.reply(dpp::message(_why).set_flags(dpp::m_ephemeral)); };
	const dpp::interaction& interaction = _event.command;
	const std::string who = std::format("{} ({})", interaction.get_issuing_user().username,
	                                    static_cast<uint64_t>(interaction.get_issuing_user().id));

	if (!_bot.config.ExecuteEnabled())
		return deny("`/execute` is not enabled.");
	if (!FindLinkForChannel(_session, interaction.channel_id))
		return deny("`/execute` can only be used in a bridged channel.");
	if (!IsExecuteAuthorized(_bot.config, interaction)) {
		LogAsync(_bot, Level::Warn, "Discord user " + who + " tried /execute without permission");
		return deny("You don't have permission to use `/execute`.");
	}

	const dpp::command_value param = _event.get_parameter("command");
	const std::string* raw = std::get_if<std::string>(&param);
	std::string command = raw ? Trim(*raw) : std::string{};
	while (!command.empty() && command.front() == '/')
		command.erase(command.begin()); // accept both "stop" and "/stop"
	command = Trim(command);

	if (command.empty())
		return deny("Please provide a command.");
	if (command.size() > EXECUTE_MAX_LENGTH)
		return deny(std::format("Command is too long (max {} characters).", EXECUTE_MAX_LENGTH));
	if (std::any_of(command.begin(), command.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; }))
		return deny("Commands can't contain control characters or newlines.");

	// Acknowledge now (Discord wants a reply within 3 s); the result is filled in once the
	// server thread has actually run the command.
	_event.thinking(false);
	std::lock_guard lock(_bot.mutex);
	_bot.commands.push(PendingCommand{ _event, std::move(command), who });
}

// Server thread only.
void RunPendingCommands(Bot& _bot, const bp_api* _api) {
	std::queue<PendingCommand> pending;
	{
		std::lock_guard lock(_bot.mutex);
		pending.swap(_bot.commands);
	}

	while (!pending.empty()) {
		PendingCommand cmd = std::move(pending.front());
		pending.pop();

		LogNow(Level::Info, "Discord user " + cmd.user + " ran command: /" + cmd.command);
		// There's no player behind a Discord command, so there is no sender.
		const bool ok = _api->cmd.execute(_api, nullptr, cmd.command.c_str());

		std::string shown = cmd.command;
		std::replace(shown.begin(), shown.end(), '`', '\'');

		dpp::embed embed;
		embed.set_color(ok ? 0x55FF55 : 0xFF5555)
		    .set_description(std::format("{} `/{}`", ok ? "✅ Command executed:" : "❌ The server didn't accept:", shown))
		    .set_footer(dpp::embed_footer().set_text("Run by " + cmd.user));
		cmd.event.edit_original_response(EmbedMessage(embed));
	}
}

// ---------------------------------------------------------------------------------
// Crash reports
// ---------------------------------------------------------------------------------

std::string ReadFileCapped(const std::string& _path, size_t _maxBytes) {
	std::ifstream file(_path, std::ios::binary);
	if (!file)
		return {};
	std::string data(_maxBytes + 1, '\0'); // +1 so "too large" can be detected
	file.read(data.data(), static_cast<std::streamsize>(data.size()));
	data.resize(static_cast<size_t>(file.gcount()));
	return data;
}

// Runs inside the host's crash event. The process is about to die, so this posts the
// report (and the offline topics, if enabled) to every bridged channel and then waits a
// bounded time for Discord.
void SendCrashReport(Bot& _bot, const std::string& _version, const std::string& _logPath, std::string _log) {
	if (_log.empty() && !_logPath.empty())
		_log = ReadFileCapped(_logPath, CRASH_LOG_MAX_BYTES);

	bool truncated = false;
	if (_log.size() > CRASH_LOG_MAX_BYTES) {
		_log = TruncateUtf8(_log, CRASH_LOG_MAX_BYTES);
		truncated = true;
	}

	dpp::embed embed;
	embed.set_title("Server crashed!")
	    .set_color(0xFF5555)
	    .add_field("Server version", _version.empty() ? ServerVersionOrUnknown(_bot) : _version, true)
	    .add_field("Players online", std::to_string(OnlineNames(_bot).size()), true);
	if (!_logPath.empty())
		embed.add_field("Log file", "`" + _logPath + "`", false);

	if (!_log.empty()) {
		// A short preview inline; the whole log is attached.
		std::string excerpt = TruncateUtf8(_log, 900);
		const bool cut = excerpt.size() < _log.size();
		ReplaceAll(excerpt, "```", "'''");
		embed.set_description("```\n" + excerpt + (cut ? "\n…" : "") + "\n```");
		if (truncated)
			embed.set_footer(dpp::embed_footer().set_text("Log was too large and was cut off at 8 MiB"));
	} else {
		embed.set_description("No crash log was provided by the server.");
	}

	// Counts requests still in flight. It is raised before each request goes out, and the
	// waiting loop only starts after all of them were sent.
	auto pending = std::make_shared<std::atomic<int>>(0);
	Bot* bot = &_bot;

	for (auto& linkPtr : _bot.links) {
		Link* link = linkPtr.get();
		if (!CanSend(_bot, *link))
			continue;

		dpp::message msg(link->channel, embed);
		msg.set_allowed_mentions(false, false, false, false, {}, {});
		if (!_log.empty())
			msg.add_file("crash-log.txt", _log, "text/plain");

		pending->fetch_add(1);
		link->session->cluster->message_create(msg, [bot, link, pending](const dpp::confirmation_callback_t& result) {
			SendConfirmCallbackLog(bot, link, "crash report", result);
			pending->fetch_sub(1);
		});

		if (link->topicEnabled) {
			pending->fetch_add(1);
			SetChannelTopic(*link->session->cluster, link->channel, _bot.config.topicOffline,
			                [bot, link, pending](bool ok, const std::string& error) {
				                if (!ok)
					                LogAsync(*bot, Level::Warn,
					                         "Couldn't set the offline topic of '" + link->label +
					                             "' after a crash: " + error);
				                pending->fetch_sub(1);
			                });
		}
	}

	const auto deadline = std::chrono::steady_clock::now() + CRASH_REPORT_WAIT;
	while (pending->load() > 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(25));

	FlushLogs(_bot);
	if (pending->load() > 0)
		LogNow(Level::Warn, "Crash report may not have reached Discord (timed out)");
	else
		LogNow(Level::Info, "Crash report sent to Discord");
}

// ---------------------------------------------------------------------------------
// Bot lifecycle
// ---------------------------------------------------------------------------------

// If the Gateway threads refuse to stop in time we have to abandon them. They execute code
// that lives in this shared object, so make sure the host's dlclose()/FreeLibrary() can't
// unmap it from under them.
void PinSelf() {
#ifdef _WIN32
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                   reinterpret_cast<LPCWSTR>(&PinSelf), &self);
#else
	Dl_info info{};
	if (dladdr(reinterpret_cast<void*>(&PinSelf), &info) && info.dli_fname)
		dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
#endif
}

// Turns the parsed channel entries into validated links and groups them into one session per
// bot token. A bad entry is skipped with a warning so one typo doesn't take the others down.
void BuildLinks(Bot& _bot, const std::vector<LinkSpec>& _specs) {
	const Config& cfg = _bot.config;

	auto resolveAffix = [](const std::optional<std::string>& _own, const std::string& _fallback,
	                       const std::string& _label, const char* _what) {
		if (!_own)
			return _fallback;
		bool cut = false;
		std::string value = SanitizeAffix(*_own, &cut);
		if (cut)
			LogNow(Level::Warn, std::format("Channel '{}': {} is longer than {} characters and was cut to \"{}\"",
			                                _label, _what, AFFIX_MAX_CHARS, RemoveMinecraftFormatting(value)));
		return value;
	};

	for (size_t i = 0; i < _specs.size(); ++i) {
		const LinkSpec& spec = _specs[i];

		std::string label = RemoveMinecraftFormatting(Trim(spec.label));
		if (label.empty())
			label = std::format("channel{}", i + 1);
		label = TruncateUtf8(label, LABEL_MAX_BYTES);

		const std::string entry = std::format("Channel entry #{} ('{}')", i + 1, label);

		if (EqualsIgnoreCase(label, "id")) {
			LogNow(Level::Error, entry + ": the label \"id\" is reserved for /discord select id; skipped");
			continue;
		}
		if (FindLinkByLabel(_bot, label)) {
			LogNow(Level::Warn, entry + ": another channel already uses this label; skipped");
			continue;
		}

		const dpp::snowflake channel = ParseSnowflake(spec.channelId);
		if (channel == 0) {
			LogNow(Level::Warn, entry + ": channel_id is missing or not a valid snowflake; skipped");
			continue;
		}
		const std::string channelText = std::to_string(static_cast<uint64_t>(channel));
		if (FindLinkByChannelId(_bot, channelText)) {
			LogNow(Level::Warn, entry + ": this channel is already bridged by another entry; skipped");
			continue;
		}

		const std::string token = !spec.botToken.empty() ? spec.botToken : cfg.token;
		if (token.empty()) {
			LogNow(Level::Warn, entry + ": no bot_token, and discord-token is empty; skipped");
			continue;
		}

		std::string guildId = !spec.guildId.empty() ? spec.guildId : cfg.guildId;
		if (!guildId.empty() && ParseSnowflake(guildId) == 0) {
			LogNow(Level::Warn, entry + ": guild id is invalid; registering its slash commands globally "
			                            "(can take up to an hour to appear).");
			guildId.clear();
		}

		auto link = std::make_unique<Link>();
		link->label = label;
		link->channel = channel;
		link->channelText = channelText;
		link->guildId = guildId;
		link->prefix = resolveAffix(spec.prefix, cfg.prefix, label, "prefix");
		link->postfix = resolveAffix(spec.postfix, cfg.postfix, label, "postfix");
		link->reactions = spec.reactions.value_or(cfg.reactions);
		link->topicEnabled = spec.topicStatus.value_or(cfg.topicEnabled);

		if (!spec.webhookUrl.empty()) {
			try {
				link->webhook = dpp::webhook(spec.webhookUrl);
				link->hasWebhook = true;
			} catch (const std::exception& e) {
				LogNow(Level::Warn, entry + ": webhook_url is invalid (" + e.what() +
				                        "); player chat will be relayed as the bot instead.");
			}
		}

		Session* session = nullptr;
		for (auto& existing : _bot.sessions) {
			if (existing->token == token) {
				session = existing.get();
				break;
			}
		}
		if (!session) {
			_bot.sessions.push_back(std::make_unique<Session>());
			session = _bot.sessions.back().get();
			session->token = token;
		}
		link->session = session;
		session->links.push_back(link.get());
		_bot.links.push_back(std::move(link));
	}

	// Slash commands are registered per guild only when every channel of a bot names its guild;
	// otherwise the bot registers them globally (which covers all of its guilds).
	for (auto& session : _bot.sessions) {
		bool everyChannelHasGuild = true;
		for (const Link* link : session->links)
			everyChannelHasGuild = everyChannelHasGuild && !link->guildId.empty();
		if (!everyChannelHasGuild)
			continue;
		for (const Link* link : session->links) {
			if (std::find(session->guildIds.begin(), session->guildIds.end(), link->guildId) == session->guildIds.end())
				session->guildIds.push_back(link->guildId);
		}
	}
}

// A webhook URL can silently point at the wrong channel or be stale. Check each one once at
// startup so that's obvious immediately.
void VerifyWebhooks(Bot& _bot, Session& _session) {
	Bot* bot = &_bot;
	for (Link* link : _session.links) {
		if (!link->hasWebhook)
			continue;

		const dpp::snowflake webhookId = link->webhook.id;
		const std::string webhookToken = link->webhook.token;
		_session.cluster->get_webhook_with_token(
		    webhookId, webhookToken, [bot, link](const dpp::confirmation_callback_t& result) {
			    if (result.is_error()) {
				    LogAsync(*bot, Level::Error,
				             "The webhook of channel '" + link->label + "' could not be verified (" +
				                 result.get_error().message +
				                 "); it is likely deleted/regenerated. Player chat will fail to relay there until "
				                 "it's replaced with a current webhook URL.");
				    return;
			    }
			    const dpp::webhook wh = result.get<dpp::webhook>();
			    if (wh.channel_id != link->channel) {
				    LogAsync(*bot, Level::Warn,
				             std::format("The webhook of channel '{}' posts to channel {}, but its channel_id is "
				                         "{}. Player chat will relay to a different channel than /status, /list "
				                         "and join/leave messages.",
				                         link->label, static_cast<uint64_t>(wh.channel_id),
				                         static_cast<uint64_t>(link->channel)));
			    } else {
				    LogAsync(*bot, Level::Info,
				             std::format("Webhook of '{}' verified, chat will relay to channel {}", link->label,
				                         static_cast<uint64_t>(wh.channel_id)));
			    }
		    });
	}
}

void StartBot() {
	auto bot = std::make_unique<Bot>();
	bot->config = LoadConfig();
	if (g_api && g_api->server.getVersion) {
		if (const char* version = g_api->server.getVersion())
			bot->serverVersion = version;
	}

	BuildLinks(*bot, LoadLinkSpecs(bot->config));

	if (bot->links.empty()) {
		LogNow(Level::Warn, std::string("No usable Discord channel is configured. Add one to ") + CHANNELS_FILE +
		                        " (and set discord-token in " + CONFIG_FILE +
		                        ", or a bot_token per channel); the Discord bot will not start.");
		g_bot = std::move(bot); // keep it around so the (empty) registry still works
		return;
	}

	Bot* raw = bot.get();
	g_bot = std::move(bot);
	raw->running.store(true);

	size_t started = 0;
	for (auto& sessionPtr : raw->sessions) {
		Session* sess = sessionPtr.get();

		try {
			const uint32_t intents = dpp::i_default_intents | dpp::i_message_content | dpp::i_guild_message_reactions;
			auto cluster = std::make_unique<dpp::cluster>(sess->token, intents);

			// With several bots, say which one a log line is about.
			const std::string logTag = raw->sessions.size() > 1 ? "[" + LabelList(*sess) + "] " : std::string{};

			cluster->on_log([raw, logTag](const dpp::log_t& event) {
				// 10062 = Unknown interaction (acked too late / raced). Noise once we reply properly.
				if (event.message.find("10062") != std::string::npos)
					return;
				if (event.severity >= dpp::ll_error)
					LogAsync(*raw, Level::Error, logTag + event.message);
				else if (event.severity == dpp::ll_warning)
					LogAsync(*raw, Level::Warn, logTag + event.message);
			});

			cluster->on_ready([raw, sess](const dpp::ready_t&) {
				// on_ready also fires after every reconnect; only register once per bot.
				if (sess->commandsRegistered.exchange(true))
					return;
				if (!sess->cluster)
					return;

				const dpp::snowflake me = sess->cluster->me.id;
				// Bulk create replaces the guild/global command set.
				std::vector<dpp::slashcommand> commands{
					dpp::slashcommand("status", "Show server status: players, TPS, MSPT and uptime", me),
					dpp::slashcommand("list", "List online Minecraft players", me),
					dpp::slashcommand("tps", "Show server TPS and MSPT", me),
					dpp::slashcommand("version", "Show the Discord bridge and server versions", me),
				};
				// Only offered when someone is actually allowed to use it.
				if (raw->config.ExecuteEnabled()) {
					commands.push_back(dpp::slashcommand("execute", "Run a server command (moderators only)", me)
					                       .add_option(dpp::command_option(dpp::co_string, "command",
					                                                       "Command to run, without the leading /", true)));
				}

				if (!sess->guildIds.empty()) {
					for (const std::string& guildId : sess->guildIds)
						sess->cluster->guild_bulk_command_create(commands, dpp::snowflake{ guildId });
					LogAsync(*raw, Level::Info, "Registered guild slash commands for " + LabelList(*sess));
				} else {
					sess->cluster->global_bulk_command_create(commands);
					LogAsync(*raw, Level::Info,
					         "Registered global slash commands for " + LabelList(*sess) + " (may take up to an hour)");
				}
			});

			cluster->on_message_create([raw, sess](const dpp::message_create_t& event) {
				if (!raw->running.load())
					return;
				Link* link = FindLinkForChannel(*sess, event.msg.channel_id);
				if (!link)
					return;
				// Skips our own posts too, including webhook-relayed player chat.
				if (event.msg.author.is_bot() || event.msg.webhook_id != 0)
					return;
				if (event.msg.content.empty())
					return;

				InboundMessage chat;
				chat.kind = InboundMessage::Kind::Chat;
				chat.link = link;
				const std::string nick = event.msg.member.get_nickname();
				chat.author = !nick.empty() ? nick : event.msg.author.username;
				chat.content = ResolveDiscordMentions(event.msg);

				std::lock_guard lock(raw->mutex);
				raw->inbound.push(std::move(chat));
			});

			cluster->on_message_reaction_add([raw, sess](const dpp::message_reaction_add_t& event) {
				if (!raw->running.load())
					return;
				Link* link = FindLinkForChannel(*sess, event.channel_id);
				if (!link || !link->reactions)
					return;
				if (event.reacting_user.is_bot())
					return;

				InboundMessage reaction;
				reaction.kind = InboundMessage::Kind::Reaction;
				reaction.link = link;
				reaction.author = DisplayNameForMention(event.reacting_user, event.reacting_member);
				reaction.content = EmojiLabel(event.reacting_emoji);

				std::lock_guard lock(raw->mutex);
				raw->inbound.push(std::move(reaction));
			});

			cluster->on_slashcommand([raw, sess](const dpp::slashcommand_t& event) {
				if (!raw->running.load())
					return;

				const std::string name = event.command.get_command_name();

				if (name == "status")
					event.reply(BuildStatusReply(*raw));
				else if (name == "list")
					event.reply(BuildListReply(*raw));
				else if (name == "tps")
					event.reply(BuildTpsReply(*raw));
				else if (name == "version")
					event.reply(BuildVersionReply(*raw));
				else if (name == "execute")
					HandleExecuteCommand(*raw, *sess, event);
				else
					event.reply(dpp::message("Unknown command.").set_flags(dpp::m_ephemeral));
			});

			sess->cluster = std::move(cluster);
			sess->cluster->start(dpp::st_return);
			++started;
			LogNow(Level::Info, "Gateway bot started for " + LabelList(*sess));

			VerifyWebhooks(*raw, *sess);
		} catch (const std::exception& e) {
			LogNow(Level::Error, "Failed to start the bot for " + LabelList(*sess) + ": " + e.what());
			sess->cluster.reset();
		}
	}

	if (started == 0)
		raw->running.store(false);
}

void StopBot() {
	if (!g_bot)
		return;

	Bot& bot = *g_bot;
	bot.running.store(false);

	// One shutdown job per bot connection.
	struct ShutdownJob {
		std::unique_ptr<dpp::cluster> cluster;
		std::vector<Link*> links;
	};
	std::vector<ShutdownJob> jobs;
	{
		std::lock_guard lock(bot.mutex);
		for (auto& session : bot.sessions) {
			if (session->cluster)
				jobs.push_back(ShutdownJob{ std::move(session->cluster), session->links });
		}
		while (!bot.inbound.empty())
			bot.inbound.pop();
		while (!bot.commands.empty())
			bot.commands.pop();
	}

	if (jobs.empty()) {
		FlushLogs(bot);
		g_bot.reset();
		return;
	}

	LogNow(Level::Info, "Shutting down...");

	Bot* botPtr = &bot;
	auto remaining = std::make_shared<std::atomic<int>>(static_cast<int>(jobs.size()));

	// Ownership moves into workers so a stuck SSL/event-loop join can't freeze server exit.
	// DPP's cluster::shutdown() joins the engine thread; if that thread is blocked inside
	// OpenSSL (common when Discord HTTP is unhealthy) the join never returns. Each bot gets
	// its own worker so one stuck connection doesn't hold up the others' goodbyes.
	std::vector<std::thread> workers;
	for (ShutdownJob& job : jobs) {
		workers.emplace_back([cluster = std::move(job.cluster), links = std::move(job.links), remaining,
		                      botPtr]() mutable {
			try {
				// Fire-and-forget goodbye; never wait on Discord here.
				for (Link* link : links) {
					cluster->message_create(dpp::message(link->channel, "Server stopped!"),
					                        [](const dpp::confirmation_callback_t&) {});
				}

				// Bounded wait so the topic edits can go out before the gateway is torn down. If
				// Discord is rate-limiting us they won't make it, and the previous topics stay.
				std::vector<std::shared_ptr<std::atomic<bool>>> topicFlags;
				for (Link* link : links) {
					if (!link->topicEnabled)
						continue;
					auto flag = std::make_shared<std::atomic<bool>>(false);
					topicFlags.push_back(flag);
					SetChannelTopic(*cluster, link->channel, botPtr->config.topicOffline,
					                [botPtr, link, flag](bool ok, const std::string& error) {
						                if (!ok)
							                LogAsync(*botPtr, Level::Warn,
							                         "Couldn't set the offline topic of '" + link->label + "': " + error);
						                flag->store(true);
					                });
				}

				if (topicFlags.empty()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(150));
				} else {
					const auto topicDeadline = std::chrono::steady_clock::now() + TOPIC_SHUTDOWN_WAIT;
					const auto allDone = [&] {
						return std::all_of(topicFlags.begin(), topicFlags.end(),
						                   [](const auto& f) { return f->load(); });
					};
					while (!allDone() && std::chrono::steady_clock::now() < topicDeadline)
						std::this_thread::sleep_for(std::chrono::milliseconds(25));
				}
				cluster->shutdown();
				cluster.reset();
			} catch (...) {
			}
			remaining->fetch_sub(1);
		});
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (remaining->load() > 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

	if (remaining->load() == 0) {
		for (std::thread& worker : workers)
			worker.join();
		FlushLogs(bot);
		LogNow(Level::Info, "Shut down");
		g_bot.reset();
	} else {
		LogNow(Level::Warn, "Shutdown timed out; abandoning Gateway threads so the server can exit");
		for (std::thread& worker : workers)
			worker.detach();
		PinSelf();
		// Deliberately leak the Bot: the abandoned threads may still hold pointers to it.
		(void)g_bot.release();
	}
}

// ---------------------------------------------------------------------------------
// /discord command: lets players choose which bridged channel their chat goes to.
//   /discord                      shows where your chat goes
//   /discord list                 lists the channels
//   /discord select <name>        pick a channel by its label (everyone)
//   /discord select id <id>       pick a channel by its Discord channel id (operators)
// Command actions run on the server thread.
// ---------------------------------------------------------------------------------

void CommandReply(const bp_api* _api, const bp_cmd_action_ctx* _ctx, const std::string& _text) {
	_api->cmd.sendActionFeedback(_ctx, _text.c_str());
}

// The player who ran the command, or NULL for the console.
bp_player* CommandPlayer(const bp_api* _api, const bp_cmd_action_ctx* _ctx) {
	bp_entity* entity = _api->cmd.getActionExecutor(_ctx);
	return entity ? _api->entity.toPlayer(entity) : nullptr;
}

Link* LinkOfPlayer(Bot& _bot, bp_player* _player) {
	std::lock_guard lock(_bot.mutex);
	for (const auto& p : _bot.players) {
		if (p.player == _player)
			return p.link;
	}
	return nullptr;
}

void CmdDiscordInfo(const bp_api* _api, const bp_cmd_action_ctx* _ctx, void*) {
	if (!g_bot)
		return;
	if (bp_player* player = CommandPlayer(_api, _ctx)) {
		if (const Link* link = LinkOfPlayer(*g_bot, player))
			CommandReply(_api, _ctx, "Your chat is sent to the Discord channel '" + link->label + "'.");
	}
	CommandReply(_api, _ctx, "/discord list - show the channels, /discord select <name> - switch channel");
}

void CmdDiscordList(const bp_api* _api, const bp_cmd_action_ctx* _ctx, void*) {
	if (!g_bot)
		return;
	const Link* current = nullptr;
	if (bp_player* player = CommandPlayer(_api, _ctx))
		current = LinkOfPlayer(*g_bot, player);

	CommandReply(_api, _ctx, "Discord channels (> is where your chat goes):");
	for (const auto& link : g_bot->links)
		CommandReply(_api, _ctx, std::string(link.get() == current ? "> " : "  ") + link->label);
}

void SelectLink(const bp_api* _api, const bp_cmd_action_ctx* _ctx, Link* _link) {
	bp_player* player = CommandPlayer(_api, _ctx);
	if (!player) {
		CommandReply(_api, _ctx, "Only players can choose a Discord channel.");
		return;
	}
	if (!_link) {
		CommandReply(_api, _ctx, "No such Discord channel. Use /discord list to see them.");
		return;
	}

	bool found = false;
	{
		std::lock_guard lock(g_bot->mutex);
		for (auto& p : g_bot->players) {
			if (p.player == player) {
				p.link = _link;
				found = true;
				break;
			}
		}
	}
	if (!found) {
		CommandReply(_api, _ctx, "Couldn't change your Discord channel right now.");
		return;
	}
	CommandReply(_api, _ctx, "Your chat is now sent to the Discord channel '" + _link->label + "'.");
}

void CmdDiscordSelectLabel(const bp_api* _api, const bp_cmd_action_ctx* _ctx, void*) {
	if (!g_bot)
		return;
	const char* raw = nullptr;
	if (!_api->cmd.getStringArg(_ctx, "name", &raw) || !raw) {
		CommandReply(_api, _ctx, "Usage: /discord select <name>");
		return;
	}
	const std::string label = raw; // copy before anything else can invalidate it
	SelectLink(_api, _ctx, FindLinkByLabel(*g_bot, label));
}

void CmdDiscordSelectId(const bp_api* _api, const bp_cmd_action_ctx* _ctx, void*) {
	if (!g_bot)
		return;
	const char* raw = nullptr;
	if (!_api->cmd.getStringArg(_ctx, "channel_id", &raw) || !raw) {
		CommandReply(_api, _ctx, "Usage: /discord select id <channel id>");
		return;
	}
	const std::string id = raw;
	SelectLink(_api, _ctx, FindLinkByChannelId(*g_bot, id));
}

void RegisterDiscordCommand(const bp_api* _api) {
	auto& cmd = _api->cmd;

	bp_cmd_node* root = cmd.makeNode("discord", BP_CMD_LITERAL);
	cmd.setDescription(root, "Choose which Discord channel your chat is sent to");
	cmd.setAction(_api, root, CmdDiscordInfo, nullptr);

	bp_cmd_node* list = cmd.makeNode("list", BP_CMD_LITERAL);
	cmd.setDescription(list, "List the Discord channels you can talk through");
	cmd.setAction(_api, list, CmdDiscordList, nullptr);
	cmd.addChild(root, list);

	bp_cmd_node* select = cmd.makeNode("select", BP_CMD_LITERAL);
	cmd.setDescription(select, "Send your chat to another Discord channel");

	// "select id <channel id>" is operator-only. Added before the name argument so the literal
	// is tried first.
	bp_cmd_node* byId = cmd.makeNode("id", BP_CMD_LITERAL);
	cmd.requireOperator(byId);
	bp_cmd_node* idArg = cmd.makeNode("channel_id", BP_CMD_STRING);
	cmd.requireOperator(idArg);
	cmd.setDescription(idArg, "Select a channel by its Discord channel id");
	cmd.setAction(_api, idArg, CmdDiscordSelectId, nullptr);
	cmd.addChild(byId, idArg);
	cmd.addChild(select, byId);

	// The channel label may contain spaces, so it takes the rest of the line.
	bp_cmd_node* nameArg = cmd.makeNode("name", BP_CMD_GREEDY_STRING);
	cmd.setDescription(nameArg, "Select a channel by its label");
	cmd.setAction(_api, nameArg, CmdDiscordSelectLabel, nullptr);
	cmd.addChild(select, nameArg);

	cmd.addChild(root, select);
	cmd.registerCommand(_api, root);
}

// ---------------------------------------------------------------------------------
// Addon events (all invoked on the server thread)
// ---------------------------------------------------------------------------------

void OnAddonLoad(const bp_api* api) {
	g_api = api;
	StartBot();
	if (g_bot && !g_bot->links.empty())
		RegisterDiscordCommand(api);
}

void OnAddonUnload(const bp_api*) {
	StopBot();
	g_api = nullptr;
}

// Fired by the host when the server crashes. Not every crash can be reported (the process may
// be in a bad state), so this is best-effort and never throws.
void OnCrash(const bp_api*, const bp_crash_event* ev) {
	try {
		if (!g_bot || !ev || !g_bot->config.crashReports || g_bot->crashReported)
			return;
		Bot& bot = *g_bot;
		if (!bot.running.load() || bot.links.empty())
			return;
		bot.crashReported = true;

		SendCrashReport(bot, ev->version_string ? ev->version_string : "",
		                ev->crash_log_path ? ev->crash_log_path : "", ev->crash_log ? ev->crash_log : "");
	} catch (...) {
	}
}

void OnPlayerJoin(const bp_api* api, const bp_player_join_event* ev) {
	if (!g_bot)
		return;

	const char* rawName = api->player.getUsername(ev->player);
	const std::string name = rawName ? rawName : "Player";
	// Everyone starts out talking through the first channel in bpa_discord_channels.json.
	Link* defaultLink = g_bot->links.empty() ? nullptr : g_bot->links.front().get();
	size_t online;
	{
		std::lock_guard lock(g_bot->mutex);
		g_bot->players.push_back(PlayerEntry{ ev->player, name, g_bot->tick, defaultLink });
		online = g_bot->players.size();
	}
	MarkTopicsDirty(*g_bot);
	SendPlayerEvent(*g_bot, name, true, online);
}

void OnPlayerLeave(const bp_api* api, const bp_player_leave_event* ev) {
	if (!g_bot)
		return;

	std::string name;
	size_t online;
	{
		std::lock_guard lock(g_bot->mutex);
		auto it = std::find_if(g_bot->players.begin(), g_bot->players.end(),
		                       [&](const PlayerEntry& p) { return p.player == ev->player; });
		if (it != g_bot->players.end()) {
			name = it->name;
			g_bot->players.erase(it);
		}
		online = g_bot->players.size();
	}
	MarkTopicsDirty(*g_bot);
	if (name.empty()) {
		const char* rawName = api->player.getUsername(ev->player);
		name = rawName ? rawName : "Player";
	}
	SendPlayerEvent(*g_bot, name, false, online);
}

void OnPlayerChat(const bp_api* api, bp_player_chat_event* ev) {
	if (!g_bot || ev->cancel || !ev->message || ev->message[0] == '\0')
		return;
	// Commands are handled by the server after this event; never leak them to Discord.
	if (ev->message[0] == '/')
		return;

	// Chat goes to the channel this player picked with /discord select.
	Link* link = LinkOfPlayer(*g_bot, ev->player);
	if (!link)
		return;

	const char* name = api->player.getUsername(ev->player);
	SendPlayerChat(*g_bot, *link, name ? name : "Player", ev->message);
}

void OnServerTick(const bp_api* api, const bp_server_tick_event*) {
	if (!g_bot)
		return;
	Bot& bot = *g_bot;

	++bot.tick;
	FlushLogs(bot);

	// Refresh cached stats (host API is server-thread only; slash handlers read the cache).
	if (bot.tick % STATS_REFRESH_TICKS == 1) {
		bot.tps.store(api->server.getTpsAverage(api));
		bot.mspt.store(api->server.getMsptAverage(api));
	}

	RunPendingCommands(bot, api);

	if (!bot.startNoticeSent) {
		bot.startNoticeSent = true;
		SendNotice(bot, "Server started!", 0x55FF55);
		MarkTopicsDirty(bot); // publish the initial "online" topics
	}

	PumpTopicUpdates(bot);

	// Work out who should hear each queued Discord message. With discord-receive=selected only
	// players who currently talk through that channel do.
	struct Delivery {
		InboundMessage message;
		std::vector<bp_player*> recipients;
	};
	std::vector<Delivery> deliveries;
	{
		std::lock_guard lock(bot.mutex);
		while (!bot.inbound.empty()) {
			Delivery delivery;
			delivery.message = std::move(bot.inbound.front());
			bot.inbound.pop();

			for (const auto& p : bot.players) {
				if (bot.tick - p.joinTick < CHAT_READY_TICKS)
					continue;
				if (bot.config.receive == ReceiveMode::Selected && p.link != delivery.message.link)
					continue;
				delivery.recipients.push_back(p.player);
			}
			deliveries.push_back(std::move(delivery));
		}
	}

	for (const Delivery& delivery : deliveries) {
		if (delivery.recipients.empty() || !delivery.message.link)
			continue;
		const Link& link = *delivery.message.link;

		std::vector<std::string> lines;
		if (delivery.message.kind == InboundMessage::Kind::Reaction)
			lines.push_back(FormatReactionLine(delivery.message.author, delivery.message.content, link.prefix, link.postfix));
		else
			lines = FormatDiscordChatLines(delivery.message.author, delivery.message.content, link.prefix, link.postfix);

		for (const std::string& line : lines) {
			for (bp_player* player : delivery.recipients)
				api->player.sendMessage(player, line.c_str());
		}
	}
}

} // namespace

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#else
    __attribute__((visibility("default")))
#endif
        bp_addon_info bp_addon(const bp_api*) {
	bp_addon_info info{};
	info.id = ADDON_ID;
	info.name = ADDON_NAME;
	info.version = ADDON_VERSION;
	info.min_api_version = ADDON_API_VERSION,
	info.max_api_version = 0,
	info.events.playerJoin = OnPlayerJoin;
	info.events.playerLeave = OnPlayerLeave;
	info.events.playerChat = OnPlayerChat;
	info.events.serverTick = OnServerTick;
	info.events.crash = OnCrash;
	info.events.addonLoad = OnAddonLoad;
	info.events.addonUnload = OnAddonUnload;
	return info;
}