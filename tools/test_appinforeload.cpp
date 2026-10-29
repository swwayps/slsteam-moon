// Regression tests for publishing synthesized appinfo into Steam's live cache.

#include "../src/feats/appinforeload.hpp"
#include "../src/feats/appinfostate.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

static_assert(noexcept(AppInfoState::reloadFromDisk(
	std::span<const std::uint32_t>{})));

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

struct FakeCache
{
	bool readSucceeds = true;
	std::uint32_t appResolvedByRead = 0;
	std::unordered_map<std::uint32_t, std::array<std::uint8_t, 64>> apps;
};

bool readFromDisk(void* context)
{
	auto& cache = *static_cast<FakeCache*>(context);
	if (!cache.readSucceeds)
		return false;
	if (cache.appResolvedByRead != 0)
		cache.apps[cache.appResolvedByRead][0x1c] = 0xA5;
	return true;
}

void* lookup(void* context, std::uint32_t appId, bool create)
{
	auto& cache = *static_cast<FakeCache*>(context);
	auto found = cache.apps.find(appId);
	if (found != cache.apps.end())
		return found->second.data();
	if (!create)
		return nullptr;
	return cache.apps[appId].data();
}
}

int main()
{
	constexpr AppDataLayout::Layout layout{0x10, 0x1c};

	{
		HotReloadState::Store store;
		store.publish({420530});
		FakeCache cache;
		cache.apps[420530] = {};
		cache.appResolvedByRead = 420530;
		const AppInfoReload::Runtime runtime{
			&cache, &readFromDisk, &lookup};

		const std::vector<std::uint32_t> requested{420530, 420530, 999};
		const auto result = AppInfoReload::reload(
			runtime, requested, store, layout);

		check(result.status == AppInfoReload::Status::Loaded,
		      "a successful disk read reports a loaded live cache");
		check(result.resolved == 1,
		      "only one current managed generation is resolved");
		check(result.present == 2,
		      "every requested occurrence with a live SHA is observed");
		check(store.takeResolvedDirty(),
		      "a loaded SHA raises the owner-thread reprocess signal");
		check(!store.takeResolvedDirty(),
		      "duplicate requested ids do not raise duplicate signals");

		const auto alreadyResolved = AppInfoReload::reload(
			runtime, std::vector<std::uint32_t>{420530}, store, layout);
		check(alreadyResolved.resolved == 0 && alreadyResolved.present == 1,
		      "an already-resolved generation still reports live presence");
		check(AppInfoReload::allRequestedPresent(alreadyResolved, 1),
		      "runtime publication accepts a base resolved before its worker");
	}

	{
		// The lookup guard set the skip byte while the SHA was still empty;
		// Steam's install lookup refuses a record that keeps it.
		HotReloadState::Store store;
		store.publish({420530});
		FakeCache cache;
		cache.apps[420530] = {};
		cache.apps[420530][layout.skipOffset] = 1;
		cache.appResolvedByRead = 420530;
		cache.apps[777] = {};
		cache.apps[777][layout.skipOffset] = 1;
		cache.apps[777][layout.shaOffset] = 0x5A;
		const AppInfoReload::Runtime runtime{
			&cache, &readFromDisk, &lookup};

		(void)AppInfoReload::reload(
			runtime, std::vector<std::uint32_t>{420530, 777}, store, layout);
		check(cache.apps[420530][layout.skipOffset] == 0,
		      "a managed record resolved by the disk read loses its skip byte");
		check(cache.apps[777][layout.skipOffset] == 1,
		      "a record that is not managed keeps its skip byte");
	}

	{
		HotReloadState::Store store;
		store.publish({420530});
		FakeCache cache;
		cache.apps[420530] = {};
		cache.readSucceeds = false;
		const AppInfoReload::Runtime runtime{
			&cache, &readFromDisk, &lookup};

		const auto result = AppInfoReload::reload(
			runtime, std::vector<std::uint32_t>{420530}, store, layout);
		check(result.status == AppInfoReload::Status::ReadFailed,
		      "a failed Steam disk read remains restart-recoverable");
		check(result.resolved == 0 && result.present == 0 &&
		      !store.takeResolvedDirty(),
		      "a failed read cannot claim app metadata was published");
	}

	{
		HotReloadState::Store store;
		store.publish({420530});
		FakeCache cache;
		cache.apps[420530] = {};
		const AppInfoReload::Runtime unavailable{
			&cache, nullptr, &lookup};

		const auto result = AppInfoReload::reload(
			unavailable, std::vector<std::uint32_t>{420530}, store, layout);
		check(result.status == AppInfoReload::Status::Unavailable,
		      "a missing optional locator disables only live disk reload");
		check(result.resolved == 0 && result.present == 0 &&
		      !store.takeResolvedDirty(),
		      "an unavailable reload leaves the restart fallback untouched");
	}

	if (failures != 0)
	{
		std::fprintf(stderr, "test_appinforeload: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("appinfo live reload tests passed");
	return 0;
}
