// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure capability gate for a runtime package-0 refresh.
#pragma once

namespace LicenseRefreshPolicy
{
enum class Action
{
	NoChange,
	Defer,
	Process,
	// Mark/process did not resolve on this client: nudge Steam with the
	// one-shot LicensesUpdated_t broadcast instead of waiting for a restart.
	Broadcast,
};

constexpr bool unresolvedStateSafe(
	bool hasPendingUnresolvedAddition,
	bool guardReady
) noexcept
{
	return !hasPendingUnresolvedAddition || guardReady;
}

constexpr Action decide(
	bool markReady,
	bool processReady,
	bool unresolvedStateSafe,
	bool changed
) noexcept
{
	if (!changed)
		return Action::NoChange;
	if (!unresolvedStateSafe)
		return Action::Defer;
	if (!markReady || !processReady)
		return Action::Broadcast;
	return Action::Process;
}
}
