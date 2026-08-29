// SPDX-License-Identifier: AGPL-3.0-only
// Pure policy and instruction decoder for live Steam compatibility mappings.
#pragma once

#include "../sdk/IClientCompat.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace CompatLive
{
inline constexpr int kMappingPriority = 250;
inline constexpr std::size_t kMaxManagerOffset = 1024 * 1024;
inline constexpr std::uint32_t kMaxPollAttempts = 20;

enum class StepStatus : std::uint8_t
{
	Unavailable,
	Ready,
	Requested,
	Waiting,
};

struct StepResult
{
	StepStatus status = StepStatus::Unavailable;
};

inline std::optional<std::size_t> decodeManagerOffset(
	std::span<const std::uint8_t> instruction) noexcept
{
	if (instruction.size() < 6 || instruction[0] != 0x8d ||
		instruction[1] != 0x9e)
	{
		return std::nullopt;
	}
	const std::uint32_t offset =
		static_cast<std::uint32_t>(instruction[2]) |
		(static_cast<std::uint32_t>(instruction[3]) << 8) |
		(static_cast<std::uint32_t>(instruction[4]) << 16) |
		(static_cast<std::uint32_t>(instruction[5]) << 24);
	if (offset == 0 || offset > kMaxManagerOffset)
		return std::nullopt;
	return static_cast<std::size_t>(offset);
}

inline StepResult step(
	IClientCompat* compat,
	std::uint32_t appId,
	bool requestAlreadySubmitted) noexcept
{
	if (compat == nullptr || appId == 0)
		return {StepStatus::Unavailable};
	try
	{
		const char* current = compat->getCompatToolName(appId);
		if (current != nullptr && current[0] != '\0')
			return {StepStatus::Ready};
		if (requestAlreadySubmitted)
			return {StepStatus::Waiting};

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
