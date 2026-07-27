// SPDX-License-Identifier: AGPL-3.0-only

#include "depotquarantine.hpp"

#include "depotkey.hpp"
#include "depotquarantine_store.hpp"
#include "dlcids.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../memhlp.hpp"
#include "../patterns.hpp"

#include "libmem/libmem.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	using OnChunkStackFn = void(*)(void*, void*, const char*, uint32_t,
	                               const char*, uint32_t);
	using OnChunkRegFn = void (__attribute__((regparm(3))) *)(
	    void*, void*, const char*, uint32_t, const char*, uint32_t);

	template<typename Fn>
	struct Detour
	{
		Fn orig = nullptr;
		lm_address_t addr = LM_ADDRESS_BAD;
		lm_address_t tramp = LM_ADDRESS_BAD;
		lm_size_t size = 0;
	};

	Detour<OnChunkStackFn> g_stack;
	Detour<OnChunkRegFn> g_reg;
	DepotQuarantine::Registry g_registry;

	// Persisted decisions, mirrored in memory. Guarded because the planner
	// hook (Steam worker) and the package-0 hook can both reach them.
	std::mutex g_recordsLock;
	std::vector<DepotQuarantineStore::Record> g_records;

	std::string recordsPath()
	{
		std::ostringstream path;
		path << g_config.getDir().c_str() << "/cache/dlcquarantine.txt";
		return path.str();
	}

	// Best-effort, atomic-enough persistence: a lost file simply means the
	// next session re-learns the failure the same way this one did.
	void saveRecordsLocked()
	{
		const auto path = recordsPath();
		std::error_code ec;
		std::filesystem::create_directories(
		    std::filesystem::path(path).parent_path(), ec);

		const auto tmp = path + ".tmp";
		{
			std::ofstream out(tmp, std::ios::out | std::ios::trunc);
			if (!out.is_open()) return;
			out << DepotQuarantineStore::serialize(g_records);
			if (!out.good()) return;
		}
		std::filesystem::rename(tmp, path, ec);
		if (ec) std::filesystem::remove(tmp, ec);
	}

	std::string readRecordsFile()
	{
		std::error_code ec;
		const auto path = recordsPath();
		if (!std::filesystem::is_regular_file(path, ec)) return {};
		std::ifstream in(path, std::ios::in);
		if (!in.is_open()) return {};
		std::ostringstream text;
		text << in.rdbuf();
		return text.str();
	}

	uint32_t depotIdFromContext(void* context)
	{
		if (!context) return 0;
		const auto bytes = reinterpret_cast<const uint8_t*>(context);
		const auto owner = *reinterpret_cast<void* const*>(bytes + 0x08);
		if (!owner) return 0;
		const auto depot = *reinterpret_cast<void* const*>(
		    reinterpret_cast<const uint8_t*>(owner) + 0x0c);
		if (!depot) return 0;
		return *reinterpret_cast<const uint32_t*>(depot);
	}

	uint64_t chunkFingerprint(const void* chunk)
	{
		if (!chunk) return 0;
		// OnChunkUnpacked itself formats the 20-byte SHA at chunk+0x0c. Hashing
		// it lets the policy distinguish separate failing chunks without copying
		// or logging their identifiers.
		const auto sha = reinterpret_cast<const uint8_t*>(chunk) + 0x0c;
		uint64_t hash = 14695981039346656037ULL;
		for (std::size_t i = 0; i < 20; ++i)
		{
			hash ^= sha[i];
			hash *= 1099511628211ULL;
		}
		return hash ? hash : 1;
	}

	bool currentStplugScriptExists(uint32_t appId)
	{
		if (!appId) return false;
		const char* home = std::getenv("HOME");
		if (!home) return false;

		const std::array<const char*, 3> roots = {
		    "/.steam/steam", "/.steam/debian-installation",
		    "/.local/share/Steam"};
		for (const char* root : roots)
		{
			std::error_code ec;
			const std::filesystem::path script =
			    std::string(home) + root + "/config/stplug-in/"
			    + std::to_string(appId) + ".lua";
			if (std::filesystem::is_regular_file(script, ec)) return true;
		}
		return false;
	}

	// A depot is in scope only while WE supply its key (a LuaTools depot) and
	// the app's stplug-in script is still installed.
	bool inManagedScope(const DepotKey::SavedKey& key, uint32_t depotId)
	{
		return key.managed && key.depotId == depotId
		    && currentStplugScriptExists(key.appId);
	}

	// Restore last session's decisions. Records whose key changed are dropped
	// here, so a refreshed stplug-in key retries the DLC normally.
	void restorePersistedRecords()
	{
		auto parsed = DepotQuarantineStore::parse(readRecordsFile());
		if (parsed.empty()) return;

		std::vector<DepotQuarantineStore::Record> live;
		live.reserve(parsed.size());
		std::size_t released = 0;

		for (const auto& record : parsed)
		{
			const auto key = DepotKey::getCachedKey(record.depotId);
			const bool matches =
			    inManagedScope(key, record.depotId)
			    && DepotQuarantineStore::fingerprintKey(key.key)
			           == record.keyFingerprint;
			if (!matches)
			{
				++released;
				continue;
			}
			if (g_registry.adopt(record.depotId, key.appId, key.key))
			{
				live.push_back(record);
			}
		}

		const bool changed = live.size() != parsed.size();
		{
			std::lock_guard<std::mutex> lock(g_recordsLock);
			g_records = live;
			if (changed) saveRecordsLocked();
		}

		if (!live.empty())
		{
			g_pLog->info("DepotQuarantine: restored %zu recorded DLC entr%s; "
			             "they stay out of this app's configuration\n",
			             live.size(), live.size() == 1 ? "y" : "ies");
		}
		if (released)
		{
			g_pLog->info("DepotQuarantine: released %zu recorded DLC entr%s "
			             "after a key change\n",
			             released, released == 1 ? "y" : "ies");
		}
	}

	// Classify the depot from the app's already-provisioned appinfo. The
	// planner's DlcAppId is unavailable here (and is not reported at all on a
	// DLC-only re-plan), so the persisted decision is derived from local
	// metadata instead. Returns 0 for base/shared content, which is never
	// recorded.
	uint32_t dlcAppIdFromProvisionedAppinfo(uint32_t appId, uint32_t depotId)
	{
		if (!appId || !depotId) return 0;

		std::ostringstream path;
		path << g_config.getDir().c_str() << "/cache/picsbuffer_" << appId
		     << ".bin";

		std::error_code ec;
		const auto file = path.str();
		if (!std::filesystem::is_regular_file(file, ec)) return 0;

		std::ifstream in(file, std::ios::in | std::ios::binary);
		if (!in.is_open()) return 0;
		std::ostringstream wire;
		wire << in.rdbuf();

		return AppInfoProvision::dlcAppIdForDepot(wire.str(), appId, depotId);
	}

	void recordQuarantine(uint32_t appId, uint32_t depotId, uint32_t dlcAppId,
	                      const std::string& key)
	{
		const DepotQuarantineStore::Record record{
		    appId, depotId, dlcAppId,
		    DepotQuarantineStore::fingerprintKey(key)};

		std::lock_guard<std::mutex> lock(g_recordsLock);
		if (!DepotQuarantineStore::upsert(g_records, record)) return;
		saveRecordsLocked();
		g_pLog->info("DepotQuarantine: recorded DLC app=%u depot=%u so it is "
		             "left out of the configuration from now on\n",
		             dlcAppId, depotId);
	}

	void observeUnpackFailure(void* context, const void* chunk,
	                          uint32_t location, uint32_t unpackResult)
	{
		if (!DepotQuarantine::isTargetFailure(location, unpackResult)) return;

		const uint32_t depotId = depotIdFromContext(context);
		if (!depotId || g_registry.contains(depotId)) return;

		const auto key = DepotKey::getCachedKey(depotId);
		if (g_registry.markFailure(depotId, key.appId,
		                           inManagedScope(key, depotId), key.key,
		                           unpackResult, chunkFingerprint(chunk)))
		{
			g_pLog->info(
			    "DepotQuarantine: app=%u depot=%u had %zu distinct chunk-unpack "
			    "failures; eligible DLC entry will be omitted on retry\n",
			    key.appId, depotId,
			    DepotQuarantine::kDistinctFailureThreshold);

			// Persist as soon as the decision is confirmed. Waiting for the
			// planner to report DlcAppId left the record unwritten on a
			// DLC-only re-plan, so the exclusion never armed and the depot kept
			// being retried.
			const uint32_t dlcAppId =
			    dlcAppIdFromProvisionedAppinfo(key.appId, depotId);
			if (dlcAppId)
			{
				recordQuarantine(key.appId, depotId, dlcAppId, key.key);
			}
			else
			{
				g_pLog->info("DepotQuarantine: depot %u is not advertised as "
				             "DLC content; leaving the configuration untouched\n",
				             depotId);
			}
		}
	}

	void hkOnChunkStack(void* context, void* chunk, const char* source,
	                    uint32_t location, const char* detail,
	                    uint32_t unpackResult)
	{
		try
		{
			observeUnpackFailure(context, chunk, location, unpackResult);
		}
		catch (...)
		{
			// This optional mitigation must never unwind into a Steam worker.
		}
		g_stack.orig(context, chunk, source, location, detail, unpackResult);
	}

	void __attribute__((regparm(3))) hkOnChunkReg(
	    void* context, void* chunk, const char* source, uint32_t location,
	    const char* detail, uint32_t unpackResult)
	{
		try
		{
			observeUnpackFailure(context, chunk, location, unpackResult);
		}
		catch (...)
		{
			// Preserve the original callback regardless of local policy failures.
		}
		g_reg.orig(context, chunk, source, location, detail, unpackResult);
	}

	template<typename Fn>
	bool install(Detour<Fn>& detour, Pattern_t& pattern, Fn hook)
	{
		if (pattern.address == LM_ADDRESS_BAD)
		{
			g_pLog->warn("DepotQuarantine: optional pattern %s unavailable\n",
			             pattern.name.c_str());
			return false;
		}

		detour.addr = pattern.address;
		detour.size = LM_HookCode(detour.addr,
		                          reinterpret_cast<lm_address_t>(hook),
		                          &detour.tramp);
		if (!detour.size || detour.tramp == LM_ADDRESS_BAD)
		{
			g_pLog->warn("DepotQuarantine: could not hook %s\n",
			             pattern.name.c_str());
			detour = {};
			return false;
		}
		MemHlp::fixPICThunkCall(pattern.name.c_str(), detour.addr, detour.tramp);
		detour.orig = reinterpret_cast<Fn>(detour.tramp);
		return true;
	}

	template<typename Fn>
	void uninstall(Detour<Fn>& detour)
	{
		if (detour.size && detour.addr != LM_ADDRESS_BAD
		    && detour.tramp != LM_ADDRESS_BAD)
		{
			LM_UnhookCode(detour.addr, detour.tramp, detour.size);
		}
		detour = {};
	}
}

