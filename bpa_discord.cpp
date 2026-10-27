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
 *   - Discord chat    -> Minecraft (messages in the configured channel)
 *   - Join/leave embeds (with the current player count), server start/stop notices
 *   - Optional: live player count in the channel topic, and "Server offline" on shutdown
 *   - Slash commands: /status, /list, /tps, /version, and (opt-in) /execute
 *   - Crash reports: the crash log is posted to the channel as a file attachment
 *
 * Configuration lives in "bpa_discord.properties" in the server's working directory
 * (a template is generated on first load). For migration convenience, any key missing
 * there is also looked up in "server.properties".
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
constexpr const char* ADDON_VERSION = "0.2.1";
constexpr const char* CONFIG_FILE = "bpa_discord.properties";
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

constexpr const char* DEFAULT_TOPIC_ONLINE = "Online | Players: {count}";
constexpr const char* DEFAULT_TOPIC_OFFLINE = "Offline";

enum class Level { Info, Warn, Error };

struct Config {
	std::string token;
	std::string channelId;
	std::string guildId;
	std::string webhookUrl;

	// Channel topic status. Off by default: it overwrites the channel's existing topic and
	// needs the bot to have the Manage Channels permission.
	bool topicEnabled = false;
	std::string topicOnline = DEFAULT_TOPIC_ONLINE; // {count} = players online
	std::string topicOffline = DEFAULT_TOPIC_OFFLINE;

	// Post the crash log to the channel when the server crashes.
	bool crashReports = true;

	// /execute is only registered when at least one role or user is allowed to use it.
	std::vector<dpp::snowflake> executeRoles;
	std::vector<dpp::snowflake> executeUsers;

	bool ExecuteEnabled() const { return !executeRoles.empty() || !executeUsers.empty(); }
};

struct InboundChat {
	std::string author;
	std::string content;
};

struct PlayerEntry {
	bp_player* player;
	std::string name;
	uint64_t joinTick;
};

// An /execute invocation waiting for the server thread (host API calls are server-thread only).
struct PendingCommand {
	dpp::slashcommand_t event;
	std::string command;
	std::string user;
};

struct Bot {
	Config config;
	std::unique_ptr<dpp::cluster> cluster;
	dpp::snowflake channel;
	dpp::webhook webhook; // base webhook; name/avatar are set on a copy per message
	bool hasWebhook = false;

	std::mutex mutex; // guards everything below
	std::queue<InboundChat> inbound;
	std::vector<PlayerEntry> players;
	std::queue<std::pair<Level, std::string>> logs;
	std::queue<PendingCommand> commands;

	std::atomic<bool> running{ false };

	// Cached host stats. Written on the server thread, read by slash-command handlers.
	std::string serverVersion; // set once before the bot starts
	std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
	std::atomic<double> tps{ -1.0 }; // < 0 = not sampled yet
	std::atomic<double> mspt{ -1.0 };

	// Server thread only.
	uint64_t tick = 0;
	bool startNoticeSent = false;
	bool crashReported = false;
	bool topicEverSent = false;
	std::chrono::steady_clock::time_point lastTopicSend{};

