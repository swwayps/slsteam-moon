// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace DlcMetadata
{
struct MetadataApp
{
	std::uint32_t appid = 0;
	std::uint32_t changeNumber = 0;
	std::string sha;
	std::string wireBuffer;

	bool operator==(const MetadataApp&) const = default;
};

struct CacheRecord
{
	std::uint32_t baseAppId = 0;
	std::uint64_t baseGeneration = 0;
	std::uint32_t baseChangeNumber = 0;
	std::string baseSha;
	std::vector<MetadataApp> apps;
	std::vector<std::uint32_t> rejectedAppIds;

	bool operator==(const CacheRecord&) const = default;
};

inline void appendChildAppIds(
	const CacheRecord& record,
	std::unordered_set<std::uint32_t>& appIds)
{
	for (const auto& app : record.apps)
		if (app.appid != 0) appIds.insert(app.appid);
}

// Runtime readers must match the exact managed-app generation.  A process
// starts at generation zero, so only that state may reuse a sidecar written
// by an earlier process after the base cache identity has been validated.
inline bool cacheGenerationMatches(
	std::uint64_t recordGeneration,
	std::uint64_t expectedGeneration,
	std::uint64_t currentGeneration) noexcept
{
	if (expectedGeneration != 0)
		return recordGeneration == expectedGeneration &&
			currentGeneration == expectedGeneration;
	return currentGeneration == 0 || recordGeneration == currentGeneration;
}

inline bool baseIdentityMatches(
	std::uint32_t recordChangeNumber,
	const std::string& recordSha,
	std::uint32_t currentChangeNumber,
	const std::string& currentSha) noexcept
{
	return recordChangeNumber == currentChangeNumber &&
		recordSha.size() == 20 && currentSha.size() == 20 &&
		recordSha == currentSha;
}

inline bool publicationMatchesBase(
	std::uint64_t recordGeneration,
	std::uint64_t expectedGeneration,
	std::uint64_t currentGeneration,
	std::uint32_t recordChangeNumber,
	const std::string& recordSha,
	std::uint32_t currentChangeNumber,
	const std::string& currentSha) noexcept
{
	return cacheGenerationMatches(
			recordGeneration, expectedGeneration, currentGeneration) &&
		baseIdentityMatches(
			recordChangeNumber, recordSha, currentChangeNumber, currentSha);
}

// Validate one anonymous-CM record as a child of `baseAppId` and render the
// metadata-only wire stored in Steam's appinfo cache.  The output contains
// identity/common/extended data only; content, launch and manifest topology
// never cross this boundary.
bool normalize(
	const std::string& cmWire,
	std::uint32_t requestedDlcAppId,
	std::uint32_t baseAppId,
	std::string& normalizedWire) noexcept;

bool encodeCache(const CacheRecord& record, std::string& output) noexcept;
bool decodeCache(const std::string& input, CacheRecord& record) noexcept;
}
