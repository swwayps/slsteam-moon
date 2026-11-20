// SPDX-License-Identifier: AGPL-3.0-only
//
// The ReconcilePin hooks patch Steam's plan on its content thread, next to
// their one call of Steam's function. Whatever the patch throws stays here.
#pragma once

namespace ReconcilePin::detail
{
	// Runs `patch`; if it throws, `note` records the failure instead.
	template <typename Patch, typename Note>
	void runPatched(Patch&& patch, Note&& note) noexcept
	{
		try
		{
			patch();
		}
		catch (...)
		{
			note();
		}
	}
}
