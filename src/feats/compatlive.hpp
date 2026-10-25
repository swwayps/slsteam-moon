// SPDX-License-Identifier: AGPL-3.0-only
// Pure policy for live Steam compatibility mappings.
#pragma once

#include "../sdk/IClientCompat.hpp"

#include <cstdint>

namespace CompatLive
{
inline constexpr int kMappingPriority = 250;
inline constexpr std::uint32_t kMaxPollAttempts = 20;

enum class StepStatus : std::uint8_t
{
	Unavailable,
	Ready,
	Requested,
};

struct StepResult
{
	StepStatus status = StepStatus::Unavailable;
};

// Specifies the mapping whenever none resolves: a retry follows a missing or
// faulting compat manager, so no earlier request can be relied on. Repeating
// the same tool and priority is the same write.
inline StepResult step(IClientCompat* compat, std::uint32_t appId) noexcept
{
	if (compat == nullptr || appId == 0)
		return {StepStatus::Unavailable};
	try
	{
		const char* current = compat->getCompatToolName(appId);
		if (current != nullptr && current[0] != '\0')
			return {StepStatus::Ready};

		const char* selected = compat->getCompatToolName(0);
		if (selected == nullptr || selected[0] == '\0')
			selected = "proton_experimental";
		compat->specifyCompatTool(appId, selected, "", kMappingPriority);
		current = compat->getCompatToolName(appId);
		if (current != nullptr && current[0] != '\0')
			return {StepStatus::Ready};
		return {StepStatus::Requested};
	}
	catch (...)
	{
		return {StepStatus::Unavailable};
	}
}
}
