// SPDX-License-Identifier: AGPL-3.0-only
//
// PICS appinfo handler.
//
// Background: Steam pulls product info from PICS
// (CMsgClientPICSProductInfo{Request,Response}). The default
// response shape leaves apps the user doesn't have a license for
// out of the populated `apps` list (the appid lands in
// `unknown_app_ids`). Steam then tries to read the manifest GID
// from its appinfo cache, fails, and aborts with:
//
//   CDepotDownloadMgr::BYldRequestDepotManifest(App: X, Depot: X,
//     Manifest: 0, branch: ''): Failed to get manifest request code,
//     'Invalid Parameter'
//
// This module hooks CMsgClientPICSProductInfoResponse (EMSG 8904)
// and persists per-app Binary KeyValues (BKV) buffers to the local
// cache so the offline appinfo splice can put them in front of
// Steam on the next start. The injection is paired with the
// outbound-side `meta_data_only=false` flip in feats/apps.cpp so
// Valve actually returns the buffer.

#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

class CProtoBufMsgBase;
class CMsgClientPICSProductInfoResponse;

namespace PICS
{
	void recvMsg(CProtoBufMsgBase* msg);
	void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp);

	// Rollback gate for the old architecture that staged every depot from the
	// product-info buffer and started the periodic prewarm worker. The
	// event-driven real-plan path is default; only exact "1" re-enables it.
	inline bool legacyManifestStagingEnabled(const char* envValue)
	{
		return envValue && envValue[0] == '1' && envValue[1] == '\0';
	}

	// --- Pure install-staging planning (unit-tested in tools/test_pics.cpp) --

	// (depotId, public manifest gid) of one depot.
	using DepotGid = std::pair<uint32_t, uint64_t>;

	// An AddedApp and the depots mined from its provisioned appinfo buffer.
	struct AppDepots
	{
		uint32_t appId;
		std::vector<DepotGid> depots;
	};

	// One manifest to stage synchronously before Steam plans the install.
	struct StageTarget
	{
		uint32_t appId;
		uint32_t depotId;
		uint64_t gid;
	};

	// Given the AddedApps whose live product-info buffer was empty (so we
	// must stage their manifests ourselves) and a predicate telling us
	// whether we hold a depot's decryption key, return the deduplicated
	// list of (appId, depotId, gid) manifests to stage.
	//
	// A depot is kept only if it has a real public gid AND we hold its key
	// (staging a blob we can't decrypt is a wasted CDN round-trip).  The
	// SAME (depotId, gid) seen across multiple apps is staged once — the
	// fetch layer dedups by (gid, depotId) too, but deduping the plan saves
	// a redundant readiness check.  This decides the complete target set;
	// buildPendingStagePlan later removes exact manifests already on disk.
	inline std::vector<StageTarget> buildSyncStagePlan(
	    const std::vector<AppDepots>& apps,
	    const std::function<bool(uint32_t depotId)>& hasKey)
	{
		std::vector<StageTarget> out;
		std::unordered_set<uint64_t> seen;
		for (const auto& app : apps)
		{
			for (const auto& [depotId, gid] : app.depots)
			{
				if (gid == 0) continue;
				if (hasKey && !hasKey(depotId)) continue;
				const uint64_t key =
				    gid ^ (static_cast<uint64_t>(depotId) * 0x9E3779B97F4A7C15ULL);
				if (seen.insert(key).second)
				{
					out.push_back({app.appId, depotId, gid});
				}
			}
		}
		return out;
	}

	// Remove targets whose exact depot+gid manifest is already ready for
	// Steam.  The predicate is intentionally injected so the policy stays
	// unit-testable without touching the real depotcache.
	//
	// This is a filter, not a cap: every target is checked exactly once and
	// any number of missing targets may be returned.
	inline std::vector<StageTarget> buildPendingStagePlan(
	    const std::vector<StageTarget>& plan,
	    const std::function<bool(const StageTarget&)>& isReady)
	{
		std::vector<StageTarget> pending;
		pending.reserve(plan.size());
		for (const auto& target : plan)
		{
			if (!isReady || !isReady(target))
			{
				pending.push_back(target);
			}
		}
		return pending;
	}
}
