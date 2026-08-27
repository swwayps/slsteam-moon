// SPDX-License-Identifier: AGPL-3.0-only
//
// Small pure lazy-index primitive used by DepotKey's on-disk catalog.

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace DepotKey
{
	template<typename Key, typename Value>
	class LazyIndex
	{
	public:
		template<typename Loader>
		void loadOnce(Loader&& loader)
		{
			if (loaded_) return;
			std::map<Key, Value> loaded;
			std::forward<Loader>(loader)(loaded);
			entries_ = std::move(loaded);
			loaded_ = true;
		}

		Value* find(const Key& key)
		{
			auto it = entries_.find(key);
			return it == entries_.end() ? nullptr : &it->second;
		}

		const Value* find(const Key& key) const
		{
			auto it = entries_.find(key);
			return it == entries_.end() ? nullptr : &it->second;
		}

		void upsert(const Key& key, Value value)
		{
			entries_[key] = std::move(value);
		}

		const std::map<Key, Value>& entries() const noexcept
		{
			return entries_;
		}

	private:
		bool loaded_ = false;
		std::map<Key, Value> entries_;
	};

	// Secondary index for the hot "app -> managed depots" lookup. The primary
	// catalog is keyed by depot id, so scanning it for every app turns a large
	// Lua collection into O(apps * depots) startup work.
	class ManagedDepotIndex
	{
	public:
		template<typename Entries>
		void rebuild(const Entries& entries)
		{
			std::map<std::uint32_t, std::set<std::uint32_t>> rebuilt;
			for (const auto& [depotId, entry] : entries)
			{
				if (entry.managed && entry.appId != 0 && depotId != 0)
					rebuilt[entry.appId].insert(depotId);
			}
			byApp_.swap(rebuilt);
		}

		void replace(std::uint32_t depotId,
		             std::uint32_t oldAppId, bool oldManaged,
		             std::uint32_t newAppId, bool newManaged)
		{
			const bool hadOld = oldManaged && oldAppId != 0 && depotId != 0;
			const bool hasNew = newManaged && newAppId != 0 && depotId != 0;
			if (hadOld && hasNew && oldAppId == newAppId)
			{
				byApp_[newAppId].insert(depotId);
				return;
			}
			// Publish the replacement first. If allocation fails, the previous
			// mapping remains intact rather than disappearing from the index.
			if (hasNew) byApp_[newAppId].insert(depotId);
			if (!hadOld) return;
			auto app = byApp_.find(oldAppId);
			if (app == byApp_.end()) return;
			app->second.erase(depotId);
			if (app->second.empty()) byApp_.erase(app);
		}

		std::vector<std::uint32_t> forApp(std::uint32_t appId) const
		{
			const auto app = byApp_.find(appId);
			if (app == byApp_.end()) return {};
			return {app->second.begin(), app->second.end()};
		}

	private:
		std::map<std::uint32_t, std::set<std::uint32_t>> byApp_;
	};
}
