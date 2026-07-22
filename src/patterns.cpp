#include "patterns.hpp"

#include "globals.hpp"
#include "memhlp.hpp"
#include "feats/ipcframe.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <iterator>
#include <memory>


Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, lm_module_t* module)
	:
	Pattern_t(name, pattern, followMode, std::vector<uint8_t>(), module)
{
}

Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue, lm_module_t* module)
	:
	name(name),
	pattern(pattern),
	followMode(followMode),
	prologue(prologue),
	module(module)
{
	Patterns::patterns().emplace_back(this);
}

bool Pattern_t::find()
{
	address = MemHlp::searchSignature(name.c_str(), pattern.c_str(), module ? *module : g_modSteamClient , followMode, &prologue[0], prologue.size());
	return address != LM_ADDRESS_BAD;
}

// Re-derive the volatile dispatch-tree root of every IClient*::RunIPCFrame in
// one pass over the steamclient module's executable segments, then point each
// pattern's seed at the nearest live root.  Pure decision logic lives in
// feats/ipcframe.hpp (host-unit-tested); this only walks live memory.
static void autoResolveIpcFrameRoots()
{
	struct Ctx
	{
		std::vector<IpcFrame::Cand> cands;
	} ctx;

	// Collect candidates from executable segments belonging to steamclient.
	// IpcFrame::scan bounds every read by the segment size, so this never
	// touches a guard page (same safety contract as MemHlp::patternScan).
	const auto enumSegments = [](lm_segment_t* seg, lm_void_t* arg) -> lm_bool_t
	{
		auto* c = reinterpret_cast<Ctx*>(arg);
		if ((seg->prot & LM_PROT_XR) != LM_PROT_XR)
			return LM_TRUE;
		// Restrict to the steamclient module's address range.
		if (seg->base + seg->size <= g_modSteamClient.base)
			return LM_TRUE;
		if (seg->base >= g_modSteamClient.base + g_modSteamClient.size)
			return LM_TRUE;

		const auto local = IpcFrame::scan(reinterpret_cast<const uint8_t*>(seg->base), seg->size);
		for (const auto& cand : local)
			c->cands.push_back({
				static_cast<size_t>(seg->base) + cand.offset,
				cand.root,
				cand.available,
			});
		return LM_TRUE;
	};
	LM_EnumSegments(enumSegments, &ctx);

	if (ctx.cands.empty())
	{
		g_pLog->debug("IpcFrame: no RunIPCFrame candidates found; keeping embedded roots\n");
		return;
	}

	Pattern_t* targets[] =
	{
		&Patterns::IClientApps::RunIPCFrame,
		&Patterns::IClientAppManager::RunIPCFrame,
		&Patterns::IClientRemoteStorage::RunIPCFrame,
		&Patterns::IClientUGC::RunIPCFrame,
		&Patterns::IClientUserStats::RunIPCFrame,
		&Patterns::IClientUser::RunIPCFrame,
	};

	for (Pattern_t* p : targets)
	{
		const uint32_t seed = IpcFrame::parseTrailingRoot(p->pattern);
		size_t idx = IpcFrame::resolveConfident(ctx.cands, seed, IpcFrame::kMaxRootDrift);

		// The 2026-07-21 RemoteStorage tree kept its internal comparisons but
		// selected a numerically distant median/root. Match three independent
		// pivots and still require exactly one candidate; never widen the global
		// numeric band and risk assigning an unrelated interface.
		if (idx == SIZE_MAX && p == &Patterns::IClientRemoteStorage::RunIPCFrame)
		{
			static constexpr uint32_t fingerprint[] =
			{
				0x5DB4729A, 0x7F3F5645, 0x84692E78,
			};
			size_t matches = 0;
			for (size_t i = 0; i < ctx.cands.size(); ++i)
			{
				const auto& cand = ctx.cands[i];
				if (IpcFrame::matchesCmpFingerprint(
					reinterpret_cast<const uint8_t*>(cand.offset), cand.available,
					fingerprint, std::size(fingerprint), 4))
				{
					idx = i;
					++matches;
				}
			}
			if (matches != 1)
				idx = SIZE_MAX;
		}
		if (idx == SIZE_MAX)
		{
			// No confident match: drift too large, or an ambiguous neighbour.
			// Keep the embedded root; find() then either still matches it or
			// fails loudly as a required pattern (no silent mis-hook).
			continue;
		}

		const uint32_t found = ctx.cands[idx].root;
		if (found != seed)
		{
			g_pLog->warn
			(
				"IpcFrame: %s root drifted 0x%08X -> 0x%08X; auto-resolved\n",
				p->name.c_str(), seed, found
			);
			IpcFrame::setTrailingRoot(p->pattern, found);
		}
	}
}

