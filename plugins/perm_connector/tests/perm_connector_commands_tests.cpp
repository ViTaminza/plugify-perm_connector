// perm_connector command tests — no engine: the plugin TU is compiled in (the handlers live in
// an anonymous namespace) and the __s2sdk_* binding globals are filled with recording fakes.
// Covered:
//   * a missing binding leaves the plugin inactive with an Ok result (a Failed result would stop
//     every Plugify plugin on the server);
//   * perm_status / perm_cookie reject every caller but the server console / RCON (caller -1):
//     the s2sdk chat path dispatches console commands by name without checking their flags, so
//     "!perm_status" / "!perm_cookie ..." from a client must hit the gate;
//   * a rejected caller gets exactly one reply and never the report or the usage lines;
//   * the server console still reaches the normal paths (usage, no-map report);
//   * a slot that changes hands without a disconnect event releases the previous user from the
//     permissions core before the newcomer is loaded.
// Run by ./test.sh (build/tests/perm_connector_commands_tests).

#include "../src/plugin.cpp"

#include <array>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

struct Reply {
	s2sdk::ConCommandContext context;
	int32_t caller;
	std::string text;
};

std::vector<Reply> g_replies;

void FakeReplyToCommand(s2sdk::ConCommandContext context, int32_t caller, const plg::string& message) {
	g_replies.push_back({context, caller, std::string(std::string_view(message))});
}

void ResetReplies() {
	g_replies.clear();
}

