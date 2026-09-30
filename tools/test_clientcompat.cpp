// Host regression test for the live IClientCompat bridge.

#include "sdk/IClientCompat.hpp"
#include "feats/compatlive.hpp"
#include "vftableinfo.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (condition) std::printf("ok:   %s\n", message);
	else { std::printf("FAIL: %s\n", message); ++failures; }
}

struct FakeCompat
{
	void** vtable = nullptr;
	std::string appMapping;
	std::string defaultMapping;
	std::uint32_t specifiedAppId = 0;
	std::string specifiedName;
	std::string specifiedConfig;
	int specifiedPriority = 0;
	unsigned int specifyCalls = 0;
	bool reflectSpecified = false;
};

const char* fakeGetCompatToolName(void* self, std::uint32_t appId)
{
	auto& fake = *static_cast<FakeCompat*>(self);
	const std::string& value = appId == 0
		? fake.defaultMapping : fake.appMapping;
	return value.c_str();
}

void fakeSpecifyCompatTool(
	void* self,
	std::uint32_t appId,
	const char* name,
	const char* config,
	int priority)
{
	auto& fake = *static_cast<FakeCompat*>(self);
	fake.specifiedAppId = appId;
	fake.specifiedName = name ? name : "";
	fake.specifiedConfig = config ? config : "";
	fake.specifiedPriority = priority;
	++fake.specifyCalls;
	if (fake.reflectSpecified) fake.appMapping = fake.specifiedName;
}

}

// The registry resolves these slots by name from IClientCompatMap at boot
// (1 and 4 on the 2026-09 client, 4 and 7 before); the wrapper is tested
// against the slot it is handed.
VFTableInfo_t::VFTableInfo_t(const char* type, const char* function, unsigned int slot)
	: typeName(type), functionName(function), address(0), index(slot) {}
namespace VFTIndexes::IClientCompat
{
VFTableInfo_t BIsCompatibilityToolEnabled{"16IClientCompatMap", "BIsCompatibilityToolEnabled", 3};
VFTableInfo_t GetCompatToolDisplayName{"16IClientCompatMap", "GetCompatToolDisplayName", 6};
VFTableInfo_t GetCompatToolName{"16IClientCompatMap", "GetCompatToolName", 4};
VFTableInfo_t SpecifyCompatTool{"16IClientCompatMap", "SpecifyCompatTool", 1};
}
int main()
{
	void* vtable[8]{};
	vtable[VFTIndexes::IClientCompat::SpecifyCompatTool.index] =
		reinterpret_cast<void*>(&fakeSpecifyCompatTool);
	vtable[VFTIndexes::IClientCompat::GetCompatToolName.index] =
		reinterpret_cast<void*>(&fakeGetCompatToolName);
	FakeCompat fake;
	fake.vtable = vtable;
	auto* compat = reinterpret_cast<IClientCompat*>(&fake);

	compat->specifyCompatTool(582010, "proton-cachyos", "", 250);
	check(fake.specifyCalls == 1 && fake.specifiedAppId == 582010,
	      "the registry slot dispatches SpecifyCompatTool for the requested app");
	check(fake.specifiedName == "proton-cachyos" &&
	      fake.specifiedConfig.empty() && fake.specifiedPriority == 250,
	      "SpecifyCompatTool preserves name, config, and priority ABI arguments");

	fake.defaultMapping = "proton-cachyos";
	check(std::string(compat->getCompatToolName(0)) == "proton-cachyos",
	      "the registry slot reads the default compatibility mapping");

	fake.appMapping = "GE-Proton";
	const auto existing = CompatLive::step(compat, 582010, false);
	check(existing.status == CompatLive::StepStatus::Ready &&
	      fake.specifyCalls == 1,
	      "an existing per-app mapping is ready without being overwritten");

	fake.appMapping.clear();
	const auto requested = CompatLive::step(compat, 582010, false);
	check(requested.status == CompatLive::StepStatus::Requested &&
	      fake.specifyCalls == 2 && fake.specifiedName == "proton-cachyos",
	      "an absent mapping requests the user's default tool exactly once");
	const auto waiting = CompatLive::step(compat, 582010, true);
	check(waiting.status == CompatLive::StepStatus::Waiting &&
	      fake.specifyCalls == 2,
	      "a submitted asynchronous mapping is polled without resubmission");
	fake.reflectSpecified = true;
	fake.appMapping.clear();
	const auto immediate = CompatLive::step(compat, 582010, false);
	check(immediate.status == CompatLive::StepStatus::Ready &&
	      fake.appMapping == "proton-cachyos",
	      "a synchronously visible mapping completes without another owner frame");
	fake.reflectSpecified = false;
	fake.appMapping.clear();

	fake.defaultMapping.clear();
	const auto fallback = CompatLive::step(compat, 238320, false);
	check(fallback.status == CompatLive::StepStatus::Requested &&
	      fake.specifiedName == "proton_experimental",
	      "Proton Experimental is used only when the default mapping is empty");
	check(CompatLive::step(nullptr, 582010, false).status ==
	      CompatLive::StepStatus::Unavailable,
	      "a missing live interface fails closed");

	if (failures == 0)
	{
		std::printf("client compat tests passed\n");
		return 0;
	}
	std::printf("%d client compat check(s) failed\n", failures);
	return 1;
}
