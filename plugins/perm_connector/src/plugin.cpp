// perm_connector — Plugify C++ plugin that links CS2 players (s2sdk) to users of the official
// "permissions" plugin (untrustedmodders/plugify-plugin-permissions 1.3.1).
//
// Flow per connection:
//   connected + authorised + non-bot slot
//     -> permissions::LoadUser(ourId, steamid64, playerName, offline=false, dontBroadcast=false)
//     -> the core broadcasts OnUserRequest to the storage plugin, which loads/creates the user
//     -> the core broadcasts OnUserLoaded (we log it and mark the slot loaded)
//
// What the core actually does (verified against ref/permissions/src/export/user.cpp):
//   * LoadUser() performs no storage work by itself; it only dispatches the OnUserRequest event
//     and - unless dontBroadcast is set - the OnUserLoaded event, synchronously, before it
//     returns. A connector that wants the "user is ready" notification therefore passes
//     dontBroadcast = false.
//   * pluginID is just the caller's plugin identifier (plg::Plugin::GetId()); the core forwards
//     it to the storage callbacks and to the OnUserLoaded event.
//   * There is no unload / offline-transition API: a User stays in the core registry until
//     DeleteUser removes it. Following the storage/connector contract (ref/permissions-contract.md
//     §8) the CONNECTOR owns the disconnect side, so when a tracked player leaves the user we
//     loaded is removed with DeleteUser. That call only touches the core: it broadcasts
//     OnUserDelete (a notification; the storage is not consulted) and the storage keeps its rows.
//     A rejoin issues a fresh LoadUser.
//   * Groups exist only if someone calls LoadGroups - the core never does that by itself and a
//     storage plugin can only react to the resulting OnGroupsLoad event - so this connector
//     triggers the server group load too (see EnsureGroupsLoaded): users cannot be created with
//     their groups before the groups are in the core (CreateUser -> GroupNotFound).
//
// Everything in this plugin runs on the game thread (plugify update pump, s2sdk events, console
// commands), so the slot table needs no locking. No s2sdk client query is made from
// OnPluginStart: before a map is loaded those calls crash, so client work only happens from
// OnPluginUpdate (pumped from ServerPostSimulate) and from game events.

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <plg/formatter.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

