#pragma once

#include <cstdint>

namespace AppInfoStatePolicy
{
enum class Action : std::uint8_t
{
	None,
	MarkSkip,
	SignalResolved,
	MarkSkipAndSignalResolved,
};

constexpr Action decide(
	bool managed,
	bool authoritative,
	bool create,
	bool shaEmpty,
	bool skipSet
) noexcept
{
	if (!managed || create)
		return Action::None;

	if (!shaEmpty)
	{
		if (authoritative && !skipSet)
			return Action::MarkSkipAndSignalResolved;
		return Action::SignalResolved;
	}

	return skipSet ? Action::None : Action::MarkSkip;
}
}