namespace DepotQuarantine
{
	bool setup()
	{
		const bool stack = install(
		    g_stack, Patterns::CDepotDownloadMgr::OnChunkUnpackedStack,
		    &hkOnChunkStack);
		const bool reg = install(
		    g_reg, Patterns::CDepotDownloadMgr::OnChunkUnpackedReg,
		    &hkOnChunkReg);
		g_pLog->debug("DepotQuarantine: cdecl=%d regparm3=%d\n",
		              static_cast<int>(stack), static_cast<int>(reg));

		// Restore even when neither callback resolved: last session's decisions
		// are still valid and still need to stay out of the configuration.
		try
		{
			restorePersistedRecords();
		}
		catch (...)
		{
			g_pLog->warn("DepotQuarantine: could not restore recorded DLC "
			             "entries; they will be re-learned if they fail again\n");
		}
		return stack || reg;
	}

	void remove()
	{
		uninstall(g_stack);
		uninstall(g_reg);
	}

	bool shouldDropManagedDlc(uint32_t depotId, uint32_t dlcAppId)
	{
		try
		{
			if (!depotId || !dlcAppId || !g_registry.contains(depotId))
			{
				return false;
			}

			const auto key = DepotKey::getCachedKey(depotId);
			const bool scoped = inManagedScope(key, depotId);
			if (!g_registry.shouldDrop(depotId, dlcAppId, scoped, key.key))
			{
				return false;
			}

			// The planner is the only place that knows Steam's structured
			// DlcAppId for this depot, so it is where the decision is recorded.
			recordQuarantine(key.appId, depotId, dlcAppId, key.key);
			return true;
		}
		catch (...)
		{
			// Planner safety is fail-open: retain the depot on any local error.
			return false;
		}
	}

	std::unordered_set<uint32_t> package0Exclusions()
	{
		std::unordered_set<uint32_t> exclusions;
		try
		{
			std::vector<DepotQuarantineStore::Record> snapshot;
			{
				std::lock_guard<std::mutex> lock(g_recordsLock);
				snapshot = g_records;
			}

			for (const auto& record : snapshot)
			{
				const auto key = DepotKey::getCachedKey(record.depotId);
				if (!inManagedScope(key, record.depotId)) continue;
				if (DepotQuarantineStore::fingerprintKey(key.key)
				    != record.keyFingerprint)
				{
					continue;
				}
				exclusions.insert(record.dlcAppId);
				exclusions.insert(record.depotId);
			}
		}
		catch (...)
		{
			// Ownership injection is fail-open: on any local error inject
			// everything, exactly as before this mitigation existed.
			return {};
		}
		return exclusions;
	}
}