namespace {

// CS2 has 64 player slots.
constexpr int32_t kMaxSlots = 64;
constexpr auto kReconcileInterval = std::chrono::seconds(1);
// A load request that has not produced a user after this long is reported once per connection.
constexpr auto kStuckAfter = std::chrono::seconds(10);
// Group bootstrap: the core loads nothing by itself and LoadGroups reports Success even when no
// storage handler is registered yet, so "did it work" is judged by the groups present afterwards.
constexpr int kMaxGroupLoadAttempts = 6;
constexpr auto kGroupsRetryInterval = std::chrono::seconds(10);

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogDebug(std::string_view message) {
	LogMessage(message, plg::Severity::Debug);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// Renders a plg::any (a permissions cookie value) through the SDK's formatter.
std::string FormatValue(const plg::any& value) {
	try {
		return std::format("{}", value);
	} catch (...) {
		return "<unprintable>";
	}
}

const char* StatusName(permissions::Status status) {
	switch (status) {
	case permissions::Status::Success: return "Success";
	case permissions::Status::TargetUserNotFound: return "TargetUserNotFound";
	case permissions::Status::UserAlreadyExist: return "UserAlreadyExist";
	case permissions::Status::CookieNotFound: return "CookieNotFound";
	case permissions::Status::InvalidCookieName: return "InvalidCookieName";
	case permissions::Status::DBNotReady: return "DBNotReady";
	case permissions::Status::StorageError: return "StorageError";
	case permissions::Status::CallbackInvalid: return "CallbackInvalid";
	case permissions::Status::CallbackAlreadyExist: return "CallbackAlreadyExist";
	case permissions::Status::CallbackNotFound: return "CallbackNotFound";
	default: return "?";
	}
}

std::string StatusText(permissions::Status status) {
	const char* name = StatusName(status);
	if (std::string_view(name) != "?") {
		return std::string(name);
	}
	return "status " + std::to_string(static_cast<int32_t>(status));
}

const char* PlayerStateName(permissions::PlayerState state) {
	switch (state) {
	case permissions::PlayerState::NotFound: return "NotFound";
	case permissions::PlayerState::Online: return "Online";
	case permissions::PlayerState::Offline: return "Offline";
	default: return "?";
	}
}

bool ParseSlot(std::string_view text, int32_t& out) {
	if (text.empty()) {
		return false;
	}
	int32_t value = 0;
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (ec != std::errc() || end != text.data() + text.size()) {
		return false;
	}
	out = value;
	return true;
}

std::string Quoted(std::string_view value) {
	return "\"" + std::string(value) + "\"";
}

class PermConnectorPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// s2sdk map lifecycle listeners (game thread): only state, no client/entity query.
	void HandleMapStart();
	void HandleMapEnd();

	// s2sdk listener bodies (game thread, invoked by the engine through s2sdk).
	void HandleClientAuthenticated(int32_t playerSlot, uint64_t eventSteamId);
	void HandleClientDisconnect(int32_t playerSlot);
	// permissions OnUserLoaded listener body (called synchronously from LoadUser).
	void HandleUserLoaded(uint64_t targetId, permissions::PlayerState playerState);

	// Console commands.
	s2sdk::ResultType HandleStatusCommand(int32_t caller, s2sdk::ConCommandContext context,
										  const plg::vector<plg::string>& args);
	s2sdk::ResultType HandleCookieCommand(int32_t caller, s2sdk::ConCommandContext context,
										  const plg::vector<plg::string>& args);

private:
	// What we track for one player slot. "requested" means a LoadUser was issued for the
	// steamid that currently occupies the slot.
	struct SlotState {
		bool requested = false;
		bool loaded = false;
		bool stuckLogged = false;
		int failures = 0;
		uint64_t steamid = 0;
		std::string name;
		std::chrono::steady_clock::time_point requestedAt{};
	};

	bool BindingsAvailable(std::string& error) const;
	// Once per second: requests LoadUser for every connected, authorised, non-bot slot without a
	// pending request (this is also what picks up players that were already on the server when
	// the plugin started) and clears slots whose client is gone.
	void ReconcileSlots();
	// Triggers LoadGroups until the core actually holds groups (bounded retries, see constants).
	void EnsureGroupsLoaded();
	void RequestLoad(int32_t playerSlot, uint64_t steamid64);
	void ClearSlot(int32_t playerSlot, std::string_view reason);
	// SteamID64 of the player in the slot: live s2sdk query when possible, else the tracked one.
	uint64_t ResolveSlotSteamId(int32_t playerSlot) const;
	int64_t RequesterId() const { return static_cast<int64_t>(GetId()); }
	void Reply(s2sdk::ConCommandContext context, int32_t caller, std::string_view text) const;

	std::array<SlotState, kMaxSlots> m_slots{};
	bool m_s2sdkListenersRegistered = false;
	bool m_userLoadedRegistered = false;
	bool m_statusCommandRegistered = false;
	bool m_cookieCommandRegistered = false;
	// Map lifecycle gate (same state machine as pisexcompat): true only while a map is really
	// running. Between the map end and the next map start s2sdk's client list is torn down and
	// every client query - IsClientConnected included - segfaults (live crash in cs2ac); the
	// reconcile, the auth event, the commands and the steamid lookup all check this first.
	bool m_mapReady = false;
	bool m_mapStartSeen = false; // an OnMapStart arrived since the last OnMapEnd
	bool m_mapEndSeen = false;   // an OnMapEnd arrived since the last OnMapStart
	// Group bootstrap state (see EnsureGroupsLoaded).
	bool m_groupsLoaded = false;
	int m_groupsAttempts = 0;
	std::chrono::steady_clock::time_point m_nextGroupsAttempt{};
	// Reconcile is armed at plugin start and pumped from OnPluginUpdate, which the host calls
	// from ServerPostSimulate - i.e. only once a map is running and s2sdk client queries are
	// safe. The first reconcile therefore runs on the first update with a live map.
	bool m_reconcileEnabled = false;
	std::chrono::milliseconds m_sinceReconcile{0};
};

PermConnectorPlugin g_permConnectorPlugin;

// --- listener trampolines -----------------------------------------------------
// Nothing may throw into the plugify module, s2sdk or the permissions core.

void OnClientAuthenticatedListener(int32_t playerSlot, uint64_t steamid) {
	try {
		g_permConnectorPlugin.HandleClientAuthenticated(playerSlot, steamid);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: exception in OnClientAuthenticated: ") + ex.what());
	} catch (...) {
		LogError("perm_connector: unknown exception in OnClientAuthenticated");
	}
}

// Map lifecycle: these run on the game thread through s2sdk. Only local state is touched - a
// client or entity query between the map end and the next map start crashes inside s2sdk.
void OnMapStartListener() {
	try {
		g_permConnectorPlugin.HandleMapStart();
	} catch (...) {
	}
}

void OnMapEndListener() {
	try {
		g_permConnectorPlugin.HandleMapEnd();
	} catch (...) {
	}
}

void OnClientDisconnectListener(int32_t playerSlot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	try {
		g_permConnectorPlugin.HandleClientDisconnect(playerSlot);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: exception in OnClientDisconnect_Post: ") + ex.what());
	} catch (...) {
		LogError("perm_connector: unknown exception in OnClientDisconnect_Post");
	}
}

void OnUserLoadedListener(int64_t pluginID, uint64_t targetId, permissions::PlayerState playerState) {
	(void)pluginID;
	try {
		g_permConnectorPlugin.HandleUserLoaded(targetId, playerState);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: exception in OnUserLoaded: ") + ex.what());
	} catch (...) {
		LogError("perm_connector: unknown exception in OnUserLoaded");
	}
}

s2sdk::ResultType OnPermStatusCommand(int32_t caller, s2sdk::ConCommandContext context,
									  const plg::vector<plg::string>& args) {
	try {
		return g_permConnectorPlugin.HandleStatusCommand(caller, context, args);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: exception in 'perm_status': ") + ex.what());
	} catch (...) {
		LogError("perm_connector: unknown exception in 'perm_status'");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnPermCookieCommand(int32_t caller, s2sdk::ConCommandContext context,
									  const plg::vector<plg::string>& args) {
	try {
		return g_permConnectorPlugin.HandleCookieCommand(caller, context, args);
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: exception in 'perm_cookie': ") + ex.what());
	} catch (...) {
		LogError("perm_connector: unknown exception in 'perm_cookie'");
	}
	return s2sdk::ResultType::Handled;
}

// --- plugin -------------------------------------------------------------------

bool PermConnectorPlugin::BindingsAvailable(std::string& error) const {
	// The module fills the __permissions_* / __s2sdk_* slots from the plugins declared as our
	// dependencies; without them nothing here can work. These are the only two failure cases
	// that may take the plugin down (everything else fails soft).
	if (!__permissions_LoadUser || !__permissions_UserExists || !__permissions_GetUserGroups ||
		!__permissions_GetImmunity || !__permissions_GetAllCookies || !__permissions_GetCookie ||
		!__permissions_SetCookie || !__permissions_OnUserLoaded_Register ||
		!__permissions_OnUserLoaded_Unregister || !__permissions_LoadGroups || !__permissions_GetAllGroups ||
		!__permissions_DeleteUser) {
		error = "perm_connector: permissions bindings are not available (is the permissions plugin >= 1.3.1 installed?)";
		return false;
	}
	if (!__s2sdk_IsClientConnected || !__s2sdk_IsClientAuthorized || !__s2sdk_IsFakeClient ||
		!__s2sdk_GetClientSteamID64 || !__s2sdk_GetClientName || !__s2sdk_AddConsoleCommand ||
		!__s2sdk_RemoveCommand || !__s2sdk_ReplyToCommand || !__s2sdk_OnClientAuthenticated_Register ||
		!__s2sdk_OnClientDisconnect_Post_Register || !__s2sdk_OnMapStart_Register ||
		!__s2sdk_OnMapStart_Unregister || !__s2sdk_OnMapEnd_Register || !__s2sdk_OnMapEnd_Unregister) {
		error = "perm_connector: s2sdk bindings are not available (is the s2sdk plugin loaded?)";
		return false;
	}
	return true;
}

plg::PluginResult PermConnectorPlugin::OnPluginStart() {
	try {
		std::string error;
		if (!BindingsAvailable(error)) {
			LogError(error);
			return plg::PluginResult(error, plg::PluginCode::Failed);
		}

		// No client queries here - before a map is loaded they crash inside s2sdk. The
		// reconcile pass covers the players that are already on the server (late load).
		s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
		s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectListener);
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		m_s2sdkListenersRegistered = true;

		const permissions::Status registerStatus = permissions::OnUserLoaded_Register(&OnUserLoadedListener);
		if (registerStatus == permissions::Status::Success) {
			m_userLoadedRegistered = true;
		} else {
			LogWarn("perm_connector: registering the OnUserLoaded listener failed (" + StatusText(registerStatus) +
					") - loads are still requested, only the 'loaded' log line is missing");
		}

		// AddConsoleCommand attaches the listener even when the valve-command creation is refused
		// because the name already exists (s2sdk cannot destroy a console command, so our own
		// entry from a previous `plg reload` keeps its name); the registration is ours to remove
		// in every case, so the flags are set whenever the call was made.
		const bool statusAdded = s2sdk::AddConsoleCommand("perm_status",
														  "Prints the permissions state of every connected human player",
														  s2sdk::ConVarFlag::None, &OnPermStatusCommand, s2sdk::HookMode::Pre);
		m_statusCommandRegistered = true;
		if (!statusAdded) {
			LogInfo("perm_connector: 'perm_status' already exists (a previous load); reusing the entry");
		}

		const bool cookieAdded = s2sdk::AddConsoleCommand("perm_cookie",
														  "Gets or sets a permissions cookie: perm_cookie <slot> <name> [value]",
														  s2sdk::ConVarFlag::None, &OnPermCookieCommand, s2sdk::HookMode::Pre);
		m_cookieCommandRegistered = true;
		if (!cookieAdded) {
			LogInfo("perm_connector: 'perm_cookie' already exists (a previous load); reusing the entry");
		}

		m_reconcileEnabled = true;
		m_sinceReconcile = kReconcileInterval; // first reconcile on the first update
		m_nextGroupsAttempt = std::chrono::steady_clock::now(); // first LoadGroups on the first update

		LogInfo("perm_connector: ready (plugin id " + std::to_string(RequesterId()) +
				"); 'perm_status' and 'perm_cookie' registered");
		return {};
	} catch (const std::exception& ex) {
		// Fail soft: Only genuinely missing bindings take the plugin down (checked above).
		LogError(std::string("perm_connector: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("perm_connector: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult PermConnectorPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	// Map lifecycle gate, the same state machine as pisexcompat: the first update proves a live
	// map (a plugin loaded mid-map never saw an OnMapStart), but the host can keep pumping
	// updates throughout a changelevel, so after an OnMapEnd only a new OnMapStart re-opens it.
	if (!m_mapReady && (m_mapStartSeen || !m_mapEndSeen)) {
		m_mapReady = true;
	}
	if (!m_reconcileEnabled) {
		return {};
	}
	m_sinceReconcile += dt;
	if (m_sinceReconcile < kReconcileInterval) {
		return {};
	}
	m_sinceReconcile = std::chrono::milliseconds(0);
	try {
		EnsureGroupsLoaded(); // groups must exist before users can be created with them
		ReconcileSlots();
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: reconcile failed: ") + ex.what());
	} catch (...) {
		LogError("perm_connector: reconcile failed with an unknown exception");
	}
	return {};
}

plg::PluginResult PermConnectorPlugin::OnPluginEnd() {
	try {
		// Drop the core listener first: it points into this library, which is about to go away.
		if (m_userLoadedRegistered) {
			m_userLoadedRegistered = false;
			if (__permissions_OnUserLoaded_Unregister) {
				permissions::OnUserLoaded_Unregister(&OnUserLoadedListener);
			}
		}
		if (m_s2sdkListenersRegistered) {
			m_s2sdkListenersRegistered = false;
			if (__s2sdk_OnClientAuthenticated_Unregister) {
				s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
			}
			if (__s2sdk_OnClientDisconnect_Post_Unregister) {
				s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectListener);
			}
			if (__s2sdk_OnMapStart_Unregister) {
				s2sdk::OnMapStart_Unregister(&OnMapStartListener);
			}
			if (__s2sdk_OnMapEnd_Unregister) {
				s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
			}
		}
		if (m_statusCommandRegistered) {
			m_statusCommandRegistered = false;
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand("perm_status", &OnPermStatusCommand);
			}
		}
		if (m_cookieCommandRegistered) {
			m_cookieCommandRegistered = false;
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand("perm_cookie", &OnPermCookieCommand);
			}
		}
		// Users stay loaded on plugin end: stopping/reloading this plugin is not a disconnect, and
		// deleting every user here would evict all players and force a full reload on rejoin.
	} catch (const std::exception& ex) {
		LogError(std::string("perm_connector: OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("perm_connector: OnPluginEnd failed with an unknown exception");
	}
	return {};
}

void PermConnectorPlugin::HandleMapStart() {
	// State only (see OnMapStartListener): the next reconcile picks up whoever is on the server.
	m_mapReady = true;
	m_mapStartSeen = true;
	m_mapEndSeen = false;
}

void PermConnectorPlugin::HandleMapEnd() {
	// From here until the next map start every s2sdk client query crashes (the client list is
	// torn down); only state changes may happen. The tracked slots stay as they are: a player who
	// is still connected through a changelevel must not be dropped from the permissions core and
	// reloaded for nothing, and the disconnect event (which fires for the clients that really
	// leave) clears them.
	m_mapReady = false;
	m_mapStartSeen = false;
	m_mapEndSeen = true;
}

void PermConnectorPlugin::EnsureGroupsLoaded() {
	if (m_groupsLoaded) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < m_nextGroupsAttempt || m_groupsAttempts >= kMaxGroupLoadAttempts) {
		return;
	}
	m_nextGroupsAttempt = now + kGroupsRetryInterval;
	const bool first = m_groupsAttempts == 0;
	++m_groupsAttempts;

	// The core never calls LoadGroups by itself and a storage plugin can only react to the
	// OnGroupsLoad event, so triggering the server group load is the caller's job (contract §8).
	// LoadGroups returns Success even when no storage handler is registered yet, so the result is
	// judged by the groups present in the core afterwards - which also covers the load order
	// (perm_storage and perm_connector only share the permissions dependency, so either may start
	// first). When the storage vetoes (DBNotReady: database not ready yet), the next attempt is
	// made after kGroupsRetryInterval.
	const permissions::Status status = permissions::LoadGroups(RequesterId(), /*dontBroadcast=*/false);
	const std::size_t groupCount = permissions::GetAllGroups().size();

	if (groupCount != 0) {
		m_groupsLoaded = true;
		LogInfo("perm_connector: " + std::to_string(groupCount) + " server group" +
				(groupCount == 1 ? "" : "s") + " present in the permissions core");
		return;
	}

	if (first) {
		LogInfo("perm_connector: requested the server groups (LoadGroups) - waiting for the permissions storage");
	}
	if (m_groupsAttempts >= kMaxGroupLoadAttempts) {
		LogWarn("perm_connector: no server groups appeared in the permissions core after " +
				std::to_string(kMaxGroupLoadAttempts) + " LoadGroups attempts (last status " + StatusText(status) +
				") - continuing without them; is a permissions storage plugin installed and its database reachable?");
	}
}

void PermConnectorPlugin::ReconcileSlots() {
	if (!m_mapReady) {
		return; // map-change window: IsClientConnected itself would crash inside s2sdk
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			const bool connected = s2sdk::IsClientConnected(slot);
			if (!connected) {
				if (m_slots[slot].steamid != 0) {
					ClearSlot(slot, "client no longer connected (missed disconnect event)");
				}
				continue;
			}
			if (!s2sdk::IsClientAuthorized(slot) || s2sdk::IsFakeClient(slot)) {
				continue; // still connecting, or a bot without a persistent identity
			}
			const uint64_t steamid64 = s2sdk::GetClientSteamID64(slot);
			if (steamid64 == 0) {
				continue;
			}
			RequestLoad(slot, steamid64);
		} catch (const std::exception& ex) {
			LogError("perm_connector: reconcile of slot " + std::to_string(slot) + " failed: " + ex.what());
		} catch (...) {
			LogError("perm_connector: reconcile of slot " + std::to_string(slot) + " failed with an unknown exception");
		}
	}
}

void PermConnectorPlugin::RequestLoad(int32_t playerSlot, uint64_t steamid64) {
	SlotState& slot = m_slots[playerSlot];

	if (slot.steamid != 0 && slot.steamid != steamid64) {
		// The slot changed hands without a disconnect event (rare; also covers a request that
		// never completed): start over for the newcomer.
		LogInfo("perm_connector: slot " + std::to_string(playerSlot) + " now holds user " + std::to_string(steamid64) +
				" (was " + std::to_string(slot.steamid) + ") - treating it as a new connection");
		slot = SlotState{};
	}

	if (slot.requested && slot.steamid == steamid64) {
		// Already requested for this connection. If the user never showed up, say so once.
		if (!slot.loaded && !slot.stuckLogged && std::chrono::steady_clock::now() - slot.requestedAt >= kStuckAfter) {
			slot.stuckLogged = true;
			LogWarn("perm_connector: user " + std::to_string(steamid64) + " (slot " + std::to_string(playerSlot) +
					") was requested " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kStuckAfter).count()) +
					"s ago and is still not loaded - is a permissions storage plugin running?");
		}
		return;
	}

