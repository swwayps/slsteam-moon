// SPDX-License-Identifier: AGPL-3.0-only
//
// Package patch feature.
//
// Hooks Steam's LoadPackage so that, when Steam loads PackageId 0
// (the default anonymous package every account has), our
// managed app ids from stplug-in/luaappids get appended to
// `pInfo->AppIdVec` via the resolved `CUtlMemoryGrow` helper.
//
// Why: Steam's depot eligibility filter walks every package and
// looks for an appid in `AppIdVec`. Without an entry in package 0
// the filter can return "no eligible depots" — the install dialog
// then shows 0 B and Steam declares the app installed without
// downloading anything. Adding the appid to package 0 makes the
// filter return the real depot list, the install size becomes
// correct, and existing depot-key + manifest paths finish the
// pipeline.

#pragma once

#include "hotreload_types.hpp"

#include <cstdint>

namespace PackagePatch
{
	// True only for IDs appended by this process (not native package-0 IDs).
	bool isInjectedAppId(uint32_t appId);
	// Set up the LoadPackage detour and resolve CUtlMemoryGrow.  Returns
	// false if either pattern failed to resolve; in that case the hook
	// is not installed and the feature is a no-op.
	bool setup();

	// Tear the hooks back down (called from Hooks::remove).
	void remove();

	// Retry the pending license broadcast (boot, then once per unprocessed boot
	// generation) from CheckAppOwnership, which has a local user on a cold cache.
	void tryReconcileLicenses();

	// Reconcile package 0 to one complete managed-state snapshot.  The caller
	// is the OwnerWork command runner, so vector mutation and the mark/process
	// pair execute as one serialized Steam-owned work unit.
	void synchronizePackage0(const PackageSnapshot& snapshot);


	// Lock-free owner-frame hint for a generation whose mark/process work was
	// deferred or lost a non-blocking execution claim.
	bool runtimeRefreshPending() noexcept;

	// Retry the latest applied desired state without mutating package vectors.
	// OwnerWork invokes this after consuming the lock-free appinfo-resolution
	// signal on the owner frame.
	void reprocessCurrentState() noexcept;
}
