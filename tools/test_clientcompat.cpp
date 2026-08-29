// Host regression test for the live IClientCompat bridge.

#include "sdk/IClientCompat.hpp"
#include "feats/compatlive.hpp"
#include "vftableinfo.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

std::vector<std::size_t> findCompatOffsetSignature(const std::string& image)
{
	// lea ebx,[esi+disp32] ; spill ; push ebx ; mov ebx,edi ; call ctor ;
	// pop/pop ; initialize the next CUser member. The displacement is decoded
	// by CompatLive rather than pinned to one Steam build.
	static constexpr int pattern[] = {
		0x8d, 0x9e, -1, -1, -1, -1,
		0x89, 0x9d, -1, -1, -1, -1,
		0x53, 0x89, 0xfb, 0xe8, -1, -1, -1, -1,
		0x58, 0x5a, 0xc7, 0x86, -1, -1, -1, -1,
		0xff, 0xff, 0xff, 0xff,
	};
	std::vector<std::size_t> matches;
	for (std::size_t offset = 0;
		offset + std::size(pattern) <= image.size(); ++offset)
	{
		bool equal = true;
		for (std::size_t index = 0; index < std::size(pattern); ++index)
		{
			if (pattern[index] >= 0 &&
				static_cast<unsigned char>(image[offset + index]) !=
					static_cast<unsigned char>(pattern[index]))
			{
				equal = false;
				break;
			}
		}
		if (equal) matches.push_back(offset);
	}
	return matches;
}
}

int main(int argc, char** argv)
{
	void* vtable[8]{};
	vtable[VFTIndexes::IClientCompat::SpecifyCompatTool] =
		reinterpret_cast<void*>(&fakeSpecifyCompatTool);
	vtable[VFTIndexes::IClientCompat::GetCompatToolName] =
		reinterpret_cast<void*>(&fakeGetCompatToolName);
	FakeCompat fake;
	fake.vtable = vtable;
	auto* compat = reinterpret_cast<IClientCompat*>(&fake);

	compat->specifyCompatTool(582010, "proton-cachyos", "", 250);
	check(fake.specifyCalls == 1 && fake.specifiedAppId == 582010,
	      "vtable index 4 dispatches SpecifyCompatTool for the requested app");
	check(fake.specifiedName == "proton-cachyos" &&
	      fake.specifiedConfig.empty() && fake.specifiedPriority == 250,
	      "SpecifyCompatTool preserves name, config, and priority ABI arguments");

	fake.defaultMapping = "proton-cachyos";
	check(std::string(compat->getCompatToolName(0)) == "proton-cachyos",
	      "vtable index 7 reads the default compatibility mapping");

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

	const std::uint8_t locator[] = {0x8d, 0x9e, 0x78, 0x35, 0x00, 0x00};
	const auto decoded = CompatLive::decodeManagerOffset(locator);
	check(decoded.has_value() && *decoded == 0x3578,
	      "the CUser member locator decodes its little-endian displacement");
	const std::uint8_t invalid[] = {0x8d, 0x86, 0x78, 0x35, 0x00, 0x00};
	check(!CompatLive::decodeManagerOffset(invalid).has_value(),
	      "a neighboring LEA register form is rejected");

	if (argc == 2)
	{
		std::ifstream input(argv[1], std::ios::binary);
		check(static_cast<bool>(input), "Steam client fixture opens");
		if (input)
		{
			const std::string image{
				std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>()};
			const auto matches = findCompatOffsetSignature(image);
			check(matches.size() == 1,
			      "CCompatManager member locator is unique in the Steam client");
			if (matches.size() == 1)
			{
				const auto* bytes = reinterpret_cast<const std::uint8_t*>(
					image.data() + matches.front());
				const auto offset = CompatLive::decodeManagerOffset(
					std::span<const std::uint8_t>(bytes, 6));
				check(offset.has_value() && *offset > 0,
				      "unique live locator yields a positive CUser member offset");
			}
		}
	}

	if (failures == 0)
	{
		std::printf("client compat tests passed\n");
		return 0;
	}
	std::printf("%d client compat check(s) failed\n", failures);
	return 1;
}