	const bool firstAttempt = slot.failures == 0;
	const std::string name = ToString(s2sdk::GetClientName(playerSlot));

	slot.requested = true;
	slot.loaded = false;
	slot.stuckLogged = false;
	slot.steamid = steamid64;
	slot.requestedAt = std::chrono::steady_clock::now();
	slot.name = name;

	if (firstAttempt) {
		LogInfo("perm_connector: requesting load for user " + std::to_string(steamid64) + " (slot " +
				std::to_string(playerSlot) + ", name " + Quoted(name) + ")");
	} else {
		LogDebug("perm_connector: retrying load for user " + std::to_string(steamid64) + " (slot " +
				 std::to_string(playerSlot) + ", attempt " + std::to_string(slot.failures + 1) + ")");
	}

	// offline=false: the player is on the server. dontBroadcast=false: let the core dispatch
	// OnUserLoaded (it does so synchronously inside LoadUser) so listeners - including us - see
	// the completion.
	const permissions::Status status = permissions::LoadUser(RequesterId(), steamid64, plg::string(name),
															/*offline=*/false, /*dontBroadcast=*/false);
	if (status == permissions::Status::Success) {
		slot.failures = 0;
		return;
	}

	// The storage plugin refused the request (typically DBNotReady while it is not ready yet).
	// Fail soft: keep the slot, retry on a later reconcile, and log only from time to time so a
	// missing/broken storage plugin cannot flood the log.
	slot.requested = false;
	slot.loaded = false;
	++slot.failures;
	if (slot.failures == 1 || slot.failures % 30 == 0) {
		LogWarn("perm_connector: LoadUser for user " + std::to_string(steamid64) + " (slot " +
				std::to_string(playerSlot) + ") returned " + StatusText(status) + " - will retry");
	}
}

