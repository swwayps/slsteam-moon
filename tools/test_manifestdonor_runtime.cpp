// Donor worker lifecycle against the real ManifestDonor (make
// test-manifestdonor-runtime). Steam calls and archive HTTP are recorded
// instead of executed.
#include "../src/config.hpp"
#include "../src/feats/manifestcode.hpp"
#include "../src/feats/manifestdonor.hpp"
#include "../src/feats/stats_policy.hpp"
#include "../src/log.hpp"
#include "../src/sdk/protobufs/steammessages_clientserver.pb.h"
#include "../src/utils/ManifestFetch.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
namespace
{
	int failures = 0;
	std::atomic<int> codeRequests{0};
	std::atomic<int> httpRequests{0};
	std::string logPath;
	void check(bool condition, const std::string& message)
	{
		std::printf("%s: %s\n", condition ? "ok" : "FAIL", message.c_str());
		if (!condition)
		{
			++failures;
		}
	}
	std::string logText()
	{
		std::ifstream in(logPath);
		std::stringstream text;
		text << in.rdbuf();
		return text.str();
	}
	bool waitForLog(const std::string& needle, std::chrono::milliseconds limit)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (logText().find(needle) != std::string::npos) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}
}
namespace ManifestCode
{
	CodeRequest requestCode(uint32_t, uint32_t, uint64_t)
	{
		++codeRequests;
		std::promise<uint64_t> promise;
		CodeRequest request;
		request.result = promise.get_future();
		promise.set_value(0);
		return request;
	}
	void discardRequest(uint64_t) {}
}
namespace ManifestFetch
{
	HttpResponse archiveRequest(const std::string&, const std::string&,
	                            const std::string&, std::size_t, long,
	                            const std::atomic<bool>*)
	{
		++httpRequests;
		HttpResponse response;
		response.status = 404;
		return response;
	}
}
namespace StatsPolicy
{
	Context context() { return {}; }
	void replaceNativePackageApps(Context, const std::vector<uint32_t>&) {}
	bool hasNativeLicense(uint32_t) { return false; }
}
// The config translation unit publishes configured DLC ids to the apps
// feature, which this test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}
int main()
{
	const std::string root = "/tmp/slssteam-donor-runtime." + std::to_string(getpid());
	const std::string configDir = root + "/config/SLSsteam";
	std::filesystem::create_directories(configDir);
	setenv("HOME", root.c_str(), 1);
	setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
	logPath = root + "/test.log";
	g_pLog = std::unique_ptr<CLog>(new CLog(logPath.c_str()));
	g_config.logLevel.set(0);

	// A depot Steam asked a code for, and a probe file naming it.
	ManifestDonor::observeDepot(10, 20, 30);
	std::ofstream(configDir + "/manifest_probe.txt") << "20\n";

	CMsgClientLicenseList licenses;
	licenses.set_eresult(1);
	CConfig::DonateSettings off;
	off.enabled = false;
	g_config.donate.set(off);
	ManifestDonor::onLicenseList(&licenses);
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	check(logText().find("Donor: started") == std::string::npos,
	      "with Donate.Enabled off the worker does not start");
	check(codeRequests.load() == 0 && httpRequests.load() == 0,
	      "with Donate.Enabled off no Steam code request or archive request is made");

	CConfig::DonateSettings on;
	g_config.donate.set(on);
	ManifestDonor::onLicenseList(&licenses);
	check(waitForLog("Donor: started", std::chrono::milliseconds(3000)),
	      "with Donate.Enabled on the next license list starts the worker");
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	check(codeRequests.load() == 0,
	      "a probe file does not make the worker request a manifest code");

	ManifestDonor::stop();
	g_pLog.reset();
	std::filesystem::remove_all(root);
	if (failures != 0)
	{
		std::fprintf(stderr, "test_manifestdonor_runtime: %d failure(s)\n", failures);
		return 1;
	}
	return 0;
}
