// SPDX-License-Identifier: AGPL-3.0-only

#include "managed_dlc_scope.hpp"

#include "apps.hpp"

namespace
{
ManagedDlcScope::Store g_managedDlcScope;
}

void Apps::setDiscoveredAppDlcIds(
	const std::vector<std::uint32_t>& dlcIds)
{
	g_managedDlcScope.setDiscovered(dlcIds);
}

void Apps::setConfiguredAppDlcIds(
	const std::vector<std::uint32_t>& dlcIds)
{
	g_managedDlcScope.setConfigured(dlcIds);
}

bool Apps::isAddedAppDlcId(std::uint32_t appId)
{
	return g_managedDlcScope.contains(appId);
}
