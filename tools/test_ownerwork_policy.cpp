// Dispatch policy test for OwnerWork (make test-ownerwork-policy): work
// submitted off the owner IPC thread is queued for that thread, and no
// environment setting sends it back to the calling thread.
#include "../src/feats/appinfostate.hpp"
#include "../src/feats/hotreload.hpp"
#include "../src/feats/packagepatch.hpp"
#include "../src/log.hpp"
#include "../src/ownerwork.hpp"
#include "../src/sdk/IClientCompat.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>
namespace
{
	int failures = 0;
	int syncRuns = 0;
	long syncTid = 0;
	void check(bool condition, const std::string& message)
	{
		std::printf("%s: %s\n", condition ? "ok" : "FAIL", message.c_str());
		if (!condition)
		{
			++failures;
		}
	}
	long threadId()
	{
		return static_cast<long>(syscall(SYS_gettid));
	}
}
// Steam-side calls the command runner reaches; recorded instead of executed.
namespace PackagePatch
{
	void synchronizePackage0(const PackageSnapshot&)
	{
		++syncRuns;
		syncTid = threadId();
	}
	bool runtimeRefreshPending() noexcept { return false; }
	void reprocessCurrentState() noexcept {}
}
// Order of the compat calls and ownership publications the runner makes:
// 's' = specifyCompatTool, 'p' = publishPreparedBase.
std::string g_compatEvents;
namespace HotReload
{
	bool publishPreparedBase(std::uint32_t, std::uint64_t) noexcept
	{
		g_compatEvents += 'p';
		return true;
	}
}
namespace AppInfoState
{
	bool resolvedDirtyHint() noexcept { return false; }
	bool takeResolvedDirty() noexcept { return false; }
}
IClientCompat* g_pClientCompat = nullptr;
const char* IClientCompat::getCompatToolName(const AppId_t) { return ""; }
void IClientCompat::specifyCompatTool(const AppId_t, const char*, const char*, int32_t)
{
	g_compatEvents += 's';
}
// The config translation unit publishes configured DLC ids to the apps
// feature, which this test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}
int main()
{
	const std::string root = "/tmp/slssteam-ownerwork-policy." + std::to_string(getpid());
	std::filesystem::create_directories(root);
	const std::string logPath = root + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));

	setenv("SLSSTEAM_OWNER_QUEUE", "off", 1);
	OwnerWork::init();
	OwnerWork::latchOwnerThread();

	PackageSnapshot snapshot;
	snapshot.generation = 1;
	snapshot.appIds = {123};
	OwnerWork::Mode mode = OwnerWork::Mode::Abandoned;
	std::thread watcher([&] { mode = OwnerWork::submitManagedState(snapshot); });
	watcher.join();
	check(mode == OwnerWork::Mode::Queued,
	      "a submit from another thread is queued even with SLSSTEAM_OWNER_QUEUE=off");
	check(syncRuns == 0, "the submitting thread does not run the package 0 sync");

	OwnerWork::drainOnOwnerFrame();
	check(syncRuns == 1 && syncTid == threadId(), "the owner frame runs the queued sync");

	// The compat manager is captured only on Steam's first compat call, so an
	// early request is retried. That retry has issued nothing yet: it must
	// still specify the mapping before the title is published.
	g_pClientCompat = nullptr;
	OwnerWork::submitCompatReadiness(42, 1);
	check(g_compatEvents.empty(), "no compat manager yet: nothing is specified or published");
	static unsigned char compatObject = 0;
	g_pClientCompat = reinterpret_cast<IClientCompat*>(&compatObject);
	OwnerWork::drainOnOwnerFrame();
	check(g_compatEvents == "sp",
	      "the retried request specifies the mapping before publishing (got \"" +
	      g_compatEvents + "\")");

	g_pLog.reset();
	std::ifstream in(logPath);
	std::stringstream log;
	log << in.rdbuf();
	check(log.str().find("SLSSTEAM_OWNER_QUEUE is no longer supported") != std::string::npos,
	      "a leftover SLSSTEAM_OWNER_QUEUE is reported as ignored");

	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_ownerwork_policy: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
