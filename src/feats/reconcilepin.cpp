// SPDX-License-Identifier: AGPL-3.0-only
//
// See reconcilepin.hpp for the design.

#include "reconcilepin.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../patterns.hpp"

#include "depotkey.hpp"
#include "manageddepotfilter.hpp"
#include "manifeststore.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>


namespace
{
	// EvaluateConfigChanges calling convention: a global
	// anchor/manager pointer arrives in EAX (BOTH the manager object the body
	// dereferences AND the PIC anchor for its .rodata string leas); the three
	// real args are on the stack.  regparm(1) models exactly that: arg0 -> EAX,
	// the rest -> stack.  Declaring both the hook and the orig-trampoline
	// pointer regparm(1) keeps EAX intact across the call so the relocated
	// prologue's `mov %eax,-0xb0(%ebp)` reads the right anchor.  (Prologue is a
	// plain `push ebp`, not a get_pc_thunk -> no fixPICThunkCall.)
	typedef void* (__attribute__((regparm(1))) *ReconFn_t)(
	    void* mgr, void* ctx, void* a1, void* a2);

	ReconFn_t    g_orig = nullptr;
	lm_address_t g_addr = LM_ADDRESS_BAD;
	lm_address_t g_tramp = LM_ADDRESS_BAD;
	lm_size_t    g_size = 0;

	bool g_pinActive = false;   // SLSSTEAM_RECONCILE_PIN: perform the gid rewrite
	bool g_trace = false;       // SLSSTEAM_RECONCILE_TRACE: per-call logging
	std::atomic<int> g_traceBudget{400};

	// ctx layout (confirmed live on build cfe99f0c).
	constexpr size_t kCtxFlagsOff = 0x04;
	constexpr size_t kCtxAppIdOff = 0x08;
	constexpr size_t kCtxDepotPtrOff = 0x78;
	constexpr size_t kCtxDepotCountOff = 0x84;
	// flag bit marking the TARGET/appinfo ctx (vs the ACTIVE/installed ctx).
	constexpr uint32_t kFlagTargetSide = 0x8;
	// DepotEntry: DepotId @ +0, ManifestGid @ +0x8, Size @ +0x10,
	// stride 0x20.
	constexpr size_t kDepotEntryStride = ManagedDepotFilter::kDepotEntryStride;
	constexpr size_t kDepotEntryGidOff = 0x08;
	constexpr size_t kDepotEntrySizeOff = ManagedDepotFilter::kDepotSizeOff;
	constexpr int32_t kMaxDepots = 512;

	bool applyPinnedEntry(const char* site, uint32_t appId, uint8_t* entry,
	                      uint32_t depotId, uint64_t pin)
	{
		auto* const gidp =
		    reinterpret_cast<uint64_t*>(entry + kDepotEntryGidOff);
		auto* const sizep =
		    reinterpret_cast<uint64_t*>(entry + kDepotEntrySizeOff);
		const auto pinSize = ManifestStore::installedSize(depotId, pin);
		if (!pinSize)
		{
			g_pLog->debugOnce(
			    "ReconcilePin[%s]: app=%u depot=%u pinned gid=%llu has no "
			    "known size; leaving target gid=%llu size=%llu\n",
			    site, appId, depotId, static_cast<unsigned long long>(pin),
			    static_cast<unsigned long long>(*gidp),
			    static_cast<unsigned long long>(*sizep));
			return false;
		}
		if (*gidp != pin || *sizep != *pinSize)
		{
			g_pLog->info(
			    "ReconcilePin[%s]: app=%u depot=%u target gid=%llu size=%llu "
			    "-> pinned gid=%llu size=%llu\n",
			    site, appId, depotId, static_cast<unsigned long long>(*gidp),
			    static_cast<unsigned long long>(*sizep),
			    static_cast<unsigned long long>(pin),
			    static_cast<unsigned long long>(*pinSize));
		}
		*gidp = pin;
		*sizep = *pinSize;
		return true;
	}

