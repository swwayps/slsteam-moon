// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace ManagedDlcScope
{
class Store
{
public:
	void setDiscovered(const std::vector<std::uint32_t>& ids)
	{
		setSource(discovered_, ids);
	}

	void setConfigured(const std::vector<std::uint32_t>& ids)
	{
		setSource(configured_, ids);
	}

	bool contains(std::uint32_t appId) const
	{
		if (appId == 0) return false;
		std::lock_guard<std::mutex> lock(mutex_);
		return discovered_.contains(appId) || configured_.contains(appId);
	}

private:
	void setSource(
		std::unordered_set<std::uint32_t>& destination,
		const std::vector<std::uint32_t>& ids)
	{
		std::unordered_set<std::uint32_t> replacement;
		replacement.reserve(ids.size());
		for (const std::uint32_t appId : ids)
			if (appId != 0) replacement.insert(appId);
		std::lock_guard<std::mutex> lock(mutex_);
		destination.swap(replacement);
	}

	mutable std::mutex mutex_;
	std::unordered_set<std::uint32_t> discovered_;
	std::unordered_set<std::uint32_t> configured_;
};
}
