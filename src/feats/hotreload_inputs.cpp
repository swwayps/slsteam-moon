// SPDX-License-Identifier: AGPL-3.0-only

#include "hotreload_inputs.hpp"

#include "appinfo_provision.hpp"
#include "dlcids.hpp"
#include "hotreload_publish_policy.hpp"

#include "../config.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DepotKey
{
std::vector<std::uint32_t> managedDepotsForApp(std::uint32_t appId);
}

namespace HotReloadInputs
{
namespace
{
// Resolve one managed base into its AppInput. This is the expensive step: it
// reads and SHA-validates the app's cached appinfo buffer, parses the appinfo
// VDF, and reads+parses its DLC-metadata sidecar (large for DLC-heavy titles).
// The three booleans are the only per-generation state it depends on, so the
// memo below can key on them plus the two cache-file mtimes.
AppInput buildOneInput(
	std::uint32_t baseAppId, bool pending, bool deferred, bool ready)
{
	AppInput input;
	input.baseAppId = baseAppId;
	input.publishReady = ready;

	try
	{
		std::string wire;
		input.cacheValid =
			AppInfoProvision::readValidatedCacheBuffer(baseAppId, wire);
		if (input.cacheValid)
		{
			input.cacheMtimeSecs =
				AppInfoProvision::cachePairMtimeSecs(baseAppId);
			const auto sources =
				AppInfoProvision::extractDlcAppIdsBySource(wire, baseAppId);
			const auto metadataCandidates =
				AppInfoProvision::selectDlcMetadataCandidates(baseAppId, sources);
			std::unordered_set<std::uint32_t> advertisedWithContent;
			const bool baseHasDlcDepots =
				AppInfoProvision::hasDepotsInDlc(wire);
			for (const std::uint32_t dlcId : sources.advertised)
			{
				if (baseHasDlcDepots ||
					!DepotKey::managedDepotsForApp(dlcId).empty())
				{
					advertisedWithContent.insert(dlcId);
				}
			}

			const auto selected = AppInfoProvision::selectDlcInjectionIds(
				sources, advertisedWithContent,
				g_config.injectAllAdvertisedDlc.get());
			input.plannerAppIds = selected.package0;

			DlcMetadata::CacheRecord metadata;
			const bool metadataReady =
				AppInfoProvision::readValidatedDlcMetadataCache(
					baseAppId, 0, metadata);
			// The coordinator, not sidecar existence alone, decides when a
			// runtime addition is complete.  A durable sidecar may exist after a
			// failed live splice/reload; keep license refresh deferred until the
			// worker explicitly confirms the live publication.
			input.childMetadataPending =
				pending && !metadataCandidates.empty();
			input.childMetadataMissing =
				!metadataCandidates.empty() && !metadataReady;
			if (HotReloadPublishPolicy::metadataCacheVisibleInSession(
				metadataReady, deferred))
			{
				std::unordered_set<std::uint32_t> rejected(
					metadata.rejectedAppIds.begin(),
					metadata.rejectedAppIds.end());
				input.plannerAppIds.erase(
					std::remove_if(
						input.plannerAppIds.begin(), input.plannerAppIds.end(),
						[&rejected](std::uint32_t appId)
						{
							return rejected.count(appId) != 0;
						}),
					input.plannerAppIds.end());
				std::unordered_set<std::uint32_t> seen(
					input.plannerAppIds.begin(), input.plannerAppIds.end());
				for (const auto& app : metadata.apps)
				{
					if (app.appid != 0 && seen.insert(app.appid).second)
						input.plannerAppIds.push_back(app.appid);
				}
			}
		}

		input.depotIds = DepotKey::managedDepotsForApp(baseAppId);
		for (const std::uint32_t plannerAppId : input.plannerAppIds)
		{
			const auto dlcDepots =
				DepotKey::managedDepotsForApp(plannerAppId);
			input.depotIds.insert(
				input.depotIds.end(), dlcDepots.begin(), dlcDepots.end());
		}
	}
	catch (...)
	{
		// One malformed or concurrently-changing app remains unresolved.  Its
		// base id still participates, and no partial planner ids escape.
		input.cacheValid = false;
		input.plannerAppIds.clear();
	}

	return input;
}

// Per-base input memo. Every publish rebuilt the full input set by re-reading
// and re-parsing each managed app's caches, so hot-reload latency scaled with
// library size (measured ~1.4s per build on a 155-app library, run more than
// once per hot-add). An app's AppInput is a pure function of its two cache-file
// mtimes and its ready/pending/deferred state, so cache it and only recompute
// when one of those changes (or the caller forces it).
struct MemoKey
{
	std::int64_t pairMtime = -1;
	std::int64_t dlcMetaMtime = -1;
	bool ready = false;
	bool pending = false;
	bool deferred = false;

	bool operator==(const MemoKey& o) const
	{
		return pairMtime == o.pairMtime && dlcMetaMtime == o.dlcMetaMtime &&
			ready == o.ready && pending == o.pending && deferred == o.deferred;
	}
};

std::mutex g_memoMu;
std::unordered_map<std::uint32_t, std::pair<MemoKey, AppInput>> g_memo;
} // namespace

void clearInputMemo() noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(g_memoMu);
		g_memo.clear();
	}
	catch (...) {}
}

BuildResult buildFromCaches(
	std::uint64_t generation,
	const std::unordered_set<std::uint32_t>& managedAppIds,
	const std::unordered_set<std::uint32_t>& metadataPendingBaseIds,
	const std::unordered_set<std::uint32_t>& metadataDeferredBaseIds,
	const std::unordered_set<std::uint32_t>& readyBaseIds,
	const std::unordered_set<std::uint32_t>& forceRecompute)
{
	std::vector<std::uint32_t> sortedBases(
		managedAppIds.begin(), managedAppIds.end());
	std::sort(sortedBases.begin(), sortedBases.end());

	std::vector<AppInput> inputs;
	inputs.reserve(sortedBases.size());

	std::lock_guard<std::mutex> memoLock(g_memoMu);
	// Forget apps that are no longer managed so the memo cannot grow without
	// bound across a session of adds and removes.
	for (auto it = g_memo.begin(); it != g_memo.end();)
		it = managedAppIds.count(it->first) == 0 ? g_memo.erase(it)
		                                         : std::next(it);

	for (const std::uint32_t baseAppId : sortedBases)
	{
		const MemoKey key{
			AppInfoProvision::cachePairMtimeSecs(baseAppId),
			AppInfoProvision::dlcMetadataCacheMtimeSecs(baseAppId),
			readyBaseIds.count(baseAppId) != 0,
			metadataPendingBaseIds.count(baseAppId) != 0,
			metadataDeferredBaseIds.count(baseAppId) != 0,
		};
		if (forceRecompute.count(baseAppId) == 0)
		{
			const auto it = g_memo.find(baseAppId);
			if (it != g_memo.end() && it->second.first == key)
			{
				inputs.push_back(it->second.second);
				continue;
			}
		}
		AppInput input =
			buildOneInput(baseAppId, key.pending, key.deferred, key.ready);
		g_memo[baseAppId] = {key, input};
		inputs.push_back(std::move(input));
	}

	try
	{
		return build(generation, inputs);
	}
	catch (...)
	{
		BuildResult failed;
		failed.snapshot.generation = generation;
		failed.snapshot.metadataComplete = false;
		return failed;
	}
}
} // namespace HotReloadInputs