	void traceLog(uint32_t appId, uint32_t flags, void* base, int32_t count)
	{
		if (!g_trace) return;
		const bool added = g_config.isAddedAppId(appId);
		const bool locked = g_config.isAppLocked(appId);
		// Locked apps are the ones we're debugging: never let the global
		// budget (exhausted by the startup batch of all apps) hide their
		// reconcile calls, especially mid-loop.  Non-locked apps still
		// respect the budget so the log doesn't flood.
		if (!locked && g_traceBudget.fetch_sub(1) <= 0) return;
		g_pLog->info("ReconcilePin[trace]: app=%u flags=0x%x added=%d locked=%d "
		             "side=%s depots@%p count=%d\n",
		             appId, flags, static_cast<int>(added),
		             static_cast<int>(locked),
		             (flags & kFlagTargetSide) ? "target" : "active",
		             base, count);
		if (base && count > 0 && count <= kMaxDepots)
		{
			const auto* e = reinterpret_cast<const uint8_t*>(base);
			for (int32_t i = 0; i < count; ++i, e += kDepotEntryStride)
			{
				g_pLog->info("ReconcilePin[trace]:   depot=%u gid=%llu pin=%llu\n",
				    *reinterpret_cast<const uint32_t*>(e),
				    static_cast<unsigned long long>(
				        *reinterpret_cast<const uint64_t*>(e + kDepotEntryGidOff)),
				    static_cast<unsigned long long>(
				        g_config.getManifestPin(appId,
				            *reinterpret_cast<const uint32_t*>(e))));
			}
		}
	}

	int32_t filterEmptyManaged(uint8_t* base, int32_t count,
	                           uint32_t appId, const char* side)
	{
		return ManagedDepotFilter::compactEmptyManaged(
		    base, count,
		    [](uint32_t depotId) { return DepotKey::isManagedDepot(depotId); },
		    [appId, side](uint32_t depotId)
		    {
			    g_pLog->info(
			        "ReconcilePin: app=%u %s dropping empty managed depot=%u "
			        "from target\n",
			        appId, side, depotId);
		    });
	}

	// Keep the reconciler's TARGET set aligned with the install planner, then
	// apply configured gid pins.  The ACTIVE/installed side is never changed:
	// real updates and the first public->pinned transition remain visible.
	void patchTargetCtx(void* ctxv, uint32_t appId, uint32_t flags)
	{
		if (!(flags & kFlagTargetSide)) return;       // target/appinfo side only

		auto* ctx = reinterpret_cast<uint8_t*>(ctxv);
		auto* base = *reinterpret_cast<uint8_t* const*>(ctx + kCtxDepotPtrOff);
		auto* const countPtr =
		    reinterpret_cast<int32_t*>(ctx + kCtxDepotCountOff);
		int32_t count = *countPtr;
		if (!base || count <= 0 || count > kMaxDepots) return;

		count = filterEmptyManaged(base, count, appId, "target-ctx");
		*countPtr = count;

		if (!g_pinActive) return;

		auto* e = base;
		for (int32_t i = 0; i < count; ++i, e += kDepotEntryStride)
		{
			const uint32_t depotId = *reinterpret_cast<const uint32_t*>(e);
			const uint64_t pin = g_config.getManifestPin(appId, depotId);
			if (!pin) continue;
			applyPinnedEntry("target-ctx", appId, e, depotId, pin);
		}
	}

	// --- the TARGET-LOCAL fix (the -0x90(ebp) CUtlVector) -----------------
	//
	// applyTargetPin above rewrites the gid in the ctx vector ([ctx+0x78]).
	// For some apps that vector already holds the pin and the DIVERGENT
	// (still-public) gid the reconcile actually compares against lives in a
	// FUNCTION-LOCAL CUtlVector at -0x90(ebp) instead — which an entry hook
	// cannot reach (it isn't built yet at the prologue).  Live trace, app
	// 3525970: [ctx+0x78]=pin (no rewrite) but the local=public -> mismatch ->
	// perpetual "updated depots" loop while installing.
	//
	// That local is filled by a shared appinfo->depot-vector builder.
	// The builder receives &targetVec as an argument and the appId, so a
	// function-replacement hook can patch the local AFTER it is populated.
	// We act ONLY when the return address is EvaluateConfigChanges' own call site,
	// i.e. the local being filled is THIS reconcile's TARGET vector.
	//
	// The builder + its call-site return address are derived from the matched
	// EvaluateConfigChanges pattern (offsets confirmed on build cfe99f0c):
	//   call site `e8 rel32` @ EvalAddr+0x183 -> builder = site+5+rel32;
	//   return addr (the patch gate) = EvalAddr+0x188.
	// A function-replacement hook on the builder needs no PIC fixup: its
	// get_pc_thunk is its 5th instruction, outside the stolen 5 prologue bytes.
	//
	// CUtlVector<DepotEntry>: element base @ +0x0, count @ +0xc.
	constexpr size_t kBuilderCallOff = 0x183;  // EvalAddr -> the `e8` opcode
	constexpr size_t kBuilderRetOff = 0x188;   // EvalAddr -> insn after the call
	constexpr size_t kVecBaseOff = 0x00;
	constexpr size_t kVecCountOff = 0x0c;

