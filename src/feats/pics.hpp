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

#include "protowire.hpp"
#include "provision_refresh.hpp"

#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

class CMsgClientPICSProductInfoResponse;
class CMsgClientPICSChangesSinceResponse;

namespace PICS
{
	void recvProductInfoResponse(CMsgClientPICSProductInfoResponse* resp);
	// An answer that names no added app: only the repairs that wait for any
	// product-info callback run.
	void recvUnmanagedProductInfoResponse();
	// Remove Steam's shader hit cache (<appid>_pbuf under userdata) of apps
	// whose appinfo was just published.
	void cleanShaderHitCaches(const std::unordered_set<uint32_t>& appIds);
	void recvChangesSinceResponse(CMsgClientPICSChangesSinceResponse* resp);

	// Apps the request hook hid from Steam's metadata request `jobId`. Their
	// answer is given from the local pairs, or Steam asks for them again and
	// again (answerHiddenApps).
	void noteHiddenApps(std::uint64_t jobId, std::vector<std::uint32_t> appIds);
	// The apps hidden from the request a response answers; each request's
	// list is handed out once.
	std::vector<std::uint32_t> takeHiddenApps(std::uint64_t jobId);
	// Adds to `resp` the metadata Steam holds for each hidden app from our
	// splice (AppInfoVdf::heldEntry): its change number and SHA-1, so Steam
	// sees it as current. False when nothing was added.
	bool answerHiddenApps(const std::vector<std::uint32_t>& appIds,
	                      CMsgClientPICSProductInfoResponse& resp);

	// Whether a serialized product-info answer names an app `isAdded`
	// accepts, read from the wire without copying the app buffers. An answer
	// that does not walk cleanly counts as naming one and gets the full parse.
	template <typename IsAdded>
	bool namesAddedApp(std::span<const std::uint8_t> message, IsAdded&& isAdded)
	{
		bool named = false;
		const bool walked = ProtoWire::walk(message.data(), message.size(),
			[&](const ProtoWire::Field& field)
			{
				// CMsgClientPICSProductInfoResponse.apps = 1, AppInfo.appid = 1.
				if (named || field.number != 1 || field.wireType != 2) return;
				const std::uint8_t* app = message.data() + field.valueOff;
				const bool appWalked = ProtoWire::walk(app, field.valueLen,
					[&](const ProtoWire::Field& appField)
					{
						if (appField.number != 1 || appField.wireType != 0) return;
						std::size_t at = appField.valueOff;
						std::uint64_t appId = 0;
						if (!ProtoWire::readVarint(app, field.valueLen, at, appId) ||
							appId > UINT32_MAX || isAdded(static_cast<std::uint32_t>(appId)))
							named = true;
					});
				named = named || !appWalked;
			});
		return named || !walked;
	}


	inline std::unordered_set<uint32_t> selectManagedResponseApps(
	    const std::unordered_set<uint32_t>& managed,
	    const std::vector<uint32_t>& responseAppIds)
	{
		std::unordered_set<uint32_t> selected;
		for (const uint32_t appId : responseAppIds)
			if (managed.count(appId) != 0) selected.insert(appId);
		return selected;
	}

	inline bool rawResponseCanRepairCache(
		bool hasBuffer, AppInfoProvision::CacheReadiness readiness) noexcept
	{
		return hasBuffer &&
			(readiness == AppInfoProvision::CacheReadiness::Missing ||
			 readiness == AppInfoProvision::CacheReadiness::Invalid ||
			 readiness == AppInfoProvision::CacheReadiness::Busy ||
			 readiness == AppInfoProvision::CacheReadiness::Unverified);
	}

	inline bool rawCacheItemShouldReplace(
		std::uint64_t pendingGeneration, std::uint32_t pendingChange,
		std::uint64_t incomingGeneration, std::uint32_t incomingChange) noexcept
	{
		if (incomingGeneration != pendingGeneration)
			return incomingGeneration > pendingGeneration;
		return incomingChange >= pendingChange;
	}

	inline bool rawCacheWorkerShouldContinueAfterDeferral(
		bool hadConcurrentPending) noexcept
	{
		// A deferred-only batch waits for a later event instead of spinning on a
		// persistent failure. An arrival queued while the batch was running must
		// be drained by the current worker so it cannot become stranded.
		return hadConcurrentPending;
	}

	inline std::vector<AppInfoProvision::RefreshRequest>
	excludeRefreshRequestsForApps(
	    const std::vector<AppInfoProvision::RefreshRequest>& requests,
	    const std::unordered_set<uint32_t>& excluded)
	{
		std::vector<AppInfoProvision::RefreshRequest> pending;
		pending.reserve(requests.size());
		for (const auto& request : requests)
			if (excluded.count(request.appId) == 0) pending.push_back(request);
		return pending;
	}

	inline std::vector<AppInfoProvision::RefreshRequest>
	markRuntimePublicationForSuppressedApps(
		const std::vector<AppInfoProvision::RefreshRequest>& requests,
		const std::unordered_set<uint32_t>& suppressed)
	{
		auto tagged = requests;
		for (auto& request : tagged)
			request.publishRuntime = request.publishRuntime ||
				suppressed.count(request.appId) != 0;
		return tagged;
	}

}
