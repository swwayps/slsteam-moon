#include "stats_policy.hpp"

#include "packagepatch.hpp"
#include "../config.hpp"
#include "../globals.hpp"
#include "../sdk/CAppOwnershipInfo.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/CSteamEngine.hpp"

namespace
{
StatsPolicy::Store state;
}

void StatsPolicy::setAccount(uint32_t id) { state.setAccount(id); }
void StatsPolicy::invalidate() { state.invalidate(); }
uint32_t StatsPolicy::account() { return state.account(); }
StatsPolicy::Context StatsPolicy::context() { return state.context(); }
void StatsPolicy::observe(Context context, uint32_t app, bool success, const CAppOwnershipInfo* info)
{
	if (!info || !g_config.isAddedAppId(app)) return;
	state.observe(context.account, app, success, info->subId,
	              info->ownsLicense, info->licenseExpired, context.epoch);
}

uint64_t StatsPolicy::localEpoch(uint32_t app, uint32_t id, bool refresh)
{
	if (!id || id != state.account() || !g_config.isAddedAppId(app) ||
	    g_config.shouldExcludeAppId(app)) return 0;
	if (refresh)
	{
		const auto context = state.context();
		if (context.account != id) return 0;
		auto* user = getLocalUser();
		if (!user) return 0;
		CAppOwnershipInfo info{};
		info.subId = -1;
		const bool success = user->checkAppOwnership(app, &info);
		state.observe(id, app, success, info.subId, info.ownsLicense,
		              info.licenseExpired, context.epoch);
	}
	// Package 0 may contain real free games too. Membership must have been
	// appended by us, not merely already present in Steam's original vector.
	if (!PackagePatch::isInjectedAppId(app)) return 0;
	return state.localEpoch(id, app);
}

extern "C" uint64_t slsteam_local_stats_epoch_v1(uint32_t app, uint32_t id,
                                                uint32_t refresh) noexcept
{
	try { return StatsPolicy::localEpoch(app, id, refresh == 1); }
	catch (...) { return 0; } // No C++ exceptions may cross linker namespaces.
}
