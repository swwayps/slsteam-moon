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

#include "libmem/libmem.h"

#include <atomic>
#include <climits>
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
	constexpr int32_t kMaxDepots = 512;

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
			auto* gidp = reinterpret_cast<uint64_t*>(e + kDepotEntryGidOff);
			if (*gidp != pin)
			{
				g_pLog->info("ReconcilePin: app=%u target depot=%u gid=%llu -> "
				             "pinned gid=%llu\n",
				             appId, depotId,
				             static_cast<unsigned long long>(*gidp),
				             static_cast<unsigned long long>(pin));
				*gidp = pin;
			}
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
	// That local is filled by a shared appinfo->depot-vector builder. Redirect
	// only EvaluateConfigChanges' direct call so other callers remain untouched.
	//
	// The builder + its call-site return address are derived from the matched
	// EvaluateConfigChanges pattern (offsets confirmed on build cfe99f0c):
	//   call site `e8 rel32` @ EvalAddr+0x183 -> builder = site+5+rel32;
	// The wrapper calls the original entry directly, without a trampoline or
	// return-address inspection.
	//
	// CUtlVector<DepotEntry>: element base @ +0x0, count @ +0xc.
	constexpr size_t kBuilderCallOff = 0x183;  // EvalAddr -> the `e8` opcode
	constexpr size_t kVecBaseOff = 0x00;
	constexpr size_t kVecCountOff = 0x0c;

	using BuildTargetFn_t = void* (*)(void*, uint32_t, void*, void*, void*,
	                                  void*, void*, void*);
	BuildTargetFn_t g_origBuild = nullptr;
	lm_address_t    g_buildAddr = LM_ADDRESS_BAD;
	lm_size_t       g_buildSize = 0;
	uint8_t         g_buildCall[5]{};

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
			auto* gidp = reinterpret_cast<uint64_t*>(e + kDepotEntryGidOff);
			if (*gidp != pin)
			{
				g_pLog->info("ReconcilePin: app=%u target-local depot=%u "
				             "gid=%llu -> pinned gid=%llu\n",
				             appId, depotId,
				             static_cast<unsigned long long>(*gidp),
				             static_cast<unsigned long long>(pin));
				*gidp = pin;
			}
		}
	}

	__attribute__((noinline))
	void* hkBuildTarget(void* a0, uint32_t appId, void* a2, void* targetVec,
	                    void* a4, void* ctx, void* a6, void* a7)
	{
		void* r = g_origBuild(a0, appId, a2, targetVec, a4, ctx, a6, a7);
		if (targetVec) patchTargetVec(targetVec, appId);
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
		const lm_address_t builderAddr = reinterpret_cast<lm_address_t>(
		    const_cast<uint8_t*>(site) + 5 + rel);

		const intptr_t hookRel = reinterpret_cast<intptr_t>(&hkBuildTarget)
		    - (reinterpret_cast<intptr_t>(site) + 5);
		if (hookRel < INT32_MIN || hookRel > INT32_MAX)
		{
			g_pLog->warn("%s", "ReconcilePin: target-vector call redirect is out of "
			                    "rel32 range; target-local fix disabled\n");
			return false;
		}

		uint8_t replacement[5] = {0xE8, 0, 0, 0, 0};
		const int32_t hookRel32 = static_cast<int32_t>(hookRel);
		__builtin_memcpy(replacement + 1, &hookRel32, sizeof(hookRel32));
		__builtin_memcpy(g_buildCall, site, sizeof(g_buildCall));
		g_buildAddr = reinterpret_cast<lm_address_t>(const_cast<uint8_t*>(site));

		lm_prot_t oldProt = LM_PROT_NONE;
		if (!LM_ProtMemory(
		        g_buildAddr, sizeof(replacement), LM_PROT_XRW, &oldProt))
		{
			g_pLog->warn("%s", "ReconcilePin: failed to make target-vector call "
			                    "writable; target-local fix disabled\n");
			g_buildAddr = LM_ADDRESS_BAD;
			return false;
		}
		const bool wrote =
		    LM_WriteMemory(g_buildAddr, replacement, sizeof(replacement))
		    == sizeof(replacement);
		LM_ProtMemory(g_buildAddr, sizeof(replacement), oldProt, nullptr);
		if (!wrote)
		{
			g_pLog->warn("%s", "ReconcilePin: failed to redirect target-vector "
			                    "call; target-local fix disabled\n");
			g_buildAddr = LM_ADDRESS_BAD;
			return false;
		}
		g_buildSize = sizeof(replacement);
		g_origBuild = reinterpret_cast<BuildTargetFn_t>(builderAddr);
		g_pLog->info("ReconcilePin: redirected target-vector call at %p "
		             "(builder=%p)\n",
		             reinterpret_cast<void*>(g_buildAddr),
		             reinterpret_cast<void*>(builderAddr));
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
		if (g_buildSize && g_buildAddr != LM_ADDRESS_BAD)
		{
			lm_prot_t oldProt = LM_PROT_NONE;
			if (LM_ProtMemory(g_buildAddr, g_buildSize, LM_PROT_XRW, &oldProt))
			{
				LM_WriteMemory(g_buildAddr, g_buildCall, g_buildSize);
				LM_ProtMemory(g_buildAddr, g_buildSize, oldProt, nullptr);
			}
		}
		g_origBuild = nullptr;
		g_buildAddr = LM_ADDRESS_BAD;
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
