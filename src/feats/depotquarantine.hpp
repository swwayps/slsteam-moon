// SPDX-License-Identifier: AGPL-3.0-only
//
// In-memory policy for quarantining a managed DLC depot after Steam reports
// repeated definitive chunk-unpack failures. The quarantine is bound to the
// exact key bytes that failed: replacing or withdrawing that key releases the
// depot automatically. Nothing is persisted and this policy performs no I/O.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace DepotQuarantine
{
	// Internal CDepotReconstruct work-result value used for the path that
	// formats "Unpack failed (c:...,u:...,r:...,b:...)".
	inline constexpr uint32_t kUnpackFailed = 4;
	inline constexpr uint32_t kDownloadedChunkLocation = 4;
	inline constexpr std::size_t kDistinctFailureThreshold = 3;

	inline constexpr bool isTargetFailure(uint32_t location,
	                                      uint32_t unpackResult)
	{
		return location == kDownloadedChunkLocation
		    && unpackResult == kUnpackFailed;
	}

	class Registry
	{
	public:
		// Returns true exactly once, when enough distinct chunks have failed to
		// promote this depot from a candidate to a confirmed quarantine.
		bool markFailure(uint32_t depotId, uint32_t appId, bool inManagedScope,
		                 const std::string& key, uint32_t unpackResult,
		                 uint64_t chunkFingerprint)
		{
			if (!depotId || !appId || !inManagedScope || key.size() != 32
			    || unpackResult != kUnpackFailed || !chunkFingerprint)
			{
				return false;
			}

			std::lock_guard<std::mutex> lock(m_lock);
			auto [it, inserted] = m_records.try_emplace(
			    depotId, Record{appId, key, {}, false});
			if (!inserted
			    && (it->second.appId != appId || it->second.key != key))
			{
				it->second = Record{appId, key, {}, false};
			}

			Record& record = it->second;
			if (record.quarantined) return false;
			record.failedChunks.insert(chunkFingerprint);
			if (record.failedChunks.size() < kDistinctFailureThreshold)
			{
				return false;
			}
			record.quarantined = true;
			return true;
		}

		// Restore a quarantine that a previous session already confirmed, so
		// the very first plan of this session omits the depot instead of
		// re-downloading its undecryptable chunks. Returns true when this call
		// established a new quarantine.
		bool adopt(uint32_t depotId, uint32_t appId, const std::string& key)
		{
			if (!depotId || !appId || key.size() != 32) return false;

			std::lock_guard<std::mutex> lock(m_lock);
			auto [it, inserted] = m_records.try_emplace(
			    depotId, Record{appId, key, {}, true});
			if (inserted) return true;
			if (it->second.quarantined && it->second.appId == appId
			    && it->second.key == key)
			{
				return false;
			}
			it->second = Record{appId, key, {}, true};
			return true;
		}

		// Fast lookup used before consulting the key cache in the planner.
		bool contains(uint32_t depotId) const
		{
			std::lock_guard<std::mutex> lock(m_lock);
			auto it = m_records.find(depotId);
			return it != m_records.end() && it->second.quarantined;
		}

		bool shouldDrop(uint32_t depotId, uint32_t dlcAppId,
		                bool inManagedScope, const std::string& currentKey)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			auto it = m_records.find(depotId);
			if (it == m_records.end() || !it->second.quarantined) return false;

			if (!inManagedScope || currentKey.size() != 32
			    || currentKey != it->second.key)
			{
				m_records.erase(it);
				return false;
			}

			// DepotEntry::DlcAppId is Steam's structured classification. A zero
			// value is base/shared content and is never removed automatically.
			return dlcAppId != 0;
		}

	private:
		struct Record
		{
			uint32_t appId;
			std::string key;
			std::unordered_set<uint64_t> failedChunks;
			bool quarantined;
		};

		mutable std::mutex m_lock;
		std::unordered_map<uint32_t, Record> m_records;
	};

	// Runtime bridge: optional steamclient callbacks feed the registry, while
	// BuildDepotDependency asks whether a specific structured DLC entry should
	// be omitted on the next plan.
	bool setup();
	void remove();
	bool shouldDropManagedDlc(uint32_t depotId, uint32_t dlcAppId);

	// Ids that must stay OUT of package 0: the DLC appids confirmed
	// undecryptable and their content depots.  Steam's desired configuration
	// is rebuilt from package 0, so omitting them there is what stops the
	// commit -> "config changed : added depots <dlc>" -> re-plan loop (and the
	// CDN-source burn that loop inflicted on unrelated apps).  Only records
	// whose key fingerprint still matches the key in use are reported, so a
	// refreshed stplug-in key releases the DLC automatically.
	std::unordered_set<uint32_t> package0Exclusions();
}
