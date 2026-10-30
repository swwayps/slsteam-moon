// SPDX-License-Identifier: AGPL-3.0-only
//
// Testable boundary around Steam's CAppInfoCache disk reload.
#pragma once

#include "appdata_layout.hpp"
#include "hotreload_state.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace AppInfoReload
{
using ReadFromDiskFn = bool (*)(void* cache);
using LookupFn = void* (*)(void* cache, std::uint32_t appId, bool create);

struct Runtime
{
	void* cache = nullptr;
	ReadFromDiskFn readFromDisk = nullptr;
	LookupFn lookup = nullptr;
};

enum class Status : std::uint8_t
{
	Unavailable,
	ReadFailed,
	Loaded,
};

struct Result
{
	Status status = Status::Unavailable;
	std::size_t resolved = 0;
	std::size_t present = 0;
};

inline bool allRequestedPresent(
	const Result& result, std::size_t requested) noexcept
{
	return result.status == Status::Loaded && result.present == requested;
}

inline bool hasSha(void* data, const AppDataLayout::Layout& layout) noexcept
{
	if (data == nullptr)
		return false;
	const auto* bytes = static_cast<const std::uint8_t*>(data);
	for (std::size_t index = 0;
		 index < AppDataLayout::detail::kShaBytes; ++index)
	{
		if (bytes[layout.shaOffset + index] != 0)
			return true;
	}
	return false;
}

inline Result reload(
	const Runtime& runtime,
	std::span<const std::uint32_t> appIds,
	HotReloadState::Store& store,
	const AppDataLayout::Layout& layout) noexcept
{
	if (runtime.cache == nullptr || runtime.readFromDisk == nullptr ||
		runtime.lookup == nullptr)
	{
		return {};
	}

	try
	{
		if (!runtime.readFromDisk(runtime.cache))
			return {Status::ReadFailed, 0};

		Result result{Status::Loaded, 0, 0};
		for (const std::uint32_t appId : appIds)
		{
			void* const data = runtime.lookup(runtime.cache, appId, false);
			if (!hasSha(data, layout)) continue;
			++result.present;
			// The lookup goes through the trampoline, so the guard that clears
			// the skip byte it set while the SHA was empty does not run here,
			// and Steam's install lookup refuses a record that keeps it.
			if (store.contains(appId))
				static_cast<std::uint8_t*>(data)[layout.skipOffset] = 0;
			if (store.noteResolved(appId)) ++result.resolved;
		}
		return result;
	}
	catch (...)
	{
		return {Status::ReadFailed, 0};
	}
}
}
