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
 *   - Join/leave embeds, server start/stop notices
 *   - Slash commands: /status, /list, /version
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
constexpr const char* ADDON_VERSION = "0.1.0";
constexpr const char* CONFIG_FILE = "bpa_discord.properties";
constexpr const char* SERVER_CONFIG_FILE = "server.properties";

// Players are only sent Discord chat once they've been registered for this many server
// ticks. The join event fires *before* the Login packet is sent, and a chat packet that
// arrives ahead of Login can confuse the client.
constexpr uint64_t CHAT_READY_TICKS = 40;

enum class Level { Info, Warn, Error };

struct Config {
	std::string token;
	std::string channelId;
	std::string guildId;
	std::string webhookUrl;
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

	std::atomic<bool> running{ false };

	// Server thread only.
	uint64_t tick = 0;
	bool startNoticeSent = false;
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
std::string BuildSkinAvatarUrl(const std::string& _username, int _size) {
	std::string safe;
	safe.reserve(_username.size());
	for (const char c : _username) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
			safe += c;
	}
	if (safe.empty())
		safe = "MHF_Steve";
	return "https://mc-heads.net/avatar/" + safe + "/" + std::to_string(_size);
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
	        "discord-webhook-url=\n";
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

	return Config{ get("discord-token"), get("discord-channel-id"), get("discord-guild-id"),
		           get("discord-webhook-url") };
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

void SendPlayerEvent(Bot& _bot, const std::string& _rawUsername, bool _joined) {
	if (!_bot.running.load() || !_bot.cluster)
		return;

	const std::string username = RemoveMinecraftFormatting(_rawUsername);

	dpp::embed embed;
	embed.set_color(_joined ? 0x55FF55 : 0xFF5555)
	    .set_author(username, "", BuildSkinAvatarUrl(username, 256))
	    .set_description(std::format("**{}** {} the game", username, _joined ? "joined" : "left"));

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
			const std::vector<dpp::slashcommand> commands{
				dpp::slashcommand("status", "Show Minecraft server status", me),
				dpp::slashcommand("list", "List online Minecraft players", me),
				dpp::slashcommand("version", "Show the Discord bridge version", me),
			};

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

			if (name == "status") {
				const size_t online = OnlineNames(*raw).size();
				event.reply(std::format("{} {} — {} player(s) online", ADDON_NAME, ADDON_VERSION, online));
			} else if (name == "list") {
				const auto names = OnlineNames(*raw);
				if (names.empty()) {
					event.reply("No players online.");
				} else {
					std::string joined;
					for (size_t i = 0; i < names.size(); ++i)
						joined += (i ? ", " : "") + names[i];
					event.reply(std::format("{} player(s): {}", names.size(), joined));
				}
			} else if (name == "version") {
				event.reply(std::format("{} {}", ADDON_NAME, ADDON_VERSION));
			} else {
				event.reply(dpp::message("Unknown command.").set_flags(dpp::m_ephemeral));
			}
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
	}

	if (!cluster) {
		FlushLogs(bot);
		g_bot.reset();
		return;
	}

	LogNow(Level::Info, "Shutting down...");

	// Ownership moves into a worker so a stuck SSL/event-loop join can't freeze server exit.
	// DPP's cluster::shutdown() joins the engine thread; if that thread is blocked inside
	// OpenSSL (common when Discord HTTP is unhealthy) the join never returns.
	auto done = std::make_shared<std::atomic<bool>>(false);
	std::thread worker([cluster = std::move(cluster), channel = bot.channel, done]() mutable {
		try {
			// Fire-and-forget goodbye; never wait on Discord here.
			cluster->message_create(dpp::message(channel, "Server stopped!"), [](const dpp::confirmation_callback_t&) {});
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			cluster->shutdown();
			cluster.reset();
		} catch (...) {
		}
		done->store(true);
	});

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
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

void OnPlayerJoin(const bp_api* api, const bp_player_join_event* ev) {
	if (!g_bot)
		return;

	const char* rawName = api->player.getUsername(ev->player);
	const std::string name = rawName ? rawName : "Player";
	{
		std::lock_guard lock(g_bot->mutex);
		g_bot->players.push_back(PlayerEntry{ ev->player, name, g_bot->tick });
	}
	SendPlayerEvent(*g_bot, name, true);
}

void OnPlayerLeave(const bp_api* api, const bp_player_leave_event* ev) {
	if (!g_bot)
		return;

	std::string name;
	{
		std::lock_guard lock(g_bot->mutex);
		auto it = std::find_if(g_bot->players.begin(), g_bot->players.end(),
		                       [&](const PlayerEntry& p) { return p.player == ev->player; });
		if (it != g_bot->players.end()) {
			name = it->name;
			g_bot->players.erase(it);
		}
	}
	if (name.empty()) {
		const char* rawName = api->player.getUsername(ev->player);
		name = rawName ? rawName : "Player";
	}
	SendPlayerEvent(*g_bot, name, false);
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

	if (!bot.startNoticeSent) {
		bot.startNoticeSent = true;
		SendNotice(bot, "Server started!", 0x55FF55);
	}

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
	info.events.addonLoad = OnAddonLoad;
	info.events.addonUnload = OnAddonUnload;
	return info;
}
