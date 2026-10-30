// A pin lookup reads the pin map in place (make test-pin-lookup). It runs for
// every planned depot on Steam's download threads, and a copy of the map cost
// allocations proportional to every pinned app and depot each time.
#include "../src/config.hpp"
#include "../src/log.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>
namespace
{
	int failures = 0;
	void check(bool condition, const std::string& message)
	{
		std::printf("%s: %s\n", condition ? "ok" : "FAIL", message.c_str());
		if (!condition) ++failures;
	}
	std::atomic<bool> g_counting{false};
	std::atomic<int> g_allocations{0};
}
void* operator new(std::size_t size)
{
	if (g_counting.load(std::memory_order_relaxed))
		g_allocations.fetch_add(1, std::memory_order_relaxed);
	if (void* p = std::malloc(size ? size : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
// The config translation unit publishes configured DLC ids to the apps
// feature, which this test does not link.
namespace Apps
{
	void setConfiguredAppDlcIds(const std::vector<uint32_t>&) {}
}
int main()
{
	g_pLog = std::unique_ptr<CLog>(new CLog("/dev/null"));
	ManifestPins::PinMap pins;
	for (uint32_t app = 1; app <= 100; ++app)
		for (uint32_t depot = 0; depot < 5; ++depot)
			pins[app].depots[app * 100 + depot] = 1000 + depot;
	pins[42].depots[4201] = 777;
	g_config.manifestPinsByApp.set(pins);

	g_counting.store(true);
	const uint64_t pin = g_config.getManifestPin(42, 4201);
	const uint64_t planned = g_config.getManifestPinForPlanner(42, 4201);
	const uint64_t missing = g_config.getManifestPin(42, 9999);
	g_counting.store(false);
	check(pin == 777 && planned == 777 && missing == 0,
	      "the lookups return the pinned gid and 0 for an unpinned depot");
	check(g_allocations.load() == 0,
	      "a pin lookup allocates nothing (got " + std::to_string(g_allocations.load()) + ")");
	return failures == 0 ? 0 : 1;
}
