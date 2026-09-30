// Live integration harness for the native CM product-info client.
//
// NOT a unit test — it talks to real Valve CMs.  Used to de-risk the
// transport before deploying to the VM.  Build via the Makefile target
// `make test-cmclient-live` (links the .so's objects).
//
//   make test-cmclient-live && /tmp/test_cmclient_live 3035500 250900 638510

#include "../src/feats/cmclient.hpp"

#include "../src/log.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	g_pLog = std::unique_ptr<CLog>(new CLog("/tmp/test_cmclient_live.log"));

	std::vector<uint32_t> apps;
	for (int i = 1; i < argc; ++i)
		apps.push_back(static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10)));
	if (apps.empty()) apps = {3035500u, 250900u, 638510u};

	std::printf("requesting %zu apps...\n", apps.size());
	std::unordered_map<uint32_t, std::string> out;
	const bool ok = CmClient::fetchProductInfoDetailed(apps, out) ==
	                CmClient::FetchResult::Success;
	std::printf("fetchProductInfoDetailed -> %s, %zu buffers\n",
	            ok ? "true" : "false", out.size());
	for (uint32_t a : apps)
	{
		auto it = out.find(a);
		if (it == out.end())
		{
			std::printf("  app %u: MISSING\n", a);
			continue;
		}
		const bool hasDepots = it->second.find("\"depots\"") != std::string::npos;
		std::printf("  app %u: %zu bytes, depots=%s\n",
		            a, it->second.size(), hasDepots ? "yes" : "no");
		if (const char* dump = std::getenv("DUMP"))
		{
			(void)dump;
			std::printf("----- buffer for %u -----\n%.700s\n-----\n",
			            a, it->second.c_str());
		}
	}
	return ok ? 0 : 1;
}