	using BuildTargetFn_t = void* (*)(void*, uint32_t, void*, void*, void*,
	                                  void*, void*, void*);
	BuildTargetFn_t g_origBuild = nullptr;
	lm_address_t    g_buildAddr = LM_ADDRESS_BAD;
	lm_address_t    g_buildTramp = LM_ADDRESS_BAD;
	lm_size_t       g_buildSize = 0;
	uintptr_t       g_buildRet = 0;  // EvaluateConfigChanges call-site return addr

	// Walk the appinfo-derived TARGET CUtlVector. Filter the same managed
	// size-zero entries as the final install plan, then force configured gids
	// to their pins. This is always the desired side, never ACTIVE/installed.
	void patchTargetVec(void* vecv, uint32_t appId)
	{
		if (!vecv) return;
		auto* vec = reinterpret_cast<uint8_t*>(vecv);
		auto* base = *reinterpret_cast<uint8_t* const*>(vec + kVecBaseOff);
		auto* const countPtr = reinterpret_cast<int32_t*>(vec + kVecCountOff);
		int32_t count = *countPtr;
		if (!base || count <= 0 || count > kMaxDepots) return;

		count = filterEmptyManaged(base, count, appId, "target-local");
		*countPtr = count;

		if (!g_pinActive) return;

		auto* e = base;
		for (int32_t i = 0; i < count; ++i, e += kDepotEntryStride)
		{
			const uint32_t depotId = *reinterpret_cast<const uint32_t*>(e);
			const uint64_t pin = g_config.getManifestPin(appId, depotId);
			if (!pin) continue;
			applyPinnedEntry("target-local", appId, e, depotId, pin);
		}
	}

	__attribute__((noinline))
	void* hkBuildTarget(void* a0, uint32_t appId, void* a2, void* targetVec,
	                    void* a4, void* ctx, void* a6, void* a7)
	{
		// Capture the call-site BEFORE invoking the original (the builder is a
		// jmp-detoured cdecl function, so our frame's return address is the
		// caller's — EvaluateConfigChanges when the gate matches).
		const bool ours =
		    reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == g_buildRet;
		const bool act = ours && targetVec;

		void* r = g_origBuild(a0, appId, a2, targetVec, a4, ctx, a6, a7);

		if (act) patchTargetVec(targetVec, appId);
		return r;
	}

