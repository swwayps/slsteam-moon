// SPDX-License-Identifier: AGPL-3.0-only
//
// ReconcilePin — the downgrade-loop fix.
//
// Hooks CDepotDownloadMgr::EvaluateConfigChanges (entry VA 0xfe425a on build
// cfe99f0c) — the post-commit reconcile that diffs an app's ACTIVE (installed)
// depot config against its TARGET (appinfo) config and emits "config changed :
// updated depots" / "Update Required".  It is invoked once per side; the ctx's
// flag bit 0x8 marks the TARGET/appinfo side (confirmed live: active call
// carried the installed gid, target call carried the public appinfo gid,
// matching content_log "active: ... target: ...").
//
// The fix has TWO cooperating parts on the TARGET side. Managed size-zero
// depots are removed wherever they appear; gid rewriting remains restricted
// to LOCKED apps:
//
//  (1) ctx-vector patch: on the TARGET call (flags & 0x8), remove managed
//      size-zero depots and rewrite remaining ManifestGids to configured pins
//      in the ctx vector (ptr @ ctx+0x78, count @ ctx+0x84, stride 0x20).
//
//  (2) target-local patch: EvaluateConfigChanges also compares against an
//      appinfo-derived TARGET CUtlVector built as a function LOCAL at
//      -0x90(ebp).  For some apps the divergent (still-public) gid lives THERE,
//      not in [ctx+0x78] (live trace, app 3525970: ctx vector already held the
//      pin, the local held public -> mismatch -> perpetual "updated depots"
//      loop while installing).  An entry hook can't reach a not-yet-built
//      local, so we hook the shared appinfo->depot-vector builder that fills it
//      (derived from the EvaluateConfigChanges call site) and patch the local
//      AFTER population — but ONLY when the return address is this function's
//      call site (the builder has 6 callers; patching its output globally would
//      contaminate the chunk-diff baseline path). The local is always the
//      appinfo/desired side, so filtering and pinning it cannot hide ACTIVE
//      state.
//
// Together: a size-zero depot removed from the final plan is also absent from
// reconcile's desired set, preventing a zero-file update loop. A still-public
// install (active=public != target=pin) triggers the one downgrade, and
// afterwards (active=pin == target=pin) the reconcile finds
// "no changes" -> the perpetual "Update Required" loop never starts.  Patching
// the TARGET side in memory is downstream of appinfo, so an in-session
// RequestAppInfoUpdate cannot defeat it (unlike the appinfo.vdf provision pin).
// The active side is left untouched so a real pending downgrade is never masked.
//
// Calling convention (REd): a global anchor/manager pointer arrives in EAX; the
// three args are on the stack (ctx = arg0 @ ebp+0x8).  Modelled with a
// regparm(1) detour; the prologue is a plain `push ebp` (no get_pc_thunk), so
// no fixPICThunkCall is needed.
//
// Gating: the empty-depot filter is always active and only recognizes depots
// imported as managed. The gid rewrite acts only for locked apps and honours
// SLSSTEAM_RECONCILE_PIN. Optional per-call diagnostic logging is gated on
// SLSSTEAM_RECONCILE_TRACE.

#pragma once

namespace ReconcilePin
{
	// Resolve EvaluateConfigChanges and install the detour. Returns false when
	// the pattern or hook fails to resolve.
	bool setup();

	// Tear the detour back down (called from Hooks::remove).
	void remove();
}
