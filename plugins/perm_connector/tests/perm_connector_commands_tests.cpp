// perm_connector command tests — no engine: the plugin TU is compiled in (the handlers live in
// an anonymous namespace) and the __s2sdk_* binding globals are filled with recording fakes.
// Covered:
//   * perm_status / perm_cookie reject every caller but the server console / RCON (caller -1):
//     the s2sdk chat path dispatches console commands by name without checking their flags, so
//     "!perm_status" / "!perm_cookie ..." from a client must hit the gate;
//   * a rejected caller gets exactly one reply and never the report or the usage lines;
//   * the server console still reaches the normal paths (usage, no-map report).
// Run by ./test.sh (build/tests/perm_connector_commands_tests).

#include "../src/plugin.cpp"

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
	InstallFakes();
	TestClientsAreRejected();
	TestServerConsoleStillWorks();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
