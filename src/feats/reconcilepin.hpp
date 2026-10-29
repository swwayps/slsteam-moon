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
//      not in [ctx+0x78].  An entry hook can't reach a not-yet-built local, so
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
		// EvaluateConfigChanges entry on build cfe99f0c.
		inline constexpr std::size_t kBuilderCallOff = 0x183;
		// How far to look on either side of kBuilderCallOff when the function
		// body was recompiled and the call moved.
		inline constexpr std::size_t kBuilderCallWindow = 0x40;

		// Locate the `call rel32` (opcode 0xE8) to the target-vector builder.
		//
		// The exact offset drifts when Steam recompiles EvaluateConfigChanges
		// (the byte at kBuilderCallOff may not be the call), so instead of
		// giving up on any drift this searches around it.
		// `code`/`len` is the readable function prologue, `codeAddr` its runtime
		// base (to decode the near-call target), and `targetExecutable(addr)`
		// reports whether a decoded absolute target lands in executable memory
		// (wired to LM_FindSegment by the caller).
		//
		// Returns the offset of the call, -1 when no call qualifies, or -2 when
		// more than one does. Either failure keeps the target-local fix off:
		// a guessed-address hook could corrupt the reconcile.
		template <typename TargetExecutable>
		inline long findBuilderCallRel32(
			const std::uint8_t* code, std::size_t len, std::uintptr_t codeAddr,
			std::size_t expectedOff, std::size_t window,
			TargetExecutable targetExecutable)
		{
			// The builder takes eight stack dwords (hkBuildTarget's arity), so
			// its caller pops them with `add esp,0x20` straight after the call.
			// That rejects neighbouring calls with other arities, which the
			// 2026-10 client moved into the window.
			const auto callTargetOk = [&](std::size_t off) -> bool
			{
				if (off + 8 > len) return false;
				if (code[off] != 0xE8) return false;
				if (code[off + 5] != 0x83 || code[off + 6] != 0xC4
				    || code[off + 7] != 0x20)
					return false;
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
				if (found != -1) return -2;         // ambiguous
				found = static_cast<long>(off);
			}
			return found;
		}

		// The builder is CUserAppManager::BuildDepotDependency. When that
		// locator resolved (`builder` != 0) only a call to it qualifies,
		// because some builds have a second executable call in the same window.
		template <typename TargetExecutable>
		inline long findBuilderCall(
			const std::uint8_t* code, std::size_t len, std::uintptr_t codeAddr,
			std::uintptr_t builder, TargetExecutable targetExecutable)
		{
			return findBuilderCallRel32(
				code, len, codeAddr, kBuilderCallOff, kBuilderCallWindow,
				[&](std::uintptr_t target) {
					return builder != 0 ? target == builder
					                    : targetExecutable(target);
				});
		}

		// Largest context offset accepted for the depot vector; the struct is
		// a few hundred bytes, so anything beyond this is a mis-decode.
		inline constexpr std::size_t kMaxCtxDepotVectorOff = 0x1000;
		// Bytes of EvaluateConfigChanges searched for the depot-vector loop
		// (it sits near +0x260 on every inspected client).
		inline constexpr std::size_t kCtxDepotLoopScanLen = 0x600;

		// Decode the content-update context's DepotEntry vector offset from
		// EvaluateConfigChanges' per-entry loop:
		//
		//   mov edi,[ebp+8] ; mov eax,esi ; shl eax,5 ; add eax,[edi+disp]
		//
		// The context grows between client builds (0x78 -> 0x90 on the 2026-10
		// client), so pinning the offset silently aims the patch at another
		// field.  Accepts the disp8 (modrm 0x47) and disp32 (modrm 0x87) forms.
		// Returns the offset, or -1 when the load is absent, ambiguous,
		// truncated, or implausible, so the caller keeps the patch disabled.
		inline long findCtxDepotVectorOffset(
			const std::uint8_t* code, std::size_t len)
		{
			static constexpr std::uint8_t kHead[] = {
				0x8B, 0x7D, 0x08, 0x89, 0xF0, 0xC1, 0xE0, 0x05, 0x03};
			long found = -1;
			for (std::size_t off = 0; off + sizeof(kHead) + 2 <= len; ++off)
			{
				if (std::memcmp(code + off, kHead, sizeof(kHead)) != 0) continue;
				const std::size_t modrm = off + sizeof(kHead);
				std::uint32_t disp = 0;
				if (code[modrm] == 0x47)
				{
					disp = code[modrm + 1];
				}
				else if (code[modrm] == 0x87 && modrm + 5 <= len)
				{
					std::memcpy(&disp, code + modrm + 1, sizeof(disp));
				}
				else
				{
					continue;
				}
				if (found != -1) return -1;  // ambiguous
				if (disp == 0 || disp % 4 != 0 || disp > kMaxCtxDepotVectorOff)
					return -1;
				found = static_cast<long>(disp);
			}
			return found;
		}
	}
}
