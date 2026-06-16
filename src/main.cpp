#include <dlfcn.h>
#include "api.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "patterns.hpp"
#include "update.hpp"
#include "utils.hpp"

#include "feats/appinfo_provision.hpp"
#include "feats/appinfo_vdf.hpp"
#include "feats/depotkey.hpp"
#include "feats/manifestid.hpp"
#include "feats/packagepatch.hpp"
#include "feats/steamstub.hpp"

#include "libmem/libmem.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <memory>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>


static bool cleanEnvVar(const char* varName, const char* endsWith)
{
	char* var = getenv(varName);
	if (var == NULL)
		return false;

	auto splits = Utils::strsplit(var, ":");
	auto newEnv = std::string();

	for(unsigned int i = 0; i < splits.size(); i++)
	{
		auto split = splits.at(i);
		if (split.ends_with(endsWith))
		{
			g_pLog->debug("Removed %s from $%s\n", endsWith, varName);
			continue;
		}

		if(newEnv.size() > 0)
		{
			newEnv.append(":");
		}
		newEnv.append(split);
	}

	if(newEnv.size())
	{
		setenv(varName, newEnv.c_str(), true);
	}
	else
	{
		unsetenv(varName);
	}
	//g_pLog->debug("Set %s to %s\n", varName, newEnv.c_str());

	return true;
}

//Looking at /proc/self/maps it seems like this isn't needed for processes that aren't steam
//__attribute__((noreturn))
static void unload()
{
	Hooks::remove();

	//This is absolutely unnessecary for applications loading SLSsteam where it cancels from setup()
	//Would be nice to run have for failed load() attempts though 
	//lm_module_t mod;
	//if (LM_FindModule("SLSsteam.so", &mod))
	//{
	//	//TODO: Investigate crash ?
	//	//Possibly: Might be because we're unmapping what ever thread we're running in
	//	//munmap(reinterpret_cast<void*>(mod.base), mod.size);
	//}
	//exit(0);
}

//TODO: Remove when unload() works properly since it should not be needed anymore after that
static bool setupSuccess = false;

static void setup()
{
	lm_process_t proc {};
	if (!LM_GetProcess(&proc))
	{
		unload();
		return;
	}

	//Do not do anything in other processes
	if (strcmp(proc.name, "steam") != 0)
	{
		unload();
		return;
	}

	g_pLog = std::unique_ptr<CLog>(CLog::createDefaultLog());
	if (!g_pLog)
	{
		unload();
		return;
	}

	g_pLog->debug("SLSsteam loading in %s\n", proc.name);

	// Strip ourselves from $LD_AUDIT so child processes Steam spawns
	// (reaper, steamwebhelper, games) don't re-audit and re-run our
	// hooks in foreign processes.  setup() also early-outs on non-steam
	// processes, but cleaning the env is cheaper and avoids surprises.
	cleanEnvVar("LD_AUDIT", "SLSsteam.so");
	cleanEnvVar("LD_AUDIT", "library-inject.so");
	cleanEnvVar("LD_AUDIT", "libSLSsteam.so");
	cleanEnvVar("LD_AUDIT", "libSLS-library-inject.so");


	if(!g_config.init())
	{
		unload();
		return;
	}

	// Splice cached PICS buffers into appcache/appinfo.vdf before
	// Steam opens the file.  Each buffer was captured during a
	// previous session by `feats/pics.cpp::recvProductInfoResponse`.
	// The cache must already exist on disk; first-run installs need
	// a Steam restart so the buffers can be picked up.
	{
		const char* home = std::getenv("HOME");
		if (home)
		{
			static const char* steamRoots[] = {
				"/.steam/steam",
				"/.steam/debian-installation",
				"/.local/share/Steam",
			};
			for (const char* suffix : steamRoots)
			{
				const auto candidate = std::string(home) + suffix +
				    "/appcache/appinfo.vdf";
				if (std::filesystem::exists(candidate))
				{
					// We need DepotKey/ManifestId catalogues populated
					// from the user's Lua plugin BEFORE provisioning,
					// because AppInfoProvision drops depots without a
					// cached key (and pins manifest GIDs from the
					// catalogue).  Both importers are idempotent — a
					// second call from DepotKey::onStartup() / setup()
					// after Hooks are installed is a no-op.
					DepotKey::importLuaScripts();
					ManifestId::importLuaScripts();

					// First, fetch fresh PICS-equivalent buffers for
					// any AdditionalApps whose entry in appinfo.vdf
					// is missing depots (cold-start case).
					// Writes to <config>/cache/picsbuffer_*.{bin,yaml},
					// which the splice below then picks up.
					AppInfoProvision::provisionAllAddedApps(candidate);
					AppInfoVdf::injectAllCached(candidate);
					break;
				}
			}
		}
	}


	// Some distros honour LD_LIBRARY_PATH differently; make sure the
	// system lib dirs are searchable so library-inject.so's libcurl
	// redirect resolves.  Harmless append.
	{
		const char* cur = getenv("LD_LIBRARY_PATH");
		std::string ldLibPath = cur ? cur : "";
		ldLibPath.append(":/usr/lib:/usr/lib32");
		setenv("LD_LIBRARY_PATH", ldLibPath.c_str(), true);
	}

	Updater::init();

	setupSuccess = true;
}

