// Standalone contract test for the IClientApps vtable wrapper.

#include "../src/sdk/IClientApps.hpp"
#include "../src/vftableinfo.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;
int calls = 0;
void* observedSelf = nullptr;
std::vector<std::uint32_t> observedIds;
bool requestResult = false;

#define CHECK(condition, message)                                           \
	do {                                                                     \
		if (!(condition)) { std::printf("FAIL: %s\n", message); ++failures; } \
		else { std::printf("ok:   %s\n", message); }                         \
	} while (0)

bool fakeRequestAppInfoUpdate(
	void* self,
	const std::uint32_t* appIds,
	int count)
{
	++calls;
	observedSelf = self;
	observedIds.assign(appIds, appIds + count);
	return requestResult;
}

struct FakeClientApps
{
	std::uintptr_t* vtable = nullptr;
};
}

// The registry resolves these slots from the client at boot; the wrapper is
// tested against the slot the registry hands it.
VFTableInfo_t::VFTableInfo_t(const char* type, const char* function, unsigned int slot)
	: typeName(type), functionName(function), address(0), index(slot) {}
namespace VFTIndexes::IClientApps
{
VFTableInfo_t GetAppData{"14IClientAppsMap", "GetAppData", 0};
VFTableInfo_t GetAppDataSection{"14IClientAppsMap", "GetAppDataSection", 5};
VFTableInfo_t RequestAppInfoUpdate{"14IClientAppsMap", "RequestAppInfoUpdate", 7};
VFTableInfo_t GetAppType{"14IClientAppsMap", "GetAppType", 10};
}

int main()
{
	std::array<std::uintptr_t, 8> vtable{};
	vtable[VFTIndexes::IClientApps::RequestAppInfoUpdate.index] =
		reinterpret_cast<std::uintptr_t>(&fakeRequestAppInfoUpdate);
	FakeClientApps fake{vtable.data()};
	auto* const apps = reinterpret_cast<IClientApps*>(&fake);

	const std::vector<std::uint32_t> expected{1149460, 3405340};
	requestResult = true;
	CHECK(apps->requestAppInfoUpdate(expected),
	      "successful Steam request result is propagated");
	CHECK(calls == 1 && observedSelf == apps && observedIds == expected,
	      "the registry slot receives the exact object, ordered ids, and count");

	requestResult = false;
	CHECK(!apps->requestAppInfoUpdate(expected),
	      "offline Steam request result is propagated");
	CHECK(calls == 2 && observedIds == expected,
	      "a second request is forwarded without mutating ids");

	CHECK(!apps->requestAppInfoUpdate({}),
	      "an empty request is rejected before entering Steam");
	CHECK(calls == 2, "the empty request does not call the vtable");

	return failures == 0 ? 0 : 1;
}
