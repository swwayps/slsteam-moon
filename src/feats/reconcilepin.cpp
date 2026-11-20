// SPDX-License-Identifier: AGPL-3.0-only
//
// See reconcilepin.hpp for the design.

#include "reconcilepin_guard.hpp"
#include "reconcilepin.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../patterns.hpp"
#include "../sdk/CUtl.hpp"

#include "depotkey.hpp"
#include "manageddepotfilter.hpp"
#include "manifeststore.hpp"

#include "libmem/libmem.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <climits>
#include <cstdint>
#include <cstdlib>


namespace
{
	// EvaluateConfigChanges on x86-64: (ctx, CUser*, flag); the third value is
	// passed through pointer-sized so no register bits are dropped.
	using ReconFn_t = std::uintptr_t (*)(
	    void* ctx, void* user, std::uintptr_t a2);

	ReconFn_t    g_orig = nullptr;
	lm_address_t g_addr = LM_ADDRESS_BAD;
	lm_address_t g_tramp = LM_ADDRESS_BAD;
	lm_size_t    g_size = 0;

	bool g_pinActive = false;   // SLSSTEAM_RECONCILE_PIN: perform the gid rewrite

	// ctx layout on x86-64 (entry reads `mov edx,[rdi+8]` for the flags and
	// `[rbx+0xc]` for the app id; i386 had +0x04 / +0x08).
	constexpr size_t kCtxFlagsOff = 0x08;
	constexpr size_t kCtxAppIdOff = 0x0c;
	// The depot CUtlVector inside ctx moves when Steam grows the context
	// (0xc0 on the x86-64 clients), so it is decoded from
	// EvaluateConfigChanges at setup.  0 = not located: the ctx-vector patch
	// stays off instead of rewriting whatever field now sits at a stale offset.
	size_t g_ctxDepotVecOff = 0;
	constexpr size_t kCtxVecCountOff = offsetof(CUtlVector<char>, size);
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
			// Once per text: the target ctx is rebuilt on every reconcile.
			g_pLog->infoOnce(
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

	int32_t filterEmptyManaged(uint8_t* base, int32_t count,
	                           uint32_t appId, const char* side)
	{
		return ManagedDepotFilter::compactEmptyManaged(
		    base, count,
		    [](uint32_t depotId) { return DepotKey::isManagedDepot(depotId); },
		    [appId, side](uint32_t depotId)
		    {
			    g_pLog->infoOnce(
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
		if (g_ctxDepotVecOff == 0) return;            // layout not decoded

		auto* ctx = reinterpret_cast<uint8_t*>(ctxv);
		auto* base = *reinterpret_cast<uint8_t* const*>(ctx + g_ctxDepotVecOff);
		auto* const countPtr = reinterpret_cast<int32_t*>(
		    ctx + g_ctxDepotVecOff + kCtxVecCountOff);
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

	// --- the TARGET-LOCAL fix (the [rbp-0x110] CUtlVector on x86-64) ------
	//
	// The divergent gid can sit in this local instead of the context depot
	// vector. It is filled after entry, so EvaluateConfigChanges' own builder
	// call is redirected.
	//
	// CUtlVector<DepotEntry>: SDK layout, element base @ +0x0, size @ +0x10.
	constexpr size_t kVecBaseOff = offsetof(CUtlMemory<char>, base);
	constexpr size_t kVecCountOff = offsetof(CUtlVector<char>, size);

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
			applyPinnedEntry("target-local", appId, e, depotId, pin);
		}
	}

	// Both hooks run on Steam's content thread: a failed patch is logged once
	// and Steam goes on with the vector as it is.
	void notePatchFailure() noexcept
	{
		static std::atomic<bool> logged{false};
		if (logged.exchange(true, std::memory_order_relaxed)) return;
		try
		{
			g_pLog->warn("%s", "ReconcilePin: a depot patch failed; Steam keeps "
			                    "its own plan for that pass\n");
		}
		catch (...)
		{
		}
	}

	__attribute__((noinline))
	void* hkBuildTarget(void* a0, uint32_t appId, void* a2, void* targetVec,
	                    void* a4, void* ctx, void* a6, void* a7)
	{
		void* r = g_origBuild(a0, appId, a2, targetVec, a4, ctx, a6, a7);
		if (targetVec)
			ReconcilePin::detail::runPatched([&] { patchTargetVec(targetVec, appId); }, notePatchFailure);
		return r;
	}

	// Resolve + hook the target-vector builder, derived from EvalAddr.  Returns
	// false (and installs nothing) on any drift so the loop fix degrades to the
	// ctx-vector patch alone rather than hooking a wrong address.
	bool installBuildTargetHook(lm_address_t evalAddr)
	{
		const auto* fn = reinterpret_cast<const uint8_t*>(evalAddr);
		const size_t scanLen = ReconcilePin::detail::kBuilderCallOff
		                     + ReconcilePin::detail::kBuilderCallWindow + 8;
		// The RE'd call offset drifts when Steam recompiles the function, so
		// locate the call instead of trusting a fixed offset. The call must
		// reach the resolved builder; without that locator, or on
		// ambiguity/absence, it degrades to the ctx-vector patch alone.
		const lm_address_t builder =
		    Patterns::CUserAppManager::BuildDepotDependency.address;
		const long callOff = ReconcilePin::detail::findBuilderCall(
		    fn, scanLen, reinterpret_cast<uintptr_t>(fn),
		    builder == LM_ADDRESS_BAD ? 0 : static_cast<uintptr_t>(builder));
		if (callOff < 0)
		{
			using ReconcilePin::detail::kBuilderCallOff;
			using ReconcilePin::detail::kBuilderCallWindow;
			char builderText[32] = "unresolved";
			if (builder != LM_ADDRESS_BAD)
				std::snprintf(builderText, sizeof(builderText), "%p",
				              reinterpret_cast<void*>(builder));
			g_pLog->warn("ReconcilePin: %s call to the builder (%s) in +0x%zx..+0x%zx; "
			             "target-local fix disabled\n",
			             callOff == -2 ? "more than one" : "no",
			             builderText, kBuilderCallOff - kBuilderCallWindow,
			             kBuilderCallOff + kBuilderCallWindow);
			return false;
		}
		const auto* site = fn + callOff;
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

	std::uintptr_t hkEvaluate(void* ctx, void* user, std::uintptr_t a2)
	{
		if (ctx)
		{
			ReconcilePin::detail::runPatched([&]
			{
				const auto* c = reinterpret_cast<const uint8_t*>(ctx);
				const uint32_t appId = *reinterpret_cast<const uint32_t*>(c + kCtxAppIdOff);
				const uint32_t flags = *reinterpret_cast<const uint32_t*>(c + kCtxFlagsOff);

				patchTargetCtx(ctx, appId, flags);
			}, notePatchFailure);
		}
		return g_orig(ctx, user, a2);
	}
}


namespace ReconcilePin
{
	bool setup()
	{
		// The gid rewrite (the loop fix) is on by default and is driven by any
		// configured ManifestPins entry; SLSSTEAM_RECONCILE_PIN=0 turns it off.
		// lockedApps does not gate it: it is only the update-suppression policy
		// in Apps::shouldDisableUpdates.
		g_pinActive = true;
		if (const char* e = std::getenv("SLSSTEAM_RECONCILE_PIN"))
		{
			g_pinActive = !(e[0] == '0' && e[1] == '\0');
		}
		auto& pat = Patterns::CDepotDownloadMgr::EvaluateConfigChanges;
		if (pat.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("ReconcilePin: EvaluateConfigChanges pattern not found; "
			             "loop fix disabled\n");
			return false;
		}

		// Decode the ctx depot-vector offset before the detour can run, reading
		// no further than the end of the code segment.
		g_ctxDepotVecOff = 0;
		lm_segment_t segment{};
		if (LM_FindSegment(pat.address, &segment) && segment.end > pat.address)
		{
			const size_t readable = static_cast<size_t>(segment.end - pat.address);
			const long vecOff = ReconcilePin::detail::findCtxDepotVectorOffset(
			    reinterpret_cast<const uint8_t*>(pat.address),
			    readable < ReconcilePin::detail::kCtxDepotLoopScanLen
			        ? readable : ReconcilePin::detail::kCtxDepotLoopScanLen);
			if (vecOff > 0) g_ctxDepotVecOff = static_cast<size_t>(vecOff);
		}
		if (g_ctxDepotVecOff == 0)
		{
			g_pLog->warn("%s", "ReconcilePin: context depot vector not located; "
			                    "ctx-vector fix disabled\n");
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
		             "(pin=%d empty=1 ctx_depots=+0x%zx)\n",
		             reinterpret_cast<void*>(g_addr),
		             static_cast<int>(g_pinActive), g_ctxDepotVecOff);

		// Redirect EvaluateConfigChanges' builder call so the -0x90(ebp) target local
		// is patched too, even with pinning off: the empty-depot filter always runs.
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
