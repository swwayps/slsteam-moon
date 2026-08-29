// SPDX-License-Identifier: AGPL-3.0-only

#include "hotreload_inputs.hpp"

#include "appinfo_provision.hpp"
#include "dlcids.hpp"
#include "hotreload_publish_policy.hpp"

#include "../config.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace DepotKey
{
std::vector<std::uint32_t> managedDepotsForApp(std::uint32_t appId);
}

namespace HotReloadInputs
{
BuildResult buildFromCaches(
	std::uint64_t generation,
	const std::unordered_set<std::uint32_t>& managedAppIds,
	const std::unordered_set<std::uint32_t>& metadataPendingBaseIds,
	const std::unordered_set<std::uint32_t>& metadataDeferredBaseIds,
	const std::unordered_set<std::uint32_t>& readyBaseIds)
{
	std::vector<std::uint32_t> sortedBases(
		managedAppIds.begin(), managedAppIds.end());
	std::sort(sortedBases.begin(), sortedBases.end());

	std::vector<AppInput> inputs;
	inputs.reserve(sortedBases.size());
	for (const std::uint32_t baseAppId : sortedBases)
	{
		AppInput input;
		input.baseAppId = baseAppId;
		input.publishReady = readyBaseIds.count(baseAppId) != 0;

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
					metadataPendingBaseIds.count(baseAppId) != 0 &&
					!metadataCandidates.empty();
				input.childMetadataMissing =
					!metadataCandidates.empty() && !metadataReady;
				if (HotReloadPublishPolicy::metadataCacheVisibleInSession(
					metadataReady,
					metadataDeferredBaseIds.count(baseAppId) != 0))
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
