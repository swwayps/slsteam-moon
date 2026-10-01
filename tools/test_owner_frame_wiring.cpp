// Source wiring test: owner work and the interface vtable hooks ride
// CSteamEngine::ProcessIPCFrame, now that the IClient*::RunIPCFrame dispatcher
// detours are gone (upstream 8de3384).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
constexpr auto npos = std::string::npos;
int failures = 0;
void expect(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++failures;
}
std::string read(const char* path)
{
	std::ifstream stream(path);
	return {std::istreambuf_iterator<char>(stream), {}};
}
std::string between(const std::string& text, const std::string& from, const std::string& to)
{
	const auto begin = text.find(from);
	if (begin == npos) return {};
	const auto end = text.find(to, begin);
	return text.substr(begin, end == npos ? npos : end - begin);
}
}

int main()
{
	const std::string hooks = read("src/hooks.cpp");
	const std::string frame = between(hooks, "static uint32_t hkSteamEngine_ProcessIPCFrame(", "\n}\n");
	const std::string runInterface = between(frame, "if (cmd == EIPCCmd::RunInterface", "\n\telse\n");
	expect(!frame.empty() && !runInterface.empty(),
	       "ProcessIPCFrame and its RunInterface branch are found");
	expect(hooks.find("_RunIPCFrame") == npos,
	       "no IClient*::RunIPCFrame dispatcher detour remains");
	expect(runInterface.find("Hooks::placeVFTHooks();") != npos,
	       "interface vtable hooks are placed from the IPC frame");
	const auto latch = runInterface.find("OwnerWork::latchOwnerThread();");
	expect(latch != npos && runInterface.rfind("interface == EIPCInterface::Utils", latch) != npos,
	       "IClientUtils calls latch the owner thread");
	const auto drain = runInterface.find("OwnerWork::drainOnOwnerFrame();");
	const auto dispatch = runInterface.find("Hooks::CSteamEngine_ProcessIPCFrame.tramp.fn(");
	expect(drain != npos && dispatch != npos && drain < dispatch,
	       "owner work drains before Steam dispatches the call");
	const std::string place = between(hooks, "void Hooks::placeVFTHooks()", "\n}\n");
	expect(place.find("std::atomic<bool> placed") != npos && place.find("std::lock_guard") != npos,
	       "the vtable hooks are placed once, under a lock");
	return failures == 0 ? 0 : 1;
}
