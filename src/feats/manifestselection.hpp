// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure manifest selection policy shared by the Steam download hooks and
// standalone tests. No filesystem, network, or Steam dependencies.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace ManifestSelection
{
	enum class ExactState
	{
		Ready,
		Pending,
		Unavailable,
	};

	enum class ChoiceSource
	{
		Exact,
		PreferredLocal,
		LegacyLocal,
		ExactUnavailable,
	};

	struct Decision
	{
		uint64_t gid;
		ChoiceSource source;
		bool final;
	};

	struct DepotPair
	{
		uint64_t gid;
		uint64_t size;
		bool pinned;
	};

	// Resolve a pinned target without ever exposing a half-updated depot
	// entry. The caller owns the bounded wait; this policy only retries the
	// size lookup after that wait succeeds and otherwise preserves Steam's
	// original public gid/size pair.
	template<typename SizeLookup, typename AwaitPinned>
	DepotPair resolvePinnedPair(uint64_t publicGid, uint64_t publicSize,
	                            uint64_t pinnedGid, int remainingBudgetMs,
	                            SizeLookup&& installedSize,
	                            AwaitPinned&& awaitPinned)
	{
		if (!pinnedGid) return {publicGid, publicSize, false};

		auto pinnedSize = installedSize();
		if (!pinnedSize && remainingBudgetMs > 0
		    && awaitPinned(remainingBudgetMs))
		{
			pinnedSize = installedSize();
		}
		if (!pinnedSize) return {publicGid, publicSize, false};
		return {pinnedGid, *pinnedSize, true};
	}

	inline Decision choose(uint64_t plannedGid, ExactState exactState,
	                       uint64_t preferredLocalGid,
	                       uint64_t legacyLocalGid)
	{
		if (exactState == ExactState::Ready)
		{
			return {plannedGid, ChoiceSource::Exact, true};
		}
		if (exactState == ExactState::Pending)
		{
			return {plannedGid, ChoiceSource::Exact, false};
		}
		if (preferredLocalGid && preferredLocalGid != plannedGid)
		{
			return {preferredLocalGid, ChoiceSource::PreferredLocal, true};
		}
		if (legacyLocalGid && legacyLocalGid != plannedGid)
		{
			return {legacyLocalGid, ChoiceSource::LegacyLocal, true};
		}
		return {plannedGid, ChoiceSource::ExactUnavailable, true};
	}

	inline int remainingBudgetMs(int totalBudgetMs, int elapsedMs)
	{
		if (totalBudgetMs <= 0 || elapsedMs >= totalBudgetMs) return 0;
		if (elapsedMs <= 0) return totalBudgetMs;
		return totalBudgetMs - elapsedMs;
	}

	// Validate Steam's CUtlVector using its own allocation count instead of an
	// arbitrary depot-count ceiling. The only upper bound is what this process
	// can address without overflowing count * stride.
	inline bool validVectorBounds(int32_t count, int32_t capacity,
	                              std::size_t stride)
	{
		return count > 0 && capacity >= count && stride > 0
		    && static_cast<std::size_t>(count)
		           <= std::numeric_limits<std::size_t>::max() / stride;
	}
}
