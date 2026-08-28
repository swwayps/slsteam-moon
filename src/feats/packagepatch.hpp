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
#include <vector>

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

	// Manual injection entry point.  Steam normally calls LoadPackage
	// for package 0 once, very early at boot, after the cached
	// packageinfo.vdf is read from disk.  If our setup() ran *after*
	// that call (e.g. config parser loaded extra appids late), this
	// reinjects them.  Idempotent against the same id-set.
	bool injectIntoPackage0(const std::vector<uint32_t>& appIds);

	// Register planner appids beyond the managed base-app source union
	// that the LoadPackage detour must also inject into package 0 on
	// every load.  Used for DLC appids discovered from each AddedApp's
	// provisioned appinfo: Steam's install planner only schedules a
	// `dlcappid`-tagged depot when that DLC's appid is in package 0's
	// AppIdVec.  Registering them here (rather than only doing a one-shot
	// manual inject) keeps them present across package-0 reloads — e.g.
	// the reload the license reconcile triggers — the same way the
	// managed app ids are kept.  Merged with the managed source union at inject
	// time; deduplicated.  Replaces any previously-set extra list.
	void setExtraAppIds(const std::vector<uint32_t>& appIds);

	// Retry the one-shot post-injection license reconcile.  Called from
	// a hook that reliably has a valid local user (CheckAppOwnership) so
	// the LicensesUpdated_t broadcast can happen even when the package-0
	// injection occurred before the engine user map was populated (cold
	// cache).  Cheap no-op once the broadcast has fired or before
	// anything has been injected.
	void tryReconcileLicenses();

	// Legacy hot-add entry point kept until the complete-snapshot watcher owns
	// dispatch. This re-arms the one-shot reconcile
	// gate and re-broadcasts LicensesUpdated_t so Steam re-reads ownership.
	// Safe no-op if package 0 was never injected or the pattern didn't
	// resolve.
	void forceReconcileLicenses();

	// Reconcile package 0 to one complete managed-state snapshot.  The caller
	// is the OwnerWork command runner, so vector mutation and the mark/process
	// pair execute as one serialized Steam-owned work unit.
	void synchronizePackage0(const PackageSnapshot& snapshot);

	// Full runtime-add capability: mark/process locators plus the scoped
	// unresolved-app guard.  A metadata-complete snapshot can still be
	// processed when the guard is unavailable; synchronizePackage0 decides that
	// per transition.
	bool runtimeRefreshReady();

	// Lock-free owner-frame hint for a generation whose mark/process work was
	// deferred or lost a non-blocking execution claim.
	bool runtimeRefreshPending() noexcept;

	// Retry the latest applied desired state without mutating package vectors.
	// OwnerWork invokes this after consuming the lock-free appinfo-resolution
	// signal on the owner frame.
	void reprocessCurrentState() noexcept;
}
