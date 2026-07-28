// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure decision for "must Steam's cloud be reported disabled for this app?".
//
// Cloud sync is doomed for an app whose ownership we fabricate: Valve validates
// ownership server-side and answers Access Denied (visible in cloud_log.txt), so
// Steam only surfaces a cloud error to the user. Those apps must report cloud
// disabled.
//
// The dangerous input is Steam's own ownership answer for everything else. It is
// unreliable while the client rebuilds its license set — a rebuild our package-0
// reconcile itself triggers — and a false "not owned" there silently turns cloud
// saves off for a genuinely owned game for the rest of the session. So it is
// consulted only when global unlocking is on, which is the only configuration
// where that answer identifies an app we make playable.
//
// Kept free of Steam/SDK deps so it can be unit-tested with a stock g++.

#pragma once

namespace Apps
{

inline bool cloudDisableDecision(bool disableCloudEnabled,
                                 bool managedApp,
                                 bool playNotOwnedGames,
                                 bool steamReportsOwned)
{
	if (!disableCloudEnabled)
	{
		return false;
	}
	if (managedApp)
	{
		return true;
	}
	if (!playNotOwnedGames)
	{
		return false;
	}
	return !steamReportsOwned;
}

} // namespace Apps
