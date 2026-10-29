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
// so the byte is only held while appinfo is unresolved. Authoritative records
// stay protected by the outgoing PICS changelist filter instead.
constexpr Action decide(
	bool managed,
	[[maybe_unused]] bool authoritative,
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
