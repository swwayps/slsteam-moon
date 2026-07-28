// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure in-place filter for Steam's CUtlVector<DepotEntry> storage.  Both the
// install planner and the post-commit reconciler must apply the same decision
// to managed size-zero depots; otherwise Steam repeatedly re-adds an entry
// that the final plan can never commit.

#pragma once

#include <cstdint>
#include <cstring>

namespace ManagedDepotFilter
{
	constexpr std::size_t kDepotEntryStride = 0x20;
	constexpr std::size_t kDepotIdOff = 0x00;
	constexpr std::size_t kDepotSizeOff = 0x10;

	inline bool shouldDrop(uint64_t size, bool managed)
	{
		return size == 0 && managed;
	}

	// Stable-compacts `count` DepotEntry records and returns the surviving
	// count.  The caller owns the CUtlVector count field and writes the return
	// value there.  Null/empty vectors are left unchanged defensively.
	template <typename IsManaged, typename OnDrop>
	inline int32_t compactEmptyManaged(void* entries, int32_t count,
	                                  IsManaged&& isManaged,
	                                  OnDrop&& onDrop)
	{
		if (!entries || count <= 0) return count;

		auto* const base = static_cast<uint8_t*>(entries);
		int32_t writeIdx = 0;
		for (int32_t readIdx = 0; readIdx < count; ++readIdx)
		{
			auto* const entry =
			    base + static_cast<std::size_t>(readIdx) * kDepotEntryStride;
			uint32_t depotId = 0;
			uint64_t size = 0;
			std::memcpy(&depotId, entry + kDepotIdOff, sizeof(depotId));
			std::memcpy(&size, entry + kDepotSizeOff, sizeof(size));

			if (shouldDrop(size, isManaged(depotId)))
			{
				onDrop(depotId);
				continue;
			}

			if (writeIdx != readIdx)
			{
				auto* const destination =
				    base + static_cast<std::size_t>(writeIdx) * kDepotEntryStride;
				std::memmove(destination, entry, kDepotEntryStride);
			}
			++writeIdx;
		}
		return writeIdx;
	}
}