bool Patterns::init()
{
	bool found = true;

	// Self-heal the IClient*::RunIPCFrame signatures before scanning.  Their
	// only volatile byte is the dispatch-tree root message id, which drifts
	// when Steam adds/removes interface methods (the 2026-06-23 update broke
	// all four this way).  Re-derive each root structurally so a constant-only
	// drift no longer needs a code change; on any failure the embedded root is
	// kept verbatim (no regression).
	autoResolveIpcFrameRoots();

	// Mark patterns whose absence must NOT abort the load.  Their
	// dependent features null-guard on the resolved address and become
	// a safe no-op when unresolved (no regression on builds where the
	// signature drifts).
	CUser::NotifyLicensesUpdated.optional = true;
	CDepotDownloadMgr::ProcessDepotManifest.optional = true;
	CDepotDownloadMgr::PrepareDepotDownload.optional = true;
	CDepotDownloadMgr::BuildDepotDependency.optional = true;
	CDepotDownloadMgr::EvaluateConfigChanges.optional = true;
	for(auto& pattern : patterns())
	{
		if (!pattern->find())
		{
			if (pattern->optional)
			{
				// Optional patterns degrade to a safe no-op in their
				// dependent feature; don't fail the whole load.
				g_pLog->warn
				(
					"Optional pattern '%s' not found; dependent feature disabled\n",
					pattern->name.c_str()
				);
				continue;
			}
			// Required pattern missing: log WHICH one so a Steam-client
			// update that drifts a signature is diagnosable from the log
			// instead of just "Failed to find all patterns".
			g_pLog->warn
			(
				"Required pattern '%s' not found\n",
				pattern->name.c_str()
			);
			found = false;
		}
	}

	return found;
}

using SigFollowMode = MemHlp::SigFollowMode;

namespace Patterns
{
	Pattern_t FamilyGroupRunningApp
	{
		"FamilyGroupRunningApp",
		"E8 ? ? ? ? 83 C4 10 83 EC 08 C7 46 ? 01 00 00 00 C6 46 ? 01 56 57 E8 ? ? ? ? 83 C4 1C B8 01 00 00 00 5B 5E 5F 5D C3 ? ? ? ? ? ? ? 83 EC 04",
		SigFollowMode::Relative
	};
	Pattern_t StopPlayingBorrowedApp
	{
		"StopPlayingBorrowedApp",
		"8B 40 ? 83 EC 0C 89 F3 8B 95",
		SigFollowMode::PrologueUpwards,
		std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
	};

	Pattern_t TraceIPC
	{
		"TraceIPC",
		"E8 ? ? ? ? 83 C4 10 85 FF 74 ? 8B 07 83 EC 04 FF B5 ? ? ? ? FF B5 ? ? ? ? 57 FF 10 83 C4 10 8D 45 ? 83 EC 04 89 F3 6A 04 50 FF 75",
		SigFollowMode::Relative
	};

	namespace CAPIJob
	{
		Pattern_t GetPlayerStats
		{
			"CAPIJob::GetPlayerStats",
			"E8 ? ? ? ? 83 C4 10 89 C5 E9 ? ? ? ? ? ? 80 BE ? ? ? ? 00",
			SigFollowMode::Relative
		};
	}

	namespace CProtoBufMsgBase
	{
		Pattern_t InitFromPacket
		{
			"CProtoBufMsgBase::InitFromPacket",
			"E8 ? ? ? ? 58 8B 45 ? 8B 8D",
			SigFollowMode::Relative
		};
		Pattern_t Send
		{
			"CProtoBufMsgBase::Send",
			"E8 ? ? ? ? 59 5A 50 56 E8 ? ? ? ? 83 C4 0C",
			SigFollowMode::Relative
		};
	};