	// Resolve + hook the target-vector builder, derived from EvalAddr.  Returns
	// false (and installs nothing) on any drift so the loop fix degrades to the
	// ctx-vector patch alone rather than hooking a wrong address.
	bool installBuildTargetHook(lm_address_t evalAddr)
	{
		const auto* site = reinterpret_cast<const uint8_t*>(evalAddr)
		                   + kBuilderCallOff;
		if (site[0] != 0xE8)  // expected `call rel32`
		{
			g_pLog->warn("ReconcilePin: builder call site not `call rel32` "
			             "(got 0x%02x); target-local fix disabled\n", site[0]);
			return false;
		}
		int32_t rel = 0;
		__builtin_memcpy(&rel, site + 1, sizeof(rel));
		g_buildAddr = reinterpret_cast<lm_address_t>(
		    const_cast<uint8_t*>(site) + 5 + rel);
		g_buildRet = reinterpret_cast<uintptr_t>(evalAddr) + kBuilderRetOff;

		g_buildSize = LM_HookCode(g_buildAddr,
		                          reinterpret_cast<lm_address_t>(&hkBuildTarget),
		                          &g_buildTramp);
		if (!g_buildSize || g_buildTramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ReconcilePin: failed to hook target-vector builder; "
			             "target-local fix disabled\n");
			g_buildAddr = LM_ADDRESS_BAD;
			g_buildTramp = LM_ADDRESS_BAD;
			g_buildSize = 0;
			return false;
		}
		g_origBuild = reinterpret_cast<BuildTargetFn_t>(g_buildTramp);
		g_pLog->info("ReconcilePin: hooked target-vector builder at %p "
		             "(gate ret=%p)\n",
		             reinterpret_cast<void*>(g_buildAddr),
		             reinterpret_cast<void*>(g_buildRet));
		return true;
	}

	__attribute__((regparm(1)))
	void* hkEvaluate(void* mgr, void* ctx, void* a1, void* a2)
	{
		if (ctx)
		{
			const auto* c = reinterpret_cast<const uint8_t*>(ctx);
			const uint32_t appId = *reinterpret_cast<const uint32_t*>(c + kCtxAppIdOff);
			const uint32_t flags = *reinterpret_cast<const uint32_t*>(c + kCtxFlagsOff);

			if (g_trace)
			{
				void* base =
				    *reinterpret_cast<void* const*>(c + kCtxDepotPtrOff);
				const int32_t count =
				    *reinterpret_cast<const int32_t*>(c + kCtxDepotCountOff);
				traceLog(appId, flags, base, count);
			}
			patchTargetCtx(ctx, appId, flags);
		}
		return g_orig(mgr, ctx, a1, a2);
	}
}


namespace ReconcilePin
{
	bool setup()
	{
		// The gid rewrite (the loop fix) is DEFAULT ON now: it is driven by any
		// configured ManifestPins entry, while lockedApps remains exclusively the
		// update-suppression policy in Apps::shouldDisableUpdates.
		g_pinActive = true;
		if (const char* e = std::getenv("SLSSTEAM_RECONCILE_PIN"))
		{
			g_pinActive = !(e[0] == '0' && e[1] == '\0');
		}
		if (const char* e = std::getenv("SLSSTEAM_RECONCILE_TRACE"))
		{
			g_trace = !(e[0] == '0' && e[1] == '\0');
		}
		auto& pat = Patterns::CDepotDownloadMgr::EvaluateConfigChanges;
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ReconcilePin: EvaluateConfigChanges pattern not found; "
			             "loop fix disabled\n");
			return false;
		}

		g_addr = pat.address;
		g_size = LM_HookCode(g_addr,
		                     reinterpret_cast<lm_address_t>(&hkEvaluate),
		                     &g_tramp);
		if (!g_size || g_tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ReconcilePin: failed to install hook\n");
			g_addr = LM_ADDRESS_BAD;
			g_tramp = LM_ADDRESS_BAD;
			g_size = 0;
			return false;
		}
		g_orig = reinterpret_cast<ReconFn_t>(g_tramp);
		g_pLog->info("ReconcilePin: hooked EvaluateConfigChanges at %p "
		             "(pin=%d empty=1 trace=%d)\n",
		             reinterpret_cast<void*>(g_addr),
		             static_cast<int>(g_pinActive), static_cast<int>(g_trace));

		// Also patch the appinfo-derived TARGET local (the -0x90(ebp)
		// CUtlVector) via a caller-gated hook on its builder, for apps whose
		// divergent target entries live there instead of in [ctx+0x78]. The
		// hook is also required when pinning is disabled because the empty-depot
		// filter is always active. A drift degrades to the ctx-vector patch.
		installBuildTargetHook(g_addr);
		return true;
	}

	void remove()
	{
		if (g_buildSize && g_buildAddr != LM_ADDRESS_BAD
		    && g_buildTramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(g_buildAddr, g_buildTramp, g_buildSize);
		}
		g_origBuild = nullptr;
		g_buildAddr = LM_ADDRESS_BAD;
		g_buildTramp = LM_ADDRESS_BAD;
		g_buildSize = 0;

		if (g_size && g_addr != LM_ADDRESS_BAD && g_tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(g_addr, g_tramp, g_size);
		}
		g_orig = nullptr;
		g_addr = LM_ADDRESS_BAD;
		g_tramp = LM_ADDRESS_BAD;
		g_size = 0;
	}
}
