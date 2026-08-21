// SPDX-License-Identifier: AGPL-3.0-only
//
// Watcher-side coordinator for live managed-library state.
#pragma once

#include "hotreload_state.hpp"
#include "provision_refresh.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace HotReload
{
	// Bind the installed appinfo guard to the coordinator's process-lifetime
	// store and publish the first managed-source snapshot.
	void initialize() noexcept;

	// Publish the complete stplug-in/luaappids source union. Equivalent source
	// sets are a no-op; legacy compatibility ids never enter this API.
	void publish(
		const std::unordered_set<std::uint32_t>& managedAppIds,
		bool forceSourceRefresh = false) noexcept;

	// The async base refresh has made a newer metadata topology durable. Rebuild
	// one generation even when managed membership and manifest fingerprints are
	// unchanged. Returns false for stale remove/re-add work or an identical
	// topology.
	bool publishMetadataCompletion(
		std::uint32_t baseAppId,
		std::uint64_t expectedManagedGeneration) noexcept;

	// Called from a real post-login PICS worker callback. Schedules a bounded,
	// cooldown-limited repair for durable child metadata missing from older
	// caches; it never starts work from LD_AUDIT preinit.
	void repairMissingDlcMetadata(
		const std::string& appinfoVdfPath) noexcept;

	// Called only from a real PICS callback. Returns at most one cold-start
	// cache recovery request and applies a retry cooldown without doing I/O.
	std::vector<AppInfoProvision::RefreshRequest>
	takeMissingCacheRepairRequests() noexcept;

	// Mark a migration sidecar durable without changing Steam's live appinfo or
	// package state. It becomes visible naturally on the next cold splice.
	bool noteDlcMetadataCacheCompletion(
		std::uint32_t baseAppId,
		std::uint64_t expectedManagedGeneration) noexcept;

	// Process-lifetime lock-free membership store used by GetOrAddAppData.
	HotReloadState::Store& store() noexcept;

	// Stop accepting watcher publications before Steam hooks are removed.
	void shutdown() noexcept;
}