void PermConnectorPlugin::ClearSlot(int32_t playerSlot, std::string_view reason) {
	SlotState& slot = m_slots[playerSlot];
	const uint64_t steamid64 = slot.steamid;
	if (steamid64 == 0) {
		return;
	}
	slot = SlotState{};

	const std::string where = "slot " + std::to_string(playerSlot) + " (user " + std::to_string(steamid64) +
							  ", " + std::string(reason) + ")";

	// The core has no unload API: the User stays in its registry until DeleteUser. Per the
	// storage/connector contract the connector owns the disconnect side, so the user we loaded is
	// removed here. DeleteUser only touches the core - it broadcasts OnUserDelete (a notification,
	// no storage callback, no veto) - the storage keeps its rows, and a rejoin loads the user
	// again. Users that are merely loaded "offline" by someone else are left alone.
	const permissions::PlayerState state = permissions::UserExists(steamid64);
	if (state == permissions::PlayerState::Online) {
		const permissions::Status status = permissions::DeleteUser(RequesterId(), steamid64);
		if (status == permissions::Status::Success) {
			LogInfo("perm_connector: released " + where + " - user removed from the permissions core");
		} else {
			LogWarn("perm_connector: released " + where + " - DeleteUser returned " + StatusText(status) +
					"; the user stays in the permissions core");
		}
	} else if (state == permissions::PlayerState::Offline) {
		LogInfo("perm_connector: released " + where + " - the offline user load is left in place");
	} else {
		LogInfo("perm_connector: released " + where + " - no such user in the permissions core");
	}
}

