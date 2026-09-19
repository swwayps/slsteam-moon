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
// depots are removed wherever they appear; configured pins are applied to
// every app that owns the depot, independently of the update-lock policy:
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
// imported as managed. The gid rewrite acts for any configured app-scoped pin
// and honours SLSSTEAM_RECONCILE_PIN. The separate locked-app policy in
// shouldDisableUpdates controls update suppression, not this rewrite. Optional
// per-call diagnostic logging is gated on SLSSTEAM_RECONCILE_TRACE.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ReconcilePin
{
	// Resolve EvaluateConfigChanges and install the detour. Returns false when
	// the pattern or hook fails to resolve.
	bool setup();

	// Tear the detour back down (called from Hooks::remove).
	void remove();

	namespace detail
	{
		// RE'd offset of the target-vector builder `call rel32`, relative to the
		// EvaluateConfigChanges entry (confirmed on build cfe99f0c).
		inline constexpr std::size_t kBuilderCallOff = 0x183;
		// How far to look on either side of kBuilderCallOff when the function
		// body was recompiled and the call moved.
		inline constexpr std::size_t kBuilderCallWindow = 0x40;

		// Locate the `call rel32` (opcode 0xE8) to the target-vector builder.
		//
		// The exact offset drifts when Steam recompiles EvaluateConfigChanges
		// (on the beta client the byte at kBuilderCallOff is no longer the
		// call), so instead of giving up on any drift this searches around it.
		// `code`/`len` is the readable function prologue, `codeAddr` its runtime
		// base (to decode the near-call target), and `targetExecutable(addr)`
		// reports whether a decoded absolute target lands in executable memory
		// (wired to LM_FindSegment by the caller).
		//
		// Returns the offset of the call, or -1 when it cannot be located
		// UNAMBIGUOUSLY. Ambiguity (more than one candidate) or absence yields
		// -1 so the caller keeps the target-local fix disabled — the same safe
		// degradation as before, never a guessed-address hook that could
		// corrupt the reconcile.
		template <typename TargetExecutable>
		inline long findBuilderCallRel32(
			const std::uint8_t* code, std::size_t len, std::uintptr_t codeAddr,
			std::size_t expectedOff, std::size_t window,
			TargetExecutable targetExecutable)
		{
			const auto callTargetOk = [&](std::size_t off) -> bool
			{
				if (off + 5 > len) return false;
				if (code[off] != 0xE8) return false;
				std::int32_t rel = 0;
				std::memcpy(&rel, code + off + 1, sizeof(rel));
				const std::uintptr_t target = codeAddr + off + 5 +
					static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
				return targetExecutable(target);
			};

			// Fast path: the RE'd offset still holds (unambiguous by design).
			if (callTargetOk(expectedOff)) return static_cast<long>(expectedOff);

			// Drift: accept a nearby call ONLY if it is the unique valid one in
			// the window. Two candidates are ambiguous and a wrong hook is worse
			// than none, so give up safely.
			long found = -1;
			const std::size_t lo = expectedOff > window ? expectedOff - window : 0;
			const std::size_t hi = expectedOff + window;
			for (std::size_t off = lo; off <= hi; ++off)
			{
				if (off == expectedOff) continue;  // tried above, it failed
				if (!callTargetOk(off)) continue;
				if (found != -1) return -1;         // ambiguous
				found = static_cast<long>(off);
			}
			return found;
		}
	}
}