static void load()
{
	if (!setupSuccess)
	{
		return;
	}

	// la_objopen fires load() once per audited module that opens — i.e. for
	// BOTH steamclient.so AND steamui.so. The hooking work below must run
	// exactly once PER PROCESS: the first pass overwrites the target
	// functions' prologues with detour jumps, so a second pass would re-scan
	// that already-patched memory, fail the prologue signatures ("Required
	// pattern not found"), and clobber the resolved addresses.
	//
	// A per-.so `static` flag is NOT enough: under LD_AUDIT glibc instantiates
	// the auditor (this .so) once per link-map namespace, and a Steam process
	// can have more than one namespace — so there are TWO copies of SLSsteam.so
	// in the process, each with its own `static`, each running load() against
	// the SAME (shared) steamclient.so. Confirmed on the affected machine: two
	// SLSsteam.so mappings in the minidump; load() ran twice with an
	// independent guard each time. The single-instance machines never hit it.
	//
	// So the one-shot has to be PROCESS-global, shared across those copies. A
	// per-pid advisory file lock is: kernel-level (the fd table is per-process,
	// so both copies see the same inode/lock), atomic against threads, and
	// auto-released when the process dies (no stale-lock / pid-reuse hazard).
	static bool loadDone = false;
	if (loadDone)
	{
		return;
	}

	//This should never happen, but better be safe than sorry in case I refactor someday
	if (!LM_FindModule("steamclient.so", &g_modSteamClient))
	{
		unload();
		return;
	}
	if (!LM_FindModule("steamui.so", &g_modSteamUI))
	{
		unload();
		return;
	}

	// Claim the process-wide one-shot AFTER both modules are confirmed (so a
	// genuine "the other module isn't mapped yet" retry on the next objopen
	// still works) and BEFORE the heavy work.
	{
		char lockPath[64];
		std::snprintf(lockPath, sizeof(lockPath), "/tmp/.slssteam.load.%d", getpid());
		const int lockFd = open(lockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (lockFd >= 0)
		{
			if (flock(lockFd, LOCK_EX | LOCK_NB) != 0)
			{
				// Another SLSsteam.so instance in this process already claimed
				// the hooking pass. Bail before re-scanning the hooked code.
				g_pLog->info("load: another instance already claimed the hooking pass (guard %p) -> skipping\n",
				             static_cast<void*>(&loadDone));
				close(lockFd);
				return;
			}
			// We won the lock; deliberately keep lockFd open for the lifetime
			// of the process so the lock is held (released only on exit).
		}
		// open() failure falls through: never block hooking on a broken /tmp.
	}
	g_pLog->info("load: claimed hooking pass (guard %p, pid %d)\n",
	             static_cast<void*>(&loadDone), getpid());
	loadDone = true;

	auto path = std::filesystem::path(g_modSteamClient.path);
	auto dir = path.parent_path();

	g_pLog->info
	(
		"steamclient.so loaded from %s/%s at %p to %p\n",
		dir.filename().c_str(),
		path.filename().c_str(),
		g_modSteamClient.base,
		g_modSteamClient.end
	);
	g_pLog->info
	(
		"steamui.so loaded at %p to %p\n",
		g_modSteamUI.base,
		g_modSteamUI.end
	);

	// Log the GNU build-id of every relevant module so "which build is the
	// user actually running?" is answerable straight from ~/.SLSsteam.log
	// (the previous diagnosis required asking the user to run readelf).
	// libmem resolves modules from /proc/self/maps, so this sees modules in
	// the main link namespace (steamclient.so, cloud_redirect.so) too, not
	// just our LD_AUDIT namespace. Missing modules (e.g. no CloudRedirect)
	// are reported as such rather than skipped.
	{
		const auto logBuildId = [](const char* modName)
		{
			lm_module_t mod {};
			if (!LM_FindModule(modName, &mod))
			{
				g_pLog->info("buildid: %-18s (not loaded)\n", modName);
				return;
			}
			const std::string id = Utils::getBuildId(mod.path);
			g_pLog->info("buildid: %-18s %s\n", modName,
			             id.empty() ? "(no build-id)" : id.c_str());
		};
		logBuildId("SLSsteam.so");
		logBuildId("library-inject.so");
		logBuildId("steamclient.so");
		logBuildId("steamui.so");
		logBuildId("cloud_redirect.so");
	}

	if (!Updater::verifySafeModeHash())
	{
		if (g_config.safeMode.get())
		{
			g_pLog->warn("Unknown steamclient.so hash! Aborting...");
			unload();
			return;
		}
		else if (g_config.warnHashMissmatch.get())
		{
			g_pLog->warn("steamclient.so hash missmatch! Please update :)");
		}
	}

	if (!Patterns::init())
	{
		g_pLog->warn("Failed to find all patterns! Aborting...");
		return;
	}

	if (!Hooks::setup())
	{
		unload();
		return;
	}

	SLSAPI::init();

	// Resolve SLSsteam.so's own install root via libmem so the
	// runtime helpers (wrapper integration script + bundled tools)
	// can be located relative to the .so. Falls back to a no-op
	// silently when those aren't shipped (e.g. development builds
	// running from the obj tree).
	{
		lm_module_t selfMod {};
		const char* candidates[] = {
			"SLSsteam.so", "libSLSsteam.so", nullptr
		};
		bool found = false;
		for (const char** name = candidates; *name; ++name)
		{
			if (LM_FindModule(*name, &selfMod))
			{
				found = true;
				break;
			}
		}
		if (found)
		{
			auto root = std::filesystem::path(selfMod.path).parent_path().string();
			SteamStub::setup(root.c_str());
			// Background-warm the dedicated Wine prefix so the user
			// doesn't pay the first-run wineboot cost when launching
			// an app that goes through the wrapper-integration helper.
			// Detached worker; onLaunchApp will join on this before
			// invoking the helper.
			SteamStub::warmupAsync();
		}
		else
		{
			g_pLog->debug("SteamStub: could not resolve SLSsteam.so path; feature disabled\n");
		}
	}

	// Import Lua scripts and provision manifest files.  Must run
	// AFTER setup so g_config.getDir() is valid and AFTER hooks so
	// g_pLog is alive.
	DepotKey::onStartup();
	ManifestId::importLuaScripts();

	// Re-inject AdditionalApps into package 0 in case Steam already
	// loaded it before our hook was placed.  No-op when the
	// LoadPackage detour has already seeded the same ids.
	{
		const auto added = g_config.addedAppIds.get();
		std::vector<uint32_t> ids(added.begin(), added.end());

		// Also inject the DLC appids advertised by each AddedApp's
		// provisioned appinfo (extended.listofdlc / depots.*.dlcappid).
		// Steam's install planner only schedules a `dlcappid`-tagged
		// depot when the DLC's appid is present in package 0's AppIdVec
		// — ownership alone is not enough (proven on the VM 2026-06-05
		// with Binding of Isaac 250900: the base installed but its DLC
		// depots were filtered out until the DLC ids were in package 0).
		// These are NOT added to addedAppIds, so they skip the per-app
		// provisioning path (a DLC appid has no own depots).  Their
		// depots are already eligible (depot keys recorded under the
		// base) and their manifests already stage via PICS recv.
		//
		// Register them with PackagePatch so the LoadPackage detour
		// keeps re-injecting them across package-0 reloads (e.g. the
		// reload the license reconcile triggers), then do the one-shot
		// manual inject for the case Steam already loaded package 0.
		const auto dlcIds = AppInfoProvision::collectDlcAppIdsForAddedApps();
		PackagePatch::setExtraAppIds(dlcIds);
		ids.insert(ids.end(), dlcIds.begin(), dlcIds.end());

		if (!ids.empty())
		{
			PackagePatch::injectIntoPackage0(ids);
		}
	}

	if (g_config.notifyInit.get())
	{
		const auto now = std::chrono::time_point{std::chrono::system_clock::now()};
		const auto ymd = std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(now)};

		//Funsy easter egg :)
		if (static_cast<unsigned int>(ymd.month()) == 2 && static_cast<unsigned int>(ymd.day()) == 22)
		{
			g_pLog->notify("Happy birthday SLSsteam!");
		}
		else
		{
			g_pLog->notify("Loaded successfully");
		}
	}
}