void PermConnectorPlugin::HandleClientAuthenticated(int32_t playerSlot, uint64_t eventSteamId) {
	if (playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	// Events keep arriving during a changelevel; in that window the client lookups below would
	// crash. The reconcile pass requests the load on the next map instead.
	if (!m_mapReady) {
		return;
	}
	// On a live server this event is driven solely by Steam's ValidateAuthTicketResponse and was
	// observed not to fire for real players - the reconcile pass is what actually gets players
	// loaded. When it does fire, the client is not necessarily fully connected yet, so the same
	// guards as in the reconcile apply; anything that fails them is picked up there later.
	if (!s2sdk::IsClientConnected(playerSlot) || !s2sdk::IsClientAuthorized(playerSlot) ||
		s2sdk::IsFakeClient(playerSlot)) {
		return;
	}

	uint64_t steamid64 = eventSteamId;
	const uint64_t canonical = s2sdk::GetClientSteamID64(playerSlot);
	if (canonical != 0) {
		if (steamid64 != 0 && steamid64 != canonical) {
			LogWarn("perm_connector: OnClientAuthenticated reported " + std::to_string(steamid64) + " for slot " +
					std::to_string(playerSlot) + ", but the client's SteamID64 is " + std::to_string(canonical) +
					" - using the latter");
		}
		steamid64 = canonical;
	} else if (steamid64 < (1ull << 32)) {
		// The event documents "Steam account ID or 0 if not available"; a 32-bit account id is
		// not a SteamID64 and must not be used as a permissions user key. Wait for the reconcile.
		return;
	}
	if (steamid64 == 0) {
		return;
	}
	RequestLoad(playerSlot, steamid64);
}

void PermConnectorPlugin::HandleClientDisconnect(int32_t playerSlot) {
	if (playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	ClearSlot(playerSlot, "disconnect");
}

void PermConnectorPlugin::HandleUserLoaded(uint64_t targetId, permissions::PlayerState playerState) {
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		SlotState& state = m_slots[slot];
		if (state.requested && state.steamid == targetId) {
			state.loaded = true;
			state.failures = 0;
			LogInfo("perm_connector: user " + std::to_string(targetId) + " loaded (slot " + std::to_string(slot) + ")");
			if (playerState != permissions::PlayerState::Online) {
				LogWarn("perm_connector: user " + std::to_string(targetId) + " (slot " + std::to_string(slot) +
						") was loaded as " + PlayerStateName(playerState) + " although the player is connected");
			}
			return;
		}
	}
	// No pending request in any slot: someone else loaded this user (e.g. a storage plugin
	// pre-loading offline users). Nothing for the connector to do.
}

uint64_t PermConnectorPlugin::ResolveSlotSteamId(int32_t playerSlot) const {
	if (playerSlot < 0 || playerSlot >= kMaxSlots) {
		return 0;
	}
	// The map-change window has no client list (a query would crash): the tracked id is the best
	// answer available.
	if (!m_mapReady) {
		return m_slots[playerSlot].steamid;
	}
	try {
		if (s2sdk::IsClientConnected(playerSlot) && s2sdk::IsClientAuthorized(playerSlot) &&
			!s2sdk::IsFakeClient(playerSlot)) {
			const uint64_t steamid64 = s2sdk::GetClientSteamID64(playerSlot);
			if (steamid64 != 0) {
				return steamid64;
			}
		}
	} catch (...) {
		// fall through to the tracked id
	}
	return m_slots[playerSlot].steamid;
}

void PermConnectorPlugin::Reply(s2sdk::ConCommandContext context, int32_t caller, std::string_view text) const {
	s2sdk::ReplyToCommand(context, caller, plg::string(text));
}

s2sdk::ResultType PermConnectorPlugin::HandleStatusCommand(int32_t caller, s2sdk::ConCommandContext context,
														   const plg::vector<plg::string>& args) {
	// Depending on the host, the command name may or may not be part of the arguments.
	std::size_t first = 0;
	if (!args.empty() && std::string_view(args[0]) == "perm_status") {
		first = 1;
	}
	if (args.size() > first) {
		Reply(context, caller, "usage: perm_status");
		return s2sdk::ResultType::Handled;
	}

	// An rcon perm_status can land during a changelevel, when every s2sdk client query (the
	// IsClientConnected probe below included) crashes inside the torn-down client list.
	if (!m_mapReady) {
		Reply(context, caller, "perm_connector: no map is running (the client list is unavailable during a "
							   "changelevel); try again once the next map is up");
		return s2sdk::ResultType::Handled;
	}

	Reply(context, caller, "perm_connector: --- perm_status begin ---");
	int humans = 0;
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!s2sdk::IsClientConnected(slot)) {
				continue;
			}
			if (!s2sdk::IsClientAuthorized(slot) || s2sdk::IsFakeClient(slot)) {
				continue;
			}
			++humans;

			const uint64_t steamid64 = s2sdk::GetClientSteamID64(slot);
			const std::string name = ToString(s2sdk::GetClientName(slot));
			const SlotState& state = m_slots[slot];
			const bool requested = state.requested && state.steamid == steamid64;

			std::string line = "perm_connector: slot " + std::to_string(slot) + ": name " + Quoted(name) +
							   " steamid " + std::to_string(steamid64) +
							   " requested=" + (requested ? "yes" : "no");

			const permissions::PlayerState loadedState = permissions::UserExists(steamid64);
			line += " loaded=" + std::string(PlayerStateName(loadedState));

			// These four calls return TargetUserNotFound while the user is not loaded; "-" then
			// simply shows that the permissions core does not know the player (yet).
			plg::vector<plg::string> groups;
			std::string groupsText = "-";
			if (permissions::GetUserGroups(steamid64, groups) == permissions::Status::Success) {
				groupsText = "[";
				for (std::size_t i = 0; i < groups.size(); ++i) {
					if (i != 0) {
						groupsText += ", ";
					}
					groupsText += ToString(groups[i]);
				}
				groupsText += "]";
			}
			line += " groups=" + groupsText;

			int32_t immunity = 0;
			std::string immunityText = "-";
			if (permissions::GetImmunity(steamid64, immunity) == permissions::Status::Success) {
				immunityText = std::to_string(immunity);
			}
			line += " immunity=" + immunityText;

			plg::vector<plg::string> names;
			plg::vector<plg::any> values;
			std::string cookiesText = "-";
			if (permissions::GetAllCookies(steamid64, names, values) == permissions::Status::Success) {
				cookiesText = "{";
				const std::size_t count = names.size() < values.size() ? names.size() : values.size();
				for (std::size_t i = 0; i < count; ++i) {
					if (i != 0) {
						cookiesText += ", ";
					}
					cookiesText += ToString(names[i]) + "=" + FormatValue(values[i]);
				}
				cookiesText += "}";
			}
			line += " cookies=" + cookiesText;

			Reply(context, caller, line);
		} catch (const std::exception& ex) {
			Reply(context, caller, "perm_connector: slot " + std::to_string(slot) + ": error: " + ex.what());
		} catch (...) {
			Reply(context, caller, "perm_connector: slot " + std::to_string(slot) + ": unknown error");
		}
	}
	Reply(context, caller, "perm_connector: connected human players: " + std::to_string(humans));
	Reply(context, caller, "perm_connector: --- perm_status end ---");
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType PermConnectorPlugin::HandleCookieCommand(int32_t caller, s2sdk::ConCommandContext context,
														   const plg::vector<plg::string>& args) {
	std::vector<std::string> argv;
	argv.reserve(args.size());
	for (const plg::string& arg : args) {
		argv.push_back(ToString(arg));
	}
	if (!argv.empty() && argv.front() == "perm_cookie") {
		argv.erase(argv.begin());
	}

	if (argv.size() < 2 || argv.size() > 3) {
		Reply(context, caller, "usage: perm_cookie <slot> <name> [value]");
		return s2sdk::ResultType::Handled;
	}

	int32_t slot = 0;
	if (!ParseSlot(argv[0], slot)) {
		Reply(context, caller, "perm_connector: invalid slot " + Quoted(argv[0]));
		return s2sdk::ResultType::Handled;
	}
	if (slot < 0 || slot >= kMaxSlots) {
		Reply(context, caller, "perm_connector: slot " + std::to_string(slot) + " is out of range (0.." +
								   std::to_string(kMaxSlots - 1) + ")");
		return s2sdk::ResultType::Handled;
	}
	const std::string& cookieName = argv[1];
	if (cookieName.empty()) {
		Reply(context, caller, "perm_connector: cookie name must not be empty");
		return s2sdk::ResultType::Handled;
	}

	const uint64_t steamid64 = ResolveSlotSteamId(slot);
	if (steamid64 == 0) {
		Reply(context, caller, "perm_connector: slot " + std::to_string(slot) +
								   " has no connected, authorised player to act on");
		return s2sdk::ResultType::Handled;
	}

	if (argv.size() == 2) {
		plg::any value;
		const permissions::Status status = permissions::GetCookie(steamid64, plg::string(cookieName), value);
		if (status == permissions::Status::Success) {
			Reply(context, caller, "perm_connector: cookie " + Quoted(cookieName) + " of user " +
									   std::to_string(steamid64) + " = " + FormatValue(value));
		} else if (status == permissions::Status::CookieNotFound) {
			Reply(context, caller, "perm_connector: cookie " + Quoted(cookieName) + " is not set for user " +
									   std::to_string(steamid64));
		} else {
			Reply(context, caller, "perm_connector: GetCookie(" + std::to_string(steamid64) + ", " +
									   Quoted(cookieName) + ") failed: " + StatusText(status));
		}
		return s2sdk::ResultType::Handled;
	}

	const std::string& value = argv[2];
	const permissions::Status status = permissions::SetCookie(RequesterId(), steamid64, plg::string(cookieName),
															  plg::any(plg::string(value)), /*dontBroadcast=*/false);
	if (status == permissions::Status::Success) {
		Reply(context, caller, "perm_connector: cookie " + Quoted(cookieName) + " of user " +
								   std::to_string(steamid64) + " set to " + Quoted(value));
	} else {
		Reply(context, caller, "perm_connector: SetCookie(" + std::to_string(steamid64) + ", " +
								   Quoted(cookieName) + ", " + Quoted(value) + ") failed: " + StatusText(status));
	}
	return s2sdk::ResultType::Handled;
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_permConnectorPlugin)