bool HasReply(const std::string& needle) {
	for (const Reply& reply : g_replies) {
		if (reply.text.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

plg::vector<plg::string> Args(std::initializer_list<const char*> words) {
	plg::vector<plg::string> args;
	for (const char* word : words) {
		args.emplace_back(word);
	}
	return args;
}

void InstallFakes() {
	__s2sdk_ReplyToCommand = &FakeReplyToCommand;
}

// --- fake clients / permissions core (slot-reuse test) --------------------------

constexpr int32_t kFakeMaxSlots = 64;
std::array<uint64_t, kFakeMaxSlots> g_slotSteamIds{};
std::vector<uint64_t> g_loadRequests;
std::vector<uint64_t> g_deletedUsers;
std::vector<uint64_t> g_onlineUsers; // UserExists() answers Online for these

bool FakeIsClientConnected(int32_t slot) {
	return slot >= 0 && slot < kFakeMaxSlots && g_slotSteamIds[static_cast<std::size_t>(slot)] != 0;
}

bool FakeIsClientAuthorized(int32_t slot) {
	return FakeIsClientConnected(slot);
}

bool FakeIsFakeClient(int32_t) {
	return false;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	return slot >= 0 && slot < kFakeMaxSlots ? g_slotSteamIds[static_cast<std::size_t>(slot)] : 0;
}

plg::string FakeGetClientName(int32_t) {
	return plg::string("Tester");
}

permissions::Status FakeLoadUser(int64_t, uint64_t steamid, const plg::string&, bool, bool) {
	g_loadRequests.push_back(steamid);
	return permissions::Status::Success;
}

permissions::PlayerState FakeUserExists(uint64_t steamid) {
	for (const uint64_t online : g_onlineUsers) {
		if (online == steamid) {
			return permissions::PlayerState::Online;
		}
	}
	return permissions::PlayerState::NotFound;
}

permissions::Status FakeDeleteUser(int64_t, uint64_t steamid) {
	g_deletedUsers.push_back(steamid);
	return permissions::Status::Success;
}

plg::uuid FakeGetPluginId(plg::hndl) {
	return 42;
}

void InstallClientFakes() {
	plg::plugin::GetId = &FakeGetPluginId; // RequesterId() (the permissions calls take it)
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__permissions_LoadUser = &FakeLoadUser;
	__permissions_UserExists = &FakeUserExists;
	__permissions_DeleteUser = &FakeDeleteUser;
}

void TestMissingBindingsFailSoft() {
	std::printf("-- missing bindings leave the plugin inactive without failing the pipeline\n");

	// Nothing is installed yet: every binding the plugin needs is null.
	const plg::PluginResult result = g_permConnectorPlugin.OnPluginStart();
	Check(result.GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart without the bindings answers Ok (a Failed result stops every plugin)");
}

void TestSlotReuseReleasesPreviousUser() {
	std::printf("-- a slot reused without a disconnect event releases the previous user\n");

	InstallClientFakes();
	g_permConnectorPlugin.HandleMapStart(); // a map is up: client queries are allowed

	constexpr int32_t slot = 5;
	const uint64_t idA = 76561198000000001ULL;
	const uint64_t idB = 76561198000000002ULL;

	g_slotSteamIds[slot] = idA;
	g_onlineUsers.push_back(idA);
	g_permConnectorPlugin.HandleClientAuthenticated(slot, idA);
	Check(g_loadRequests.size() == 1 && g_loadRequests.back() == idA, "the first player is loaded");

	// B takes the slot before any disconnect event for A.
	g_slotSteamIds[slot] = idB;
	g_permConnectorPlugin.HandleClientAuthenticated(slot, idB);

	Check(g_deletedUsers.size() == 1 && g_deletedUsers.back() == idA,
		  "the previous user is deleted from the permissions core");
	Check(g_loadRequests.size() == 2 && g_loadRequests.back() == idB, "the newcomer is loaded");
}

void TestSlotReuseKeepsUserTrackedElsewhere() {
	std::printf("-- a reused slot does not release a user that another slot still tracks\n");

	constexpr int32_t oldSlot = 6;
	constexpr int32_t newSlot = 13;
	const uint64_t idC = 76561198000000003ULL;
	const uint64_t idD = 76561198000000004ULL;

	// C was on oldSlot and reconnects on newSlot before oldSlot's disconnection is noticed.
	g_slotSteamIds[oldSlot] = idC;
	g_onlineUsers.push_back(idC);
	g_permConnectorPlugin.HandleClientAuthenticated(oldSlot, idC);
	g_slotSteamIds[newSlot] = idC;
	g_permConnectorPlugin.HandleClientAuthenticated(newSlot, idC);

	// The old slot is taken by someone else: C's user must stay in the core (it is newSlot's).
	const std::size_t deletedBefore = g_deletedUsers.size();
	const std::size_t loadsBefore = g_loadRequests.size();
	g_slotSteamIds[oldSlot] = idD;
	g_permConnectorPlugin.HandleClientAuthenticated(oldSlot, idD);
	Check(g_deletedUsers.size() == deletedBefore,
		  "the user tracked on the other slot is not deleted from the permissions core");
	Check(g_loadRequests.size() == loadsBefore + 1 && g_loadRequests.back() == idD, "the newcomer is loaded");

	// The last slot that tracks the user still releases it on disconnect.
	g_slotSteamIds[newSlot] = 0;
	g_permConnectorPlugin.HandleClientDisconnect(newSlot);
	Check(g_deletedUsers.size() == deletedBefore + 1 && g_deletedUsers.back() == idC,
		  "the last slot tracking the user releases it on disconnect");
}

void TestClientsAreRejected() {
	std::printf("-- perm_status / perm_cookie from a client\n");

	ResetReplies();
	const s2sdk::ResultType status = g_permConnectorPlugin.HandleStatusCommand(
		7, s2sdk::ConCommandContext::Chat, Args({"perm_status"}));
	Check(status == s2sdk::ResultType::Handled, "a chat perm_status answers Handled");
	Check(g_replies.size() == 1, "a chat perm_status gets exactly one reply");
	Check(HasReply("server console only"), "the reply says the command is server-console only");
	Check(!HasReply("perm_status begin"), "no report is printed for a chat caller");

	ResetReplies();
	const s2sdk::ResultType cookie = g_permConnectorPlugin.HandleCookieCommand(
		7, s2sdk::ConCommandContext::Chat, Args({"perm_cookie", "3", "menu_type", "wasd"}));
	Check(cookie == s2sdk::ResultType::Handled, "a chat perm_cookie answers Handled");
	Check(g_replies.size() == 1, "a chat perm_cookie gets exactly one reply");
	Check(HasReply("perm_cookie is available on the server console only"),
		  "the perm_cookie reply is the gate line");

	// A player's own console (context Console, caller >= 0) is a client too.
	ResetReplies();
	g_permConnectorPlugin.HandleStatusCommand(3, s2sdk::ConCommandContext::Console, Args({"perm_status"}));
	Check(g_replies.size() == 1 && HasReply("server console only"), "a client console caller is rejected");

	// The gate runs before any argument handling: extra arguments cannot turn it into a usage line.
	ResetReplies();
	g_permConnectorPlugin.HandleCookieCommand(3, s2sdk::ConCommandContext::Chat, Args({"perm_cookie"}));
	Check(g_replies.size() == 1 && HasReply("server console only") && !HasReply("usage:"),
		  "a rejected caller sees the gate line, not the usage text");
}

void TestServerConsoleStillWorks() {
	std::printf("-- the server console keeps its paths\n");

	// No map is running (the plugin's initial state): the server gets the no-map report, not the
	// gate line.
	ResetReplies();
	const s2sdk::ResultType status = g_permConnectorPlugin.HandleStatusCommand(
		-1, s2sdk::ConCommandContext::Console, Args({"perm_status"}));
	Check(status == s2sdk::ResultType::Handled, "a console perm_status answers Handled");
	Check(g_replies.size() == 1, "a console perm_status gets the no-map reply");
	Check(HasReply("no map is running"), "the server console is not rejected by the gate");

	// And the argument checks are still in front of the work.
	ResetReplies();
	g_permConnectorPlugin.HandleCookieCommand(-1, s2sdk::ConCommandContext::Console, Args({"perm_cookie"}));
	Check(g_replies.size() == 1 && HasReply("usage: perm_cookie <slot> <name> [value]"),
		  "a console perm_cookie without arguments gets the usage line");
}

} // namespace

int main() {
	std::printf("perm_connector command tests (fake s2sdk bindings, no engine)\n");
	TestMissingBindingsFailSoft(); // must run before any binding is installed
	InstallFakes();
	TestClientsAreRejected();
	TestServerConsoleStillWorks();
	TestSlotReuseReleasesPreviousUser();
	TestSlotReuseKeepsUserTrackedElsewhere();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
