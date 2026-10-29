#pragma once

#include <cstdint>

namespace AppInfoStatePolicy
{
enum class Action : std::uint8_t
{
	None,
	MarkSkip,
	SignalResolved,
	ClearSkipAndSignalResolved,
};

// Steam's install lookup treats a set skip byte as missing app configuration,
// so the byte is only held while a managed app's appinfo is unresolved.
constexpr Action decide(
	bool managed,
	bool create,
	bool shaEmpty,
	bool skipSet
) noexcept
{
	if (!managed || create)
		return Action::None;

	if (!shaEmpty)
		return skipSet ? Action::ClearSkipAndSignalResolved
		               : Action::SignalResolved;

	return skipSet ? Action::None : Action::MarkSkip;
}
}
