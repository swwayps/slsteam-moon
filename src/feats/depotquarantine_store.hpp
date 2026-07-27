// SPDX-License-Identifier: AGPL-3.0-only
//
// Persistent records for DLC depots whose decryption key was proven unusable
// by repeated definitive chunk-unpack failures.
//
// Why persist at all: the in-memory policy (depotquarantine.hpp) only removes
// the depot from the CURRENT install plan.  Steam keeps the DLC in the app's
// desired configuration, so after a successful commit it re-announces
// "config changed : added depots <dlc>", re-plans, retries the same
// undecryptable chunks and burns every CDN source for the whole client (which
// is what made unrelated apps fail with "Content servers unreachable").
// Recording the decision lets the package-0 injection skip that DLC on the
// next load, so it never re-enters the desired configuration.
//
// A record stores a FINGERPRINT of the key, never the key bytes: it exists
// only to detect that the key changed.  When the stplug-in script ships a new
// key the fingerprint stops matching and the DLC is retried automatically.
//
// This header is pure (no Steam/SDK deps, no I/O) so it unit-tests with a
// stock g++, same pattern as feats/dlcids.hpp.

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace DepotQuarantineStore
{
	struct Record
	{
		uint32_t appId = 0;
		uint32_t depotId = 0;
		// Steam's structured DLC classification. Always non-zero here: a zero
		// value means base/shared content, which is never quarantined.
		uint32_t dlcAppId = 0;
		uint64_t keyFingerprint = 0;
	};

	// FNV-1a over the raw 32-byte key. Returns 0 for anything that is not a
	// well-formed key so callers can treat 0 as "no usable fingerprint".
	inline uint64_t fingerprintKey(const std::string& key)
	{
		if (key.size() != 32) return 0;
		uint64_t hash = 14695981039346656037ULL;
		for (unsigned char byte : key)
		{
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		return hash ? hash : 1;
	}

	inline bool isUsable(const Record& record)
	{
		return record.appId && record.depotId && record.dlcAppId
		    && record.keyFingerprint;
	}

	// Insert `record`, replacing any previous entry for the same depot.
	// Returns true when the stored set actually changed.
	inline bool upsert(std::vector<Record>& records, const Record& record)
	{
		if (!isUsable(record)) return false;

		for (auto& existing : records)
		{
			if (existing.depotId != record.depotId) continue;
			if (existing.appId == record.appId
			    && existing.dlcAppId == record.dlcAppId
			    && existing.keyFingerprint == record.keyFingerprint)
			{
				return false;
			}
			existing = record;
			return true;
		}

		records.push_back(record);
		return true;
	}

	// True iff this depot is recorded AND the key it failed with is still the
	// key in use. A changed fingerprint releases the depot.
	inline bool isQuarantined(const std::vector<Record>& records,
	                          uint32_t depotId, uint64_t keyFingerprint)
	{
		if (!depotId || !keyFingerprint) return false;
		for (const auto& record : records)
		{
			if (record.depotId == depotId
			    && record.keyFingerprint == keyFingerprint)
			{
				return true;
			}
		}
		return false;
	}

	// Line-oriented payload: "<appId> <depotId> <dlcAppId> <keyFingerprint>".
	inline std::string serialize(const std::vector<Record>& records)
	{
		std::ostringstream out;
		for (const auto& record : records)
		{
			if (!isUsable(record)) continue;
			out << record.appId << ' ' << record.depotId << ' '
			    << record.dlcAppId << ' ' << record.keyFingerprint << '\n';
		}
		return out.str();
	}

	// Tolerant parser: any line that is not a complete, usable record is
	// dropped, so a truncated or hand-edited file degrades to "fewer
	// quarantines" instead of failing the load.
	inline std::vector<Record> parse(const std::string& text)
	{
		std::vector<Record> records;
		std::istringstream in(text);
		std::string line;
		while (std::getline(in, line))
		{
			std::istringstream fields(line);
			Record record;
			if (!(fields >> record.appId >> record.depotId >> record.dlcAppId
			      >> record.keyFingerprint))
			{
				continue;
			}
			upsert(records, record);
		}
		return records;
	}

	// Order-preserving removal, used to keep quarantined DLC appids and their
	// depots out of the package-0 vectors.
	inline std::vector<uint32_t> withoutIds(
	    const std::vector<uint32_t>& ids,
	    const std::unordered_set<uint32_t>& drop)
	{
		if (drop.empty()) return ids;
		std::vector<uint32_t> kept;
		kept.reserve(ids.size());
		for (uint32_t id : ids)
		{
			if (!drop.count(id)) kept.push_back(id);
		}
		return kept;
	}
}
