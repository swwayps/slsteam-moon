// SPDX-License-Identifier: AGPL-3.0-only
//
// ReconcilePin — the downgrade-loop fix.
//
// Hooks CDepotDownloadMgr::EvaluateConfigChanges (entry VA 0xfe425a on build
// cfe99f0c) — the post-commit reconcile that diffs an app's ACTIVE (installed)
// depot config against its TARGET (appinfo) config and emits "config changed :
// updated depots" / "Update Required".  It is invoked once per side; the ctx's
// flag bit 0x8 marks the TARGET/appinfo side (the active call carries the
// installed gid, the target call the public appinfo gid, as content_log's
// "active: ... target: ..." lines show).
//
// The fix has TWO cooperating parts on the TARGET side. Managed size-zero
// depots are removed wherever they appear; configured pins are applied to
// every app that owns the depot, independently of the update-lock policy:
//
//  (1) ctx-vector patch: on the TARGET call (flags & 0x8), remove managed
//      size-zero depots and rewrite remaining ManifestGids to configured pins
//      in the ctx vector (CUtlVector, stride 0x20; ctx+0x78 on cfe99f0c,
//      ctx+0x90 on the 2026-10 client — decoded at setup, see
//      findCtxDepotVectorOffset).
//
//  (2) target-local patch: EvaluateConfigChanges also compares against an
//      appinfo-derived TARGET CUtlVector built as a function LOCAL at
//      -0x90(ebp).  For some apps the divergent (still-public) gid lives THERE,
//      not in the ctx vector.  An entry hook can't reach a not-yet-built local, so
//      this function's own call to the builder is redirected to a wrapper that
//      patches the local after it is filled; the builder's other callers (the
//      chunk-diff baseline) stay untouched.  The local is always the
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
// Calling convention (x86-64, 0x10b51b0 on beta 1793235415): (rdi = ctx,
// rsi = CUser, edx = flag); the i386 build received its PIC anchor in EAX on
// top of the same three stack arguments.  The x86-64 context keeps the flags at
// +0x08, the app id at +0x0c and the depot vector at +0xc0 (decoded at setup).
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
		// EvaluateConfigChanges entry (x86-64 betas 1792711043 and 1793235415
		// and the 2026-10-21 stable client; i386 cfe99f0c had +0x183).
		inline constexpr std::size_t kBuilderCallOff = 0x19e;
		// How far to look on either side of kBuilderCallOff when the function
		// body was recompiled and the call moved.
		inline constexpr std::size_t kBuilderCallWindow = 0x40;

		// Locate the `call rel32` (opcode 0xE8) to the target-vector builder.
		//
		// The exact offset drifts when Steam recompiles EvaluateConfigChanges
		// (the byte at kBuilderCallOff may not be the call), so instead of
		// giving up on any drift this searches around it.
		// `code`/`len` is the readable function prologue, `codeAddr` its runtime
		// base (to decode the near-call target), and `targetOk(addr)` accepts
		// a decoded absolute target.
		//
		// Returns the offset of the call, -1 when no call qualifies, or -2 when
		// more than one does. Either failure keeps the target-local fix off:
		// a guessed-address hook could corrupt the reconcile.
		template <typename TargetOk>
		inline long findBuilderCallRel32(
			const std::uint8_t* code, std::size_t len, std::uintptr_t codeAddr,
			std::size_t expectedOff, std::size_t window, TargetOk targetOk)
		{
			// On x86-64 the builder's eight arguments travel in registers and
			// two stack slots the caller pops later, so no cleanup right after
			// the call marks it; the decoded target alone decides.
			const auto callTargetOk = [&](std::size_t off) -> bool
			{
				if (off + 5 > len) return false;
				if (code[off] != 0xE8) return false;
				std::int32_t rel = 0;
				std::memcpy(&rel, code + off + 1, sizeof(rel));
				const std::uintptr_t target = codeAddr + off + 5 +
					static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
				return targetOk(target);
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
				if (found != -1) return -2;         // ambiguous
				found = static_cast<long>(off);
			}
			return found;
		}

		// The builder is CUserAppManager::BuildDepotDependency; only a call to
		// it qualifies. Without that locator (`builder` == 0) nothing tells the
		// builder call from its neighbours, so no call is redirected.
		inline long findBuilderCall(
			const std::uint8_t* code, std::size_t len, std::uintptr_t codeAddr,
			std::uintptr_t builder)
		{
			if (builder == 0) return -1;
			return findBuilderCallRel32(
				code, len, codeAddr, kBuilderCallOff, kBuilderCallWindow,
				[builder](std::uintptr_t target) { return target == builder; });
		}

		// Largest context offset accepted for the depot vector; the struct is
		// a few hundred bytes, so anything beyond this is a mis-decode.
		inline constexpr std::size_t kMaxCtxDepotVectorOff = 0x1000;
		// Bytes of EvaluateConfigChanges searched for the depot-vector loop
		// (+0x318 on the x86-64 clients).
		inline constexpr std::size_t kCtxDepotLoopScanLen = 0x600;

		// Decode the content-update context's DepotEntry vector offset from
		// EvaluateConfigChanges' per-entry loop (rbx = ctx, r15 = index):
		//
		//   mov r11,[rbx+disp] ; mov r14,r15 ; shl r14,5 ; add r14,r11
		//
		// The context grows between client builds (i386 0x78 -> 0x90; 0xc0 on
		// the x86-64 clients), so pinning the offset silently aims the patch at
		// another field.  Accepts the disp8 (4C 8B 5B) and disp32 (4C 8B 9B)
		// forms.  Returns the offset, or -1 when the load is absent, ambiguous,
		// truncated, or implausible, so the caller keeps the patch disabled.
		inline long findCtxDepotVectorOffset(
			const std::uint8_t* code, std::size_t len)
		{
			static constexpr std::uint8_t kTail[] = {
				0x4D, 0x89, 0xFE, 0x49, 0xC1, 0xE6, 0x05, 0x4D, 0x01, 0xDE};
			long found = -1;
			for (std::size_t off = 0; off + 4 <= len; ++off)
			{
				if (code[off] != 0x4C || code[off + 1] != 0x8B) continue;
				std::uint32_t disp = 0;
				std::size_t tail = 0;
				if (code[off + 2] == 0x5B)
				{
					// disp8 is signed: 0x80 and up address below the context.
					if (code[off + 3] >= 0x80) return -1;
					disp = code[off + 3];
					tail = off + 4;
				}
				else if (code[off + 2] == 0x9B && off + 7 <= len)
				{
					std::memcpy(&disp, code + off + 3, sizeof(disp));
					tail = off + 7;
				}
				else
				{
					continue;
				}
				if (tail + sizeof(kTail) > len ||
				    std::memcmp(code + tail, kTail, sizeof(kTail)) != 0)
					continue;
				if (found != -1) return -1;  // ambiguous
				if (disp == 0 || disp % alignof(void*) != 0 ||
				    disp > kMaxCtxDepotVectorOff)
					return -1;
				found = static_cast<long>(disp);
			}
			return found;
		}
	}
}