	namespace CSteamEngine
	{
		Pattern_t Init
		{
			"CSteamEngine::Init",
			"E8 ? ? ? ? 83 C4 10 8D 83 ? ? ? ? 83 EC 0C 89 AB",
			SigFollowMode::Relative
		};
		Pattern_t SetAppIdForCurrentPipe
		{
			"CSteamEngine::SetAppIdForCurrentPipe",
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? 8B 85 ? ? ? ? 83 EC 08 FF B5",
			SigFollowMode::Relative
		};
		Pattern_t Offset_User
		{
			"CSteamEngine::m_pUser",
			"8B 80 ? ? ? ? FF 75 ? 8D 34",
			SigFollowMode::None
		};
	}

	namespace CSteamMatchmakingServers
	{
		Pattern_t GetServerDetails
		{
			"CSteamMatchmakingServers::GetServerDetails",
			"89 45 ? 83 C4 10 83 EC 0C 89 F3",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t RequestInternetServerList
		{
			"CSteamMatchmakingServers::RequestInternetServerList",
			"C7 04 24 ? ? 00 00 E8 ? ? ? ? 5A 89 45 ? 59 FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? 6A 01",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace CUser
	{
		Pattern_t CheckAppOwnership
		{
			"CUser::CheckAppOwnership",
			"E8 ? ? ? ? 88 45 ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 45 ? 80 7D ? 00",
			SigFollowMode::Relative
		};
		Pattern_t GetSubscribedApps
		{
			"CUser::GetSubscribedApps",
			"E8 ? ? ? ? 89 C6 83 C4 10 85 C0 0F 84 ? ? ? ? 8B 9D ? ? ? ? 39 D8",
			SigFollowMode::Relative
		};
		Pattern_t PostCallback
		{
			"CSteamEngine::PostCallback",
			"E8 ? ? ? ? 8D 86 ? ? ? ? 83 C4 18 68 F6 01 00 00",
			SigFollowMode::Relative
		};
		Pattern_t UpdateAppOwnershipTicket
		{
			"IClientUser::UpdateAppOwnershipTicket",
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? ? 8D 45 ? 89 45 ? EB",
			SigFollowMode::Relative
		};
		// CUser::<broadcast LicensesUpdated_t>(CUser* this)
		//
		// The license-update notifier: rebuilds the LicensesUpdated_t
		// callback (callback id 0x7d) from the CUser's own license vector
		// and posts it to every subscriber via the PostCallback dispatch.
		// Used as the post-injection reconcile: after we append our
		// AdditionalApps into package 0's AppIdVec, invoking this on the
		// local CUser forces Steam's ownership/library layer to re-read
		// licenses (and therefore package 0, now containing our appids),
		// which breaks the cold-cache PICS product-info request loop.
		//
		// Positively identified via the RTTI string "17LicensesUpdated_t"
		// referenced just before it posts callback 0x7d.  Single stack arg
		// (`this`, read from [ebp+0x8]); standard cdecl, safe to call by
		// resolved pointer with g_pLocalUser as `this`.
		//
		// Direct prologue match (push ebp / mov ebp,esp / push edi,esi,ebx
		// / get_pc_thunk + add ebx / sub esp,0x1bc / mov edi,[ebp+0x8] /
		// mov edi,[eax+<off>] / mov [ebp-0x1ac],ebx / test edi,edi).  The
		// get_pc_thunk call rel, the PIC add immediate, the frame size, and
		// the [ebp-0x1ac] spill offset are masked so local-frame reshuffles
		// across builds stay compatible.  The member offset (`8B B8 ?? ?? 00
		// 00` = mov edi,[eax+0x1bXX]) is ALSO masked: it is a CUser layout
		// field whose value drifted 0x1b18 -> 0x1b14 on the 2026-06-23 client
		// (a 4-byte shift; RequiresLegacyCDKey moved the same way).  SLSsteam
		// only needs to LOCATE this function (it invokes it by pointer with
		// g_pLocalUser as `this`); the offset value itself is internal to the
		// function, so wildcarding it makes the signature self-heal across that
		// class of layout drift while staying unique.  Verified: exactly 1
		// match in both the pre- and post-2026-06-23 steamclient.so.
		Pattern_t NotifyLicensesUpdated
		{
			"CUser::NotifyLicensesUpdated",
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC ? ? ? ? 8B 45 08 8B B8 ? ? 00 00 89 9D ? ? FF FF 85 FF",
			SigFollowMode::None
		};
	}

	namespace IClientAppManager
	{
		Pattern_t RunIPCFrame
		{
			"IClientAppManager::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D B7 85 0A 7A",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t BCanRemotePlayTogether
		{
			"IClientAppManager::BCanRemotePlayTogether",
			"58 5A FF 74 24 ? 56 E8 ? ? ? ? 83 C4 10 85 C0 74",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x53, 0x56, 0x57 }
		};
	}

	namespace IClientApps
	{
		Pattern_t RunIPCFrame
		{
			"IClientApps::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 37 9C 88 A6",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientRemoteStorage
	{
		Pattern_t RunIPCFrame
		{
			"IClientRemoteStorage::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 6C E8 2F 87",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUser
	{
		Pattern_t RunIPCFrame
		{
			"IClientUser::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 10 A3 86 73",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};

		Pattern_t BLoggedOn
		{
			"IClientUser::BLoggedOn",
			"E9 ? ? ? ? ? ? ? ? ? ? 5B 5E 5F FF E0",
			SigFollowMode::Relative
		};
		Pattern_t BUpdateAppOwnershipTicket
		{
			"IClientUser::BUpdateAppOwnershipTicket",
			"83 EC 0C 89 F3 8B 7D ? FF 30 E8 ? ? ? ? 83 C4 10 83 FF 01 77 ? 84 C0 75 ? 80 7D ? 00 74 ? 80 7D ? 00 0F 84 ? ? ? ? 8D 65",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t GetAppOwnershipTicketExtendedData
		{
			"IClientUser::GetAppOwnershipTicketExtendedData",
			"83 EC 24 FF 74 24 ? 8B 44 24",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
		};
		Pattern_t GetSteamId
		{
			"IClientUser::GetSteamID",
			"E8 ? ? ? ? 89 D8 83 C4 0C 83 C4 08 5B C2 04 00 ? 83 EC 08 50 53 FF D2 89 D8 83 C4 0C 83 C4 08 5B C2 04 00",
			SigFollowMode::Relative
		};
		Pattern_t IsUserSubscribedAppInTicket
		{
			"IClientUser::IsUserSubscribedAppInTicket",
			"E8 ? ? ? ? 89 C3 83 C4 20 8B ? ? ? ? ? 8B",
			SigFollowMode::Relative
		};
		// The tail thunk adjusts the interface `this` to the implementation
		// pointer with `sub eax,<off>` (`2D ?? ?? 00 00`).  That offset is a
		// layout constant that drifted 0x18d8 -> 0x18d4 on the 2026-06-23
		// client; it is only part of the LOCATION signature (SLSsteam hooks the
		// function, it does not use the offset value), so it is masked here to
		// self-heal across that drift.  The long surrounding tail keeps the
		// match unique — verified exactly 1 hit in both the pre- and
		// post-2026-06-23 steamclient.so.
		Pattern_t RequiresLegacyCDKey
		{
			"IClientUser::RequiresLegacyCDKey",
			"75 ? 83 C4 1C 31 C0 5B 5E 5F 5D C3 ? ? ? ? ? 8B 44 24 ? 83 C4 1C 89 F9 89 F2 5B 5E 5F 5D 2D ? ? 00 00",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
		};
	}

	namespace IClientUGC
	{
		Pattern_t RunIPCFrame
		{
			"IClientUGC::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 62 0C D2 71",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUserStats
	{
		Pattern_t RunIPCFrame
		{
			"IClientUserStats::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 8F 65 6D 87",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace CPackageInfoCache
	{
		Pattern_t LoadPackage
		{
			"CPackageInfoCache::LoadPackage",
			"E8 ? ? ? ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 95 ? ? FF FF 8B 7A 18 83 FF FF",
			SigFollowMode::Relative
		};
	}

	namespace CUtlMemory
	{
		Pattern_t Grow
		{
			"CUtlMemory::Grow",
			"E8 ? ? ? ? 8B 85 ? ? FF FF 83 C4 10 8B 40 44 89 85 ? ? FF FF 83 C0 01 E9",
			SigFollowMode::Relative
		};
	}

	namespace CDepotDownloadMgr
	{
		// Two cooperating hook points in CDepotDownloadMgr, both with the
		// same 7-dword signature (context, ., appId, depotId, uint64 gid, .)
		// and both self-contained PIC functions.
		//
		// (1) ProcessDepotManifest (the manifest-acquisition LEAF, 5 callers):
		//     builds "<root>/depotcache/<depot>_<gid>.manifest" from its gid
		//     arg, checks it on disk, and only calls BYldRequestDepotManifest
		//     when missing.  Redirecting the gid here makes the on-disk check
		//     find the locally-staged (zip) manifest and SKIP the request-code
		//     fetch — this is what lets a providers-down install proceed past
		//     "No internet connection".  PIC get_pc_thunk is the FIRST insn, so
		//     fixPICThunkCall must repair the relocated thunk in the tramp.
		//
		// (2) PrepareDepotDownload (one of the 5 callers, a LATER pipeline
		//     stage): after calling the leaf it looks the depot up in the
		//     per-download table BY the gid it was called with and derefs the
		//     per-manifest state pointer.  If the leaf was redirected to the
		//     zip gid but this frame still looks up the public gid -> miss ->
		//     NULL deref -> SIGSEGV at Reconfiguring (core-dump confirmed).
		//     Redirecting the
		//     gid here too keeps the leaf call and the table lookup consistent.
		//     PIC get_pc_thunk is at +5 (after the 5-byte prologue we relocate)
		//     so fixPICThunkCall is a harmless no-op for this hook point.
		//
		// Both verified: 1 match each in .text.
		Pattern_t ProcessDepotManifest
		{
			"CDepotDownloadMgr::ProcessDepotManifest",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 55 1C 89 45 C0 8B 45 18 89 55 CC 89 45 C8",
			SigFollowMode::None
		};

		Pattern_t PrepareDepotDownload
		{
			"CDepotDownloadMgr::PrepareDepotDownload",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 60 8B 7D 08 8B 45 18 8B 55 1C FF 75 20 89 45 98 52 50 FF 75 14 89 55 9C FF 75 10 FF 75 0C 57 E8 ? ? ? ? 8B 47 4C 83 C4 20 83 F8 FF",
			SigFollowMode::None
		};

		// (3) BuildDepotDependency (the install-plan CONSUMER, located via a
		//     runtime stack trace).  The
		//     per-app planner: receives an already-built CUtlVector<DepotEntry>
		//     (arg2 = [ebp+0x10]; count @ +0xc, element base @ +0, stride 0x20)
		//     and, per entry, copies ManifestGid (+0x8) into the context's
		//     planned-gid vector (ctx+0x664) AND drives ProcessDepotManifest /
		//     the shared-depot handler.  Patching depots[i].ManifestGid here
		//     (before the original runs) is the LumaCore manifest-override
		//     point: it mutates the SOURCE the commit reads from, not the
		//     by-value gid the acquisition leaf gets (which the commit ignores).
		//     PIC get_pc_thunk is the FIRST insn (like ProcessDepotManifest) ->
		//     fixPICThunkCall repairs the relocated thunk in the tramp.
		//     Verified: 1 match in .text (entry VA 0x11413e0 on build
		//     cfe99f0cc8fee644e2a8e3d1a0794e49).
		Pattern_t BuildDepotDependency
		{
			"CDepotDownloadMgr::BuildDepotDependency",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 8C 04 00 00 8B 55 10 8B 7D 0C 89 85 A0 FB FF FF 8B 45 08",
			SigFollowMode::None
		};

		// (4) EvaluateConfigChanges (the post-commit reconcile, located via
		//     static RE).  Emits the
		//     content_log "AppID %u ...config changed : added/removed/updated
		//     depots %s" lines and decides "Update Required".  It diffs the
		//     app's installed depot vector (ptr @ ctx+0x78, count @ ctx+0x84,
		//     stride 0x20, ManifestGid @ +0x8) against an appinfo-derived
		//     target list, flagging a depot "updated" when the two gids differ
		//     (the movq/pxor compare at VA 0xfe4598).  THIS is what perpetually
		//     re-flags a downgraded (pinned) install whose appinfo still
		//     carries the public gid -> the loop.  Hook target for the (B) fix.
		//     Calling convention: a global anchor/manager pointer comes in EAX
		//     (used as both the manager object and the PIC string anchor); the
		//     three args are on the stack (ctx @ ebp+0x8, appId @ ctx+0x8).
		//     The prologue's first insn is `push ebp` (NOT a get_pc_thunk), so
		//     no fixPICThunkCall is needed; a regparm(1) detour preserves EAX.
		//     Verified: 1 match in .text (entry VA 0xfe425a on build
		//     cfe99f0cc8fee644e2a8e3d1a0794e49).
		Pattern_t EvaluateConfigChanges
		{
			"CDepotDownloadMgr::EvaluateConfigChanges",
			"55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF 8B 45 10 89 85 40 FF FF FF 8B 45 08 8B 40 04",
			SigFollowMode::None
		};
	}

	namespace IClientUtils
	{
		Pattern_t RunIPCFrame
		{
			"IClientUtils::RunIPCFrame",
			"83 EC 08 89 F3 50 57 E8 ? ? ? ? 58 FF B5 ? ? ? ? E8 ? ? ? ? 58 8D 45",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t Offset_GetPipeIndex
		{
			"IClientUtils::m_PipeIndex",
			"8B 91 ? ? ? ? 83 F8 FF 74 ? 8B 89 ? ? ? ? EB ? ? ? ? 8B 00 83 F8 FF 74 ? 8D 04 ? 8D 04 ? 3B 50",
			SigFollowMode::None,
		};
	}

	namespace ISteamMatchmakingPingResponse
	{
		Pattern_t ServerResponded
		{
			"ISteamMatchmakingPingResponse::ServerResponded",
			"8B 85 ? ? ? ? 8B 40 ? 85 C0 0F 84 ? ? ? ? 39 46",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x57, 0xe5, 0x89, 0x55 },
			&g_modSteamUI
		};
	}

	namespace CWebSocketConnection
	{
		Pattern_t BBuildAndAsyncSendFrame
		{
			"CWebSocketConnection::BBuildAndAsyncSendFrame",
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 81 EC AC 00 00 00 "
			"8B 45 10 8B 55 08 89 85 ? ? FF FF 89 95 ? ? FF FF "
			"65 A1 14 00 00 00",
			SigFollowMode::None
		};
	}

	namespace CRemoteClientManager
	{
		Pattern_t RecvPkt
		{
			"CRemoteClientManager::RecvPkt",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 1C "
			// [esi+0x8XX]: PIC-relative global slot whose displacement
			// drifts between Steam client builds (0x8B0 -> 0x8B4 on
			// 1781041600).  Wildcard the displacement byte so the match
			// survives that shift; the rest of the body keeps it unique.
			"8B 86 ? 08 00 00 8B 00 85 C0 0F 85 ? ? ? ? "
			"C7 45 E4 00 00 00 00 83 EC 08 89 F3 6A 01 FF 75 0C "
			"E8 ? ? ? ? 89 C7 83 C4 10 85 C0 0F 84 ? ? ? ? 83 EC 0C 50",
			SigFollowMode::None
		};
	}

	namespace CJobMgr
	{
		Pattern_t BRouteMsgToJob
		{
			"CJobMgr::BRouteMsgToJob",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 7C "
			"8B 45 08 8B 4D 14 89 45 90 8B 45 0C 89 4D A0 89 45 88 "
			"8B 45 10 89 45 A4 65 8B 0D 14 00 00",
			SigFollowMode::None
		};
	}

	namespace CDepotDownloadMgr
	{
		Pattern_t BYldRequestDepotManifest
		{
			"CDepotDownloadMgr::BYldRequestDepotManifest",
			"55 b9 fd ff ff ff 89 e5 57 e8 ? ? ? ? 81 c7 ? ? ? ? 56 53 83 ec 7c 8b 45 14 8b 55 18",
			SigFollowMode::None
		};
	}

	std::vector<Pattern_t*>& patterns()
	{
		// Function-local static: guaranteed initialized on first call,
		// which happens from the first Pattern_t constructor above —
		// before init() ever iterates it.  Immune to static-init order.
		static std::vector<Pattern_t*> instance;
		return instance;
	}
}