#include <thread>
#include <chrono>
#include <link.h>

// ───────────────────────────────────────────────────────────────────────
// Injection model: LD_AUDIT (rtld-audit).
//
// SLSsteam is loaded as an audit module via
//   LD_AUDIT="library-inject.so:SLSsteam.so"
// which places it in the dynamic linker's *auditing* link namespace,
// separate from the application's namespace.  This is essential: the .so
// statically links protobuf / yaml-cpp / an old-ABI libstdc++ and therefore
// carries thousands of those symbols.  In the audit namespace they are
// invisible to the application, so they cannot interpose on the copies
// Steam's own libraries resolve at runtime.
//
// We tried LD_PRELOAD instead; it puts those symbols in the process-global
// scope, Steam binds to OUR protobuf/std copies, and the resulting ABI
// mismatch aborts Steam during store load (reproduced on Pop!_OS).  Hiding
// the symbols isn't viable either: a `local:*` version script collapses
// yaml-cpp's C++ vague-linkage and crashes config parsing.  LD_AUDIT is the
// upstream-proven design and sidesteps the whole problem.
//
// la_objsearch/la_preinit/la_objopen run in every audited process; setup()
// itself bails out unless the process is the main "steam" binary, so we do
// nothing inside steamwebhelper (whose Chromium sandbox would kill us).
// We deliberately do NOT spawn a background polling thread: la_objopen
// fires synchronously the moment steamclient.so/steamui.so are mapped,
// which both avoids the thread-vs-audit glibc TLS issues and lets us patch
// the target functions while they are still cold (before Steam's worker
// threads call them).
// ───────────────────────────────────────────────────────────────────────

extern "C" unsigned int la_version(unsigned int)
{
	return LAV_CURRENT;
}

extern "C" unsigned int la_objopen(struct link_map* map,
                                   __attribute__((unused)) Lmid_t lmid,
                                   __attribute__((unused)) uintptr_t* cookie)
{
	if (map && map->l_name &&
	    (std::string(map->l_name).ends_with("/steamclient.so") ||
	     std::string(map->l_name).ends_with("/steamui.so")))
	{
		if (!setupSuccess)
		{
			setup();
		}
		load();
	}

	return 0;
}

extern "C" void la_preinit(__attribute__((unused)) uintptr_t* cookie)
{
	setup();
}
