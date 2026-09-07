#include "../src/sdk/CUtl.hpp"
#include "../src/sdk/steam.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
	if (condition)
	{
		std::printf("ok:   %s\n", message);
		return;
	}

	std::printf("FAIL: %s\n", message);
	++failures;
}

std::string read(const char* path)
{
	std::ifstream stream(path);
	return {std::istreambuf_iterator<char>(stream), {}};
}

std::size_t occurrences(const std::string& text, const std::string& needle)
{
	std::size_t count = 0;
	for (std::size_t pos = 0;
	     (pos = text.find(needle, pos)) != std::string::npos;
	     pos += needle.size())
	{
		++count;
	}
	return count;
}
}

int main()
{
	static_assert(sizeof(EIPCExitCode) == 1);

	const std::string hooks = read("src/hooks.cpp");
	const std::string engine = read("src/sdk/CSteamEngine.cpp");
	const std::string fakeAppIds = read("src/feats/fakeappid.cpp");
	const std::string patterns = read("src/patterns.cpp");
	const std::string process = read("src/process.hpp");

	expect(hooks.find("createAndPlaceSteamIdHook") == std::string::npos,
	       "legacy naked SteamID hook is removed");
	expect(hooks.find("hkNakedGetSteamId") == std::string::npos,
	       "legacy executable SteamID trampoline is removed");
	expect(hooks.find("0xD6FC3200") != std::string::npos,
	       "SteamID result handling lives in ProcessIPCFrame");
	expect(patterns.find("IClientUser::GetSteamID") == std::string::npos,
	       "the removed SteamID hook has no stale locator");
	expect(occurrences(
		hooks, "CWebSocketConnection_BBuildAndAsyncSendFrame.remove();") == 1,
	       "WebSocket detour has one teardown path");

	expect(engine.find("CUtlVector<CUser*>") != std::string::npos,
	       "CSteamEngine::getUser uses the upstream CUtlVector layout");
	expect(engine.find("index >= vec->size") != std::string::npos,
	       "CSteamEngine::getUser checks the vector's logical size");
	expect(engine.find("ppUserMap") == std::string::npos,
	       "raw legacy CUser map decoding is removed");

	const auto runStart = fakeAppIds.find("void FakeAppIds::runIPCFrame");
	const auto runEnd = fakeAppIds.find("void FakeAppIds::getServerDetails", runStart);
	const std::string run = fakeAppIds.substr(runStart, runEnd - runStart);
	expect(run.find("extendedLogging") != std::string::npos,
	       "runIPCFrame resolves IClientUtils only for extended logging");
	expect(run.find("if (!utils)") == std::string::npos,
	       "logging lookup cannot suppress the AppID transition");
	expect(process.find("cmdLine") == std::string::npos,
	       "unused command-line state is not retained");

	CUtlBuffer buffer{};
	expect(!buffer.hasBytes(1), "a null IPC buffer has no readable bytes");
	unsigned char bytes[10]{};
	buffer.mem.base = bytes;
	buffer.put = 9;
	expect(!buffer.hasBytes(10), "a short IPC buffer is rejected");
	buffer.put = 10;
	expect(buffer.hasBytes(10), "a complete IPC buffer is readable");

	return failures == 0 ? 0 : 1;
}