	// Touched from both the server thread and D++ callbacks.
	std::atomic<bool> topicDirty{ false };
	std::atomic<bool> topicInFlight{ false };
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

// Builds "§9[name] §f…" lines, each at most 119 bytes (prefix included).
std::vector<std::string> FormatDiscordChatLines(const std::string& _author, const std::string& _content) {
	static constexpr size_t MAX_LINE = 119;

	std::string author = RemoveMinecraftFormatting(_author);
	std::string content = RemoveMinecraftFormatting(_content);
	for (char& c : content) {
		if (c == '\n' || c == '\r' || c == '\t')
			c = ' ';
	}

	auto makePrefix = [](const std::string& name) {
		return "§9[" + name + "] §f";
	};

	std::string prefix = makePrefix(author);
	if (prefix.size() >= MAX_LINE) {
		const size_t overhead = makePrefix("").size();
		const size_t maxName = overhead < MAX_LINE ? MAX_LINE - overhead : 0;
		if (author.size() > maxName)
			author = author.substr(0, maxName);
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
	        "\n"
	        "# Bot token from the Discord Developer Portal (enable the Message Content Intent).\n"
	        "discord-token=\n"
	        "# Channel the bridge reads from and posts to.\n"
	        "discord-channel-id=\n"
	        "# Optional: register slash commands to one guild instantly. Leave empty for global\n"
	        "# (global registration can take up to an hour to appear).\n"
	        "discord-guild-id=\n"
	        "# Optional: a webhook URL for the SAME channel (Channel Settings -> Integrations -> Webhooks).\n"
	        "# When set, in-game chat is relayed under each player's own name + skin face\n"
	        "# instead of the bot's. Leave empty to relay chat as the bot.\n"
	        "discord-webhook-url=\n"
	        "\n"
	        "# Optional: show live server status in the channel topic. This OVERWRITES the topic and\n"
	        "# requires the bot to have the Manage Channels permission. Updates are rate-limited\n"
	        "# (at most one every 5 minutes), so the count may lag slightly behind.\n"
	        "discord-topic-status=false\n"
	        "# Topic while the server is running. {count} is replaced with the player count.\n"
	        "discord-topic-online=Online | Players: {count}\n"
	        "# Topic set when the server shuts down.\n"
	        "discord-topic-offline=Offline\n"
	        "\n"
	        "# Post the crash log to the channel (as a file) when the server crashes.\n"
	        "discord-crash-reports=true\n"
	        "\n"
	        "# Optional: let Discord moderators run server commands with /execute. The command is\n"
	        "# only registered when at least one of these is set (comma-separated Discord IDs).\n"
	        "# Only works in the bridged channel. A role match OR a user match is enough.\n"
	        "discord-execute-role-ids=\n"
	        "discord-execute-user-ids=\n";
}

Config LoadConfig() {
	auto own = ReadProperties(CONFIG_FILE);
	if (own.empty()) {
		if (!std::ifstream(CONFIG_FILE).good()) {
			WriteConfigTemplate();
			LogNow(Level::Warn, std::string("Generated ") + CONFIG_FILE + "; fill it in and restart the server.");
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

// ---------------------------------------------------------------------------------
// Discord output (safe to call from the server thread; callbacks log asynchronously)
// ---------------------------------------------------------------------------------

dpp::message MakeMessage(Bot& _bot, const std::string& _content) {
	dpp::message msg(_bot.channel, _content);
	// Player-controlled text must never be able to ping @everyone, roles or users.
	msg.set_allowed_mentions(false, false, false, false, {}, {});
	return msg;
}

void SendConfirmCallbackLog(Bot* _bot, const char* _what, const dpp::confirmation_callback_t& _result) {
	if (_result.is_error())
		LogAsync(*_bot, Level::Warn, std::string("Failed to send ") + _what + ": " + _result.get_error().message);
}

void SendPlayerChat(Bot& _bot, const std::string& _username, const std::string& _message) {
	if (!_bot.running.load() || !_bot.cluster)
		return;

	const std::string content = RemoveMinecraftFormatting(_message);
	if (content.empty())
		return;

	Bot* bot = &_bot;
	if (_bot.hasWebhook) {
		dpp::webhook wh = _bot.webhook;
		wh.name = SanitizeWebhookUsername(_username);
		wh.avatar_url = BuildSkinAvatarUrl(_username, 128);

		dpp::message msg(content);
		msg.set_allowed_mentions(false, false, false, false, {}, {});
		_bot.cluster->execute_webhook(wh, msg, false, 0, "", [bot](const dpp::confirmation_callback_t& result) {
			SendConfirmCallbackLog(bot, "chat via webhook", result);
		});
		return;
	}

	// No webhook configured: plain message under the bot's identity.
	_bot.cluster->message_create(
	    MakeMessage(_bot, std::format("[{}] {}", RemoveMinecraftFormatting(_username), content)),
	    [bot](const dpp::confirmation_callback_t& result) { SendConfirmCallbackLog(bot, "chat message", result); });
}

void SendPlayerEvent(Bot& _bot, const std::string& _rawUsername, bool _joined, size_t _onlineCount) {
	if (!_bot.running.load() || !_bot.cluster)
		return;

	const std::string username = RemoveMinecraftFormatting(_rawUsername);

	dpp::embed embed;
	embed.set_color(_joined ? 0x55FF55 : 0xFF5555)
	    .set_author(username, "", BuildSkinAvatarUrl(username, 256))
	    .set_description(std::format("**{}** {} the game", username, _joined ? "joined" : "left"))
	    .set_footer(dpp::embed_footer().set_text(std::format("{} player{} online", _onlineCount, _onlineCount == 1 ? "" : "s")));

	dpp::message msg(_bot.channel, embed);
	msg.set_allowed_mentions(false, false, false, false, {}, {});
	Bot* bot = &_bot;
	_bot.cluster->message_create(msg, [bot](const dpp::confirmation_callback_t& result) {
		SendConfirmCallbackLog(bot, "join/leave embed", result);
	});
}

void SendNotice(Bot& _bot, const std::string& _text, uint32_t _color) {
	if (!_bot.running.load() || !_bot.cluster)
		return;

	dpp::embed embed;
	embed.set_color(_color).set_description(_text);
	dpp::message msg(_bot.channel, embed);
	Bot* bot = &_bot;
	_bot.cluster->message_create(
	    msg, [bot](const dpp::confirmation_callback_t& result) { SendConfirmCallbackLog(bot, "notice", result); });
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

// Server thread only. Pushes the current player count to the channel topic when it changed
// and the rate-limit window allows. Several joins/leaves in a row collapse into one edit.
void PumpTopicUpdate(Bot& _bot) {
	if (!_bot.config.topicEnabled || !_bot.topicDirty.load() || !_bot.running.load() || !_bot.cluster)
		return;
	if (_bot.topicInFlight.load())
		return;

	const auto now = std::chrono::steady_clock::now();
	if (_bot.topicEverSent && now - _bot.lastTopicSend < TOPIC_MIN_INTERVAL)
		return;

	_bot.topicDirty.store(false);
	_bot.topicInFlight.store(true);
	_bot.topicEverSent = true;
	_bot.lastTopicSend = now;

	Bot* bot = &_bot;
	SetChannelTopic(*_bot.cluster, _bot.channel, FormatTopic(_bot.config.topicOnline, OnlineNames(_bot).size()),
	                [bot](bool ok, const std::string& error) {
		                if (!ok) {
			                LogAsync(*bot, Level::Warn,
			                         "Failed to update channel topic (does the bot have Manage Channels?): " + error);
			                bot->topicDirty.store(true); // retry on a later tick, still rate-limited
		                }
		                bot->topicInFlight.store(false);
	                });
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

void HandleExecuteCommand(Bot& _bot, const dpp::slashcommand_t& _event) {
	auto deny = [&](const std::string& _why) { _event.reply(dpp::message(_why).set_flags(dpp::m_ephemeral)); };
	const dpp::interaction& interaction = _event.command;
	const std::string who = std::format("{} ({})", interaction.get_issuing_user().username,
	                                    static_cast<uint64_t>(interaction.get_issuing_user().id));

	if (!_bot.config.ExecuteEnabled())
		return deny("`/execute` is not enabled.");
	if (interaction.channel_id != _bot.channel)
		return deny("`/execute` can only be used in the bridged channel.");
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
		const bool ok = _api->commands.execute(_api, nullptr, cmd.command.c_str());

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

// Cuts to at most _maxBytes without splitting a UTF-8 sequence.
std::string TruncateUtf8(const std::string& _s, size_t _maxBytes) {
	if (_s.size() <= _maxBytes)
		return _s;
	size_t end = _maxBytes;
	while (end > 0 && (static_cast<unsigned char>(_s[end]) & 0xC0) == 0x80)
		--end;
	return _s.substr(0, end);
}

// Runs inside the host's crash event. The process is about to die, so this posts the
// report (and the offline topic, if enabled) and then waits a bounded time for Discord.
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

	auto pending = std::make_shared<std::atomic<int>>(1);
	dpp::message msg(_bot.channel, embed);
	msg.set_allowed_mentions(false, false, false, false, {}, {});
	if (!_log.empty())
		msg.add_file("crash-log.txt", _log, "text/plain");

	Bot* bot = &_bot;
	_bot.cluster->message_create(msg, [bot, pending](const dpp::confirmation_callback_t& result) {
		SendConfirmCallbackLog(bot, "crash report", result);
		pending->fetch_sub(1);
	});

	if (_bot.config.topicEnabled) {
		pending->fetch_add(1);
		SetChannelTopic(*_bot.cluster, _bot.channel, _bot.config.topicOffline,
		                [bot, pending](bool ok, const std::string& error) {
			                if (!ok)
				                LogAsync(*bot, Level::Warn, "Couldn't set the offline topic after a crash: " + error);
			                pending->fetch_sub(1);
		                });
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

void StartBot() {
	auto bot = std::make_unique<Bot>();
	bot->config = LoadConfig();
	if (g_api && g_api->server.getVersion) {
		if (const char* version = g_api->server.getVersion())
			bot->serverVersion = version;
	}
	const Config& cfg = bot->config;

	if (cfg.token.empty() || cfg.channelId.empty()) {
		LogNow(Level::Warn, std::string("discord-token or discord-channel-id is missing in ") + CONFIG_FILE +
		                        "; the Discord bot will not start.");
		g_bot = std::move(bot); // keep it around so the (empty) registry still works
		return;
	}

	bot->channel = dpp::snowflake{ cfg.channelId };
	if (bot->channel == 0) {
		LogNow(Level::Error, "discord-channel-id is not a valid snowflake");
		g_bot = std::move(bot);
		return;
	}

	std::string guildId = cfg.guildId;
	if (!guildId.empty() && dpp::snowflake{ guildId } == 0) {
		LogNow(Level::Warn, "discord-guild-id is invalid; registering slash commands globally "
		                    "(can take up to an hour to appear).");
		guildId.clear();
	}

	if (!cfg.webhookUrl.empty()) {
		try {
			bot->webhook = dpp::webhook(cfg.webhookUrl);
			bot->hasWebhook = true;
		} catch (const std::exception& e) {
			LogNow(Level::Warn, std::string("discord-webhook-url is invalid (") + e.what() +
			                        "); player chat will be relayed as the bot instead.");
		}
	}

	Bot* raw = bot.get();
	g_bot = std::move(bot);

	try {
		const uint32_t intents = dpp::i_default_intents | dpp::i_message_content;
		auto cluster = std::make_unique<dpp::cluster>(cfg.token, intents);

		cluster->on_log([raw](const dpp::log_t& event) {
			// 10062 = Unknown interaction (acked too late / raced). Noise once we reply properly.
			if (event.message.find("10062") != std::string::npos)
				return;
			if (event.severity >= dpp::ll_error)
				LogAsync(*raw, Level::Error, event.message);
			else if (event.severity == dpp::ll_warning)
				LogAsync(*raw, Level::Warn, event.message);
		});

		cluster->on_ready([raw, guildId](const dpp::ready_t&) {
			if (!dpp::run_once<struct register_bot_commands>())
				return;
			if (!raw->cluster)
				return;

			const dpp::snowflake me = raw->cluster->me.id;
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

			if (!guildId.empty()) {
				raw->cluster->guild_bulk_command_create(commands, dpp::snowflake{ guildId });
				LogAsync(*raw, Level::Info, "Registered guild slash commands");
			} else {
				raw->cluster->global_bulk_command_create(commands);
				LogAsync(*raw, Level::Info, "Registered global slash commands (may take up to an hour)");
			}
		});

		cluster->on_message_create([raw](const dpp::message_create_t& event) {
			if (!raw->running.load())
				return;
			if (event.msg.channel_id != raw->channel)
				return;
			// Skips our own posts too, including webhook-relayed player chat.
			if (event.msg.author.is_bot() || event.msg.webhook_id != 0)
				return;
			if (event.msg.content.empty())
				return;

			InboundChat chat;
			const std::string nick = event.msg.member.get_nickname();
			chat.author = !nick.empty() ? nick : event.msg.author.username;
			chat.content = ResolveDiscordMentions(event.msg);

			std::lock_guard lock(raw->mutex);
			raw->inbound.push(std::move(chat));
		});

		cluster->on_slashcommand([raw](const dpp::slashcommand_t& event) {
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
				HandleExecuteCommand(*raw, event);
			else
				event.reply(dpp::message("Unknown command.").set_flags(dpp::m_ephemeral));
		});

		raw->cluster = std::move(cluster);
		raw->running.store(true);
		raw->cluster->start(dpp::st_return);
		LogNow(Level::Info, "Gateway bot started");

		// A webhook URL can silently point at the wrong channel or be stale. Check once
		// at startup so that's obvious immediately.
		if (raw->hasWebhook) {
			const dpp::snowflake webhookId = raw->webhook.id;
			const std::string webhookToken = raw->webhook.token;
			raw->cluster->get_webhook_with_token(
			    webhookId, webhookToken, [raw](const dpp::confirmation_callback_t& result) {
				    if (result.is_error()) {
					    LogAsync(*raw, Level::Error,
					             "discord-webhook-url could not be verified (" + result.get_error().message +
					                 "); it is likely deleted/regenerated. Player chat will fail to relay until "
					                 "it's replaced with a current webhook URL.");
					    return;
				    }
				    const dpp::webhook wh = result.get<dpp::webhook>();
				    if (wh.channel_id != raw->channel) {
					    LogAsync(*raw, Level::Warn,
					             std::format("discord-webhook-url posts to channel {}, but discord-channel-id is "
					                         "{}. Player chat will relay to a different channel than /status, /list "
					                         "and join/leave messages.",
					                         static_cast<uint64_t>(wh.channel_id),
					                         static_cast<uint64_t>(raw->channel)));
				    } else {
					    LogAsync(*raw, Level::Info,
					             std::format("Webhook verified, chat will relay to channel {}",
					                         static_cast<uint64_t>(wh.channel_id)));
				    }
			    });
		}
	} catch (const std::exception& e) {
		LogNow(Level::Error, std::string("Failed to start bot: ") + e.what());
		raw->running.store(false);
		raw->cluster.reset();
		raw->hasWebhook = false;
	}
}

void StopBot() {
	if (!g_bot)
		return;

	Bot& bot = *g_bot;
	bot.running.store(false);

	std::unique_ptr<dpp::cluster> cluster;
	{
		std::lock_guard lock(bot.mutex);
		cluster = std::move(bot.cluster);
		while (!bot.inbound.empty())
			bot.inbound.pop();
		while (!bot.commands.empty())
			bot.commands.pop();
	}

	if (!cluster) {
		FlushLogs(bot);
		g_bot.reset();
		return;
	}

	LogNow(Level::Info, "Shutting down...");

	const bool setTopic = bot.config.topicEnabled;
	const std::string offlineTopic = bot.config.topicOffline;
	Bot* botPtr = &bot;

	// Ownership moves into a worker so a stuck SSL/event-loop join can't freeze server exit.
	// DPP's cluster::shutdown() joins the engine thread; if that thread is blocked inside
	// OpenSSL (common when Discord HTTP is unhealthy) the join never returns.
	auto done = std::make_shared<std::atomic<bool>>(false);
	std::thread worker([cluster = std::move(cluster), channel = bot.channel, done, setTopic, offlineTopic,
	                    botPtr]() mutable {
		try {
			// Fire-and-forget goodbye; never wait on Discord here.
			cluster->message_create(dpp::message(channel, "Server stopped!"), [](const dpp::confirmation_callback_t&) {});

			if (setTopic) {
				// Bounded wait so the edit can go out before the gateway is torn down. If Discord
				// is rate-limiting us it won't make it, and the previous topic stays.
				auto topicDone = std::make_shared<std::atomic<bool>>(false);
				SetChannelTopic(*cluster, channel, offlineTopic, [botPtr, topicDone](bool ok, const std::string& error) {
					if (!ok)
						LogAsync(*botPtr, Level::Warn, "Couldn't set the offline topic: " + error);
					topicDone->store(true);
				});
				const auto topicDeadline = std::chrono::steady_clock::now() + TOPIC_SHUTDOWN_WAIT;
				while (!topicDone->load() && std::chrono::steady_clock::now() < topicDeadline)
					std::this_thread::sleep_for(std::chrono::milliseconds(25));
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(150));
			}
			cluster->shutdown();
			cluster.reset();
		} catch (...) {
		}
		done->store(true);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (!done->load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

	if (done->load()) {
		worker.join();
		FlushLogs(bot);
		LogNow(Level::Info, "Shut down");
		g_bot.reset();
	} else {
		LogNow(Level::Warn, "Shutdown timed out; abandoning Gateway threads so the server can exit");
		worker.detach();
		PinSelf();
		// Deliberately leak the Bot: the abandoned threads may still hold pointers to it.
		(void)g_bot.release();
	}
}

// ---------------------------------------------------------------------------------
// Addon events (all invoked on the server thread)
// ---------------------------------------------------------------------------------

void OnAddonLoad(const bp_api* api, const bp_addon_load*) {
	g_api = api;
	StartBot();
}

void OnAddonUnload(const bp_api*, const bp_addon_unload*) {
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
		if (!bot.running.load() || !bot.cluster)
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
	size_t online;
	{
		std::lock_guard lock(g_bot->mutex);
		g_bot->players.push_back(PlayerEntry{ ev->player, name, g_bot->tick });
		online = g_bot->players.size();
	}
	g_bot->topicDirty.store(true);
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
	g_bot->topicDirty.store(true);
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

	const char* name = api->player.getUsername(ev->player);
	SendPlayerChat(*g_bot, name ? name : "Player", ev->message);
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
		bot.topicDirty.store(true); // publish the initial "online" topic
	}

	PumpTopicUpdate(bot);

	std::queue<InboundChat> chats;
	std::vector<bp_player*> recipients;
	{
		std::lock_guard lock(bot.mutex);
		chats.swap(bot.inbound);
		if (!chats.empty()) {
			for (const auto& p : bot.players) {
				if (bot.tick - p.joinTick >= CHAT_READY_TICKS)
					recipients.push_back(p.player);
			}
		}
	}

	while (!chats.empty()) {
		const InboundChat chat = std::move(chats.front());
		chats.pop();
		for (const std::string& line : FormatDiscordChatLines(chat.author, chat.content)) {
			for (bp_player* player : recipients)
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
	info.events.playerJoin = OnPlayerJoin;
	info.events.playerLeave = OnPlayerLeave;
	info.events.playerChat = OnPlayerChat;
	info.events.serverTick = OnServerTick;
	info.events.crash = OnCrash;
	info.events.addonLoad = OnAddonLoad;
	info.events.addonUnload = OnAddonUnload;
	return info;
}