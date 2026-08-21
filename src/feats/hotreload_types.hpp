// SPDX-License-Identifier: AGPL-3.0-only
//
// Immutable-value inputs shared by the hot-reload owner-thread handoff.
#pragma once

#include <cstdint>
#include <vector>

struct PackageSnapshot
{
	std::uint64_t generation = 0;
	std::vector<std::uint32_t> appIds;
	std::vector<std::uint32_t> depotIds;
	bool metadataComplete = false;
	// Managed base apps introduced by this coordinator generation. This stays
	// explicit when owner-queue coalescing means the live package vector already
	// contains an id from an older generation.
	std::vector<std::uint32_t> addedAppIds;
	// Appinfo entries that Steam must invalidate after the corresponding
	// ownership topology is installed. Unlike addedAppIds, this also carries
	// planner children discovered by a later metadata generation.
	std::vector<std::uint32_t> appInfoRequestIds;

	bool operator==(const PackageSnapshot&) const = default;
};
