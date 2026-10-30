#include <dlfcn.h>
#include "ascii.hpp"
#include "sdk/steam.hpp"
#include "api.hpp"
#include "runtimedir.hpp"
#include "audit_log.hpp"
#include "audit_policy.hpp"
#include "audit_symbols.hpp"
#include "config.hpp"
#include "decompiler.hpp"
#include "globals.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "lua.hpp"
#include "bootprof.hpp"
#include "ownerwork.hpp"
#include "patterns.hpp"
#include "runtime_attestation.hpp"
#include "utils.hpp"
#include "utils/ManifestFetch.hpp"
#include "utils/process_lock.hpp"

#include "feats/achievements.hpp"
#include "feats/appinfo_provision.hpp"
#include "feats/provision_schedule.hpp"
#include "feats/appinfo_vdf.hpp"
#include "feats/cmclient.hpp"
#include "feats/apps.hpp"
#include "feats/cefport.hpp"
#include "feats/depotkey.hpp"
#include "feats/hotreload.hpp"
#include "feats/manifestdonor.hpp"
#include "feats/manifestcode.hpp"
#include "feats/steamstub.hpp"
#include "feats/stats_policy.hpp"
#include "vftableinfo.hpp"

#include "libmem/libmem.h"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <memory>
#include <mutex>
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

	for (unsigned int i = 0; i < splits.size(); i++)
	{
		auto split = splits.at(i);
		if (split.ends_with(endsWith))
		{
			LOG_DEBUG("Removed %s from $%s\n", endsWith, varName);
			continue;
		}

		if (newEnv.size() > 0)
		{
			newEnv.append(":");
		}
		newEnv.append(split);
	}

	if (newEnv.size())
	{
		setenv(varName, newEnv.c_str(), true);
	}
	else
	{
		unsetenv(varName);
	}
	//LOG_DEBUG("Set %s to %s\n", varName, newEnv.c_str());

	return true;
}

//Looking at /proc/self/maps it seems like this isn't needed for processes that aren't steam
//__attribute__((noreturn))
static void unload()
{
	// An audited non-Steam process returns from setup before logging/hooks
	// exist. Hooks::remove logs, so this early no-op must precede teardown.
	if (!g_pLog) return;
	ManifestDonor::stop();
	ManifestCode::resetSession();
	HotReload::shutdown();
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
// setup() is called from each audited link-map namespace.  Keep the lock alive
// for the whole client lifetime so duplicate auditor instances cannot repeat
// config watchers, provisioning, or appinfo writes before load() gets a chance
// to claim its own hook-placement guard.
static std::unique_ptr<ProcessLock::FileLock> g_setupLock;

// CEF debug port for this client session, resolved once in setup() and inherited
// by every webhelper fork, so Lumen keeps one port across webhelper restarts.
static uint16_t g_cefSessionPort = 0;

// When true, we deliberately leave Steam's CEF endpoint on its hard-coded 8080
// instead of rewriting it to an ephemeral port. Set in setup() when Decky
// Loader is present: Decky's injector is hard-coded to 8080 and can't follow
// our port, so we share 8080 (Lumen falls back to it). This gates BOTH the
// setup() port pick and the exec-time rewrite (cefRewriteArgv).
static bool g_cefKeepDefaultPort = false;

// Identity of THIS Steam client, captured in setup() (i.e. inside the client
// itself, before it forks anything) and stamped into the CEF port contract when
// the exec hook publishes it. It lets the Lumen sidecar tell a live contract
// from a leftover one written by a previous session: the pid must still exist
// and must still have this start time. Captured here — not at publish time —
// because the exec hook runs in a fork child, whose pid is not the client's.
static long g_clientPid = 0;
static unsigned long long g_clientStartTicks = 0;

// The contract text and path, formatted in setup() so the exec hook can
// publish it from the fork child with plain system calls.
static CefPort::PreparedContract g_cefContract;

// Audit callbacks and post-fork exec wrappers must not consult C++ logging
// state.  setup() opens this descriptor before any webhelper fork; callbacks
// only use write(2)-style operations against the inherited fd. Steam may close
// inherited descriptors in the fork child, so keep a fixed path for an
// async-signal-safe O_APPEND reopen at the last possible moment.
static AuditLog::Sink g_rawLog;
static char g_rawLogPath[4096] = {};
static std::atomic<bool> g_auditDiagnosticsEnabled{false};
static std::atomic<bool> g_auditPolicyReady{false};
static std::atomic<bool> g_auditBindAll{true};

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
	//Won't happen, log throws a runtime exception when creation fails.
	//But just in case I decide to refactor one day
	if (!g_pLog)
	{
		unload();
		return;
	}
	BootProf::Span setupProfile(g_pLog.get(), "setup");

	// Every auditor namespace gets its own copy of these statics. A secondary
	// namespace may still dispatch an exec wrapper after the process-wide setup
	// lock is owned by another copy, so remember the path and open the
	// async-signal-safe diagnostic fd before that lock can reject this instance.
	if (g_rawLogPath[0] == '\0' && !g_pLog->path.empty())
	{
		std::snprintf(g_rawLogPath, sizeof(g_rawLogPath), "%s", g_pLog->path.c_str());
	}
	if (g_rawLog.fd < 0 && g_rawLogPath[0] != '\0')
	{
		(void)AuditLog::reopen(g_rawLog, g_rawLogPath);
	}

	// Publish the environment-only audit policy before taking the process-wide
	// setup lock. Each audit namespace has its own statics; a secondary copy
	// still needs to return narrow flags even when it skips the heavier setup.
	const bool narrowAudit = AuditBinding::narrowValueEnabled(
		std::getenv("SLSSTEAM_AUDIT_NARROW"));
	const bool bindAllAudit = !narrowAudit
		|| AuditBinding::bindAllValueEnabled(
			std::getenv("SLSSTEAM_AUDIT_BINDALL"));
	g_auditBindAll.store(bindAllAudit, std::memory_order_release);
	g_auditPolicyReady.store(true, std::memory_order_release);

	g_setupLock = std::make_unique<ProcessLock::FileLock>(
		ProcessLock::perProcessPath("setup"));
	if (!g_setupLock->acquired())
	{
		// Skip only when another auditor namespace holds our own private lock; an
		// unusable lock must not stop setup, or a planted file could disable injection.
		if (g_setupLock->usable())
		{
			g_pLog->info("setup: another auditor namespace already initialized this process -> skipping\n");
			g_setupLock.reset();
			unload();
			return;
		}
		g_pLog->info("setup: no usable setup lock -> continuing without it\n");
		g_setupLock.reset();
	}

	g_pLog->debug("SLSsteam loading in %s\n", proc.name);
	// Owner-IPC-thread handoff for watcher-originated Steam-owned work.
	// Reads its env overrides; starts no thread, touches no Steam memory.
	OwnerWork::init();

	// Client identity for the CEF port contract (see g_clientPid).
	g_clientPid = static_cast<long>(getpid());
	g_clientStartTicks = CefPort::readProcStartTicks(g_clientPid);

	// Strip ourselves from $LD_AUDIT so child processes Steam spawns
	// (reaper, steamwebhelper, games) don't re-audit and re-run our
	// hooks in foreign processes.  setup() also early-outs on non-steam
	// processes, but cleaning the env is cheaper and avoids surprises.
	cleanEnvVar("LD_AUDIT", "SLSsteam.so");
	cleanEnvVar("LD_AUDIT", "library-inject.so");
	cleanEnvVar("LD_AUDIT", "libSLSsteam.so");
	cleanEnvVar("LD_AUDIT", "libSLS-library-inject.so");


	if (!g_config.init())
	{
		unload();
		return;
	}

	// This is the last safe setup-time window before Steam's ConfigStore
	// writers become active. Keep the config.vdf mutation here; load() runs
	// after preinit and must not publish a stale snapshot over Steam's state.
	DepotKey::disableShaderCache();

	g_auditDiagnosticsEnabled.store(g_config.extendedLogging.get(),
	                               std::memory_order_release);

	// Decide this session's CEF debug port ONCE, as EARLY as possible (this is
	// la_preinit, before the client can spawn the webhelper) so g_cefSessionPort
	// is already set when the exec hook fires and stays fixed across webhelper
	// restarts. We deliberately do NOT write the contract file here: at login
	// autostart two Steam instances can run setup() concurrently, and only the
	// one that wins Steam's single-instance race goes on to spawn a webhelper.
	// Writing the contract here would let the LOSING instance (which picks a
	// different free port, then exits before launching anything) overwrite it
	// with a port nothing ends up listening on; the Lumen sidecar would then
	// connect to a dead port and never inject. So we only resolve the port now
	// and publish it later, from the exec hook, when THIS tree actually launches
	// the webhelper (see cefRewriteArgv). resolveSessionPortNoPersist reuses a
	// still-bindable port from a previous session, else picks a fresh one.
	//
	// EXCEPTION (Decky coexistence): Decky Loader's injector is hard-coded to
	// localhost:8080 and runs as a persistent daemon, so it can't follow our
	// ephemeral port. When Decky is installed we leave CEF on 8080 (no rewrite,
	// no contract) and let Lumen fall back to 8080; both share the endpoint,
	// since several CDP clients can attach to one CEF target. We also drop any
	// STALE contract from a previous ephemeral session so Lumen doesn't connect
	// to a dead port instead of falling back to 8080.
	if (CefPort::deckyPresent())
	{
		g_cefKeepDefaultPort = true;
		g_cefSessionPort = 0;
		CefPort::removeContract(CefPort::contractPath());
		g_pLog->info("CEF: Decky Loader detected -> keeping debug port on 8080 (not rewriting); Lumen + Decky share it\n");
	}
	else
	{
		g_cefSessionPort = CefPort::resolveSessionPortNoPersist(CefPort::contractPath());
		// Without a contract Lumen could not follow the port, so keep 8080.
		if (g_cefSessionPort != 0 &&
		    !CefPort::prepareContract(g_cefContract, CefPort::contractPath(),
		                              g_cefSessionPort, g_clientPid, g_clientStartTicks))
		{
			g_pLog->warn("CEF: cannot prepare the port contract at '%s'; Steam will keep 8080\n",
			             CefPort::contractPath().c_str());
			g_cefSessionPort = 0;
		}
		else if (g_cefSessionPort != 0)
		{
			g_pLog->info("CEF: remote-debugging port -> %u (frees 8080); published to %s "
			             "(owner pid %ld, start %llu) when this client launches the webhelper\n",
			             g_cefSessionPort, CefPort::contractPath().c_str(), g_clientPid,
			             g_clientStartTicks);
		}
		else
		{
			g_pLog->warn("CEF: could not pick a debug port; Steam will keep 8080\n");
		}
	}

	// Splice cached PICS buffers into appcache/appinfo.vdf before Steam
	// opens the file. CM refreshes for already-provisioned apps are not on
	// this path when AsyncProvision is enabled; PICS recv starts that worker
	// from a real Steam worker thread. A missing cache pair is the exception:
	// provision it synchronously here before the splice so the first restart
	// sees the appinfo entry in Steam's initial in-memory cache.
	{
		const bool asyncProvision = AppInfoProvision::asyncProvisioningEnabled();

		// Loading libcurl's TLS dependency tree from the first PICS worker
		// callback can crash the 32-bit Steam client. Warm it up here while
		// setup() still owns the safe preinit call stack; the callback then
		// only reuses already-resolved function pointers. This intentionally
		// depends neither on appinfo.vdf being discoverable nor on managed apps
		// existing: a later hot-add starts the same worker, and the PICS
		// callback must never be the first libcurl loader.
		if (!CmClient::prepareForThreadedFetch())
		{
			g_pLog->warn("CmClient: unable to preload libcurl before PICS worker\n");
		}

		// Runtime provisioning defers Proton mappings because Steam's live
		// ConfigStore writers do not participate in our advisory lock. Apply
		// the pending set now, while setup() still owns the preinit window.
		AppInfoProvision::flushPendingProtonMappings();

		const auto candidate = AppInfoVdf::findExistingPath();
		if (candidate.empty())
		{
			// Provisioning, the appinfo splice and the DLC metadata splice all
			// live inside this block, so an undiscoverable appinfo.vdf silently
			// skipped every step that makes a managed app visible. Steam has not
			// been bootstrapped (no appcache yet) or its root is not one of the
			// supported layouts; either way the user needs to know why nothing
			// happened.
			g_pLog->warn(
			    "AppInfoProvision: no Steam appinfo cache found; managed games "
			    "cannot be prepared this boot (open Steam once to create it)\n");
		}
		else
		{
			const auto preinitAction =
			    AppInfoProvision::preinitProvisionAction(asyncProvision);
			if (preinitAction ==
			    AppInfoProvision::PreinitProvisionAction::SynchronousProvision)
			{
				// Async provisioning is off (AsyncProvision: no or
				// SLSSTEAM_ASYNC_PROVISION=0): refresh every managed app
				// before the splice.
				DepotKey::importLuaScripts();
				BootProf::Span provisionProfile(g_pLog.get(), "provision.sync_preinit");
				AppInfoProvision::provisionAllAddedApps();
			}
			else
			{
				// Async mode still keeps the genuine first-run path synchronous,
				// but only for apps without a complete validated cache pair. This
				// is deliberately before the appinfo splice: writing the pair from
				// PICS after Steam has loaded appinfo would require a second restart.
				DepotKey::importLuaScripts();
				BootProf::Span provisionProfile(g_pLog.get(), "provision.cold_preinit");
				const auto startupCandidates = g_config.managedAppIds.get();
				AppInfoProvision::provisionColdStartApps(startupCandidates);
			}

			BootProf::Span spliceProfile(g_pLog.get(), "appinfo.splice_preinit");
			AppInfoVdf::injectAllCached(candidate);
			std::vector<AppInfoVdf::MetadataApp> dlcMetadataApps;
			std::unordered_set<std::uint32_t> seenDlcMetadataApps;
			for (const std::uint32_t baseAppId :
				g_config.managedAppIds.get())
			{
				DlcMetadata::CacheRecord record;
				if (!AppInfoProvision::readValidatedDlcMetadataCache(
					baseAppId, 0, record)) continue;
				for (const auto& app : record.apps)
				{
					if (!seenDlcMetadataApps.insert(app.appid).second) continue;
					dlcMetadataApps.push_back({
						.appid = app.appid,
						.changeNumber = app.changeNumber,
						.sha = app.sha,
						.wireBuffer = app.wireBuffer,
					});
				}
			}
			if (!dlcMetadataApps.empty())
			{
				(void)AppInfoVdf::injectValidatedMetadataApps(
					candidate, dlcMetadataApps);
			}
		}
	}


	setupSuccess = true;
}

static void load()
{
	if (!setupSuccess)
	{
		return;
	}

	try
	{
	// Hook once per process: a second pass would re-scan already-patched prologues.
	// load() runs per module in every audit-namespace copy, hence the per-pid flock.
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
	// steamui.so maps after steamclient.so; the next la_objopen retries.
	if (!LM_FindModule("steamui.so", &g_modSteamUI))
	{
		return;
	}

	// Claim the process-wide one-shot AFTER both modules are confirmed (so a
	// genuine "the other module isn't mapped yet" retry on the next objopen
	// still works) and BEFORE the heavy work.
	{
		// Fail open: ignore a lock that is not our private file (cost: a redundant pass).
		const std::string lockDir =
			RuntimeDir::resolveBase(getenv("XDG_RUNTIME_DIR"), getenv("HOME"));
		if (!lockDir.empty() && RuntimeDir::ensureDir(lockDir))
		{
			const std::string lockPath =
				lockDir + "/load." + std::to_string(getpid());
			const int lockFd = open(lockPath.c_str(),
			                        O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (lockFd >= 0 && RuntimeDir::descriptorIsPrivate(lockFd))
			{
				if (flock(lockFd, LOCK_EX | LOCK_NB) != 0)
				{
					// Another SLSsteam.so instance in this process already
					// claimed the hooking pass. Bail before re-scanning the
					// hooked code.
					g_pLog->info("load: another instance already claimed the hooking pass (guard %p) -> skipping\n",
					             static_cast<void*>(&loadDone));
					close(lockFd);
					return;
				}
				// We won the lock; deliberately keep lockFd open for the lifetime
				// of the process so the lock is held (released only on exit).
			}
			else if (lockFd >= 0)
			{
				g_pLog->info("load: load lock is not a private file -> proceeding without it\n");
				close(lockFd);
			}
		}
		// A missing or unusable lock falls through: never block hooking on it.
	}
	g_pLog->info("load: claimed hooking pass (guard %p, pid %d)\n",
	             static_cast<void*>(&loadDone), getpid());
	loadDone = true;

	// Runtime evidence is opt-in and used only by the isolated repair runner.
	// Bind every event to the exact SLSsteam.so bytes that this process loaded;
	// a source diff or a successful log line alone is not sufficient evidence.
	if (const char* attestationPath = getenv("SLSSTEAM_ATTESTATION_FILE");
	    attestationPath != nullptr && attestationPath[0] != '\0')
	{
		lm_module_t selfModule {};
		if (!LM_FindModule("SLSsteam.so", &selfModule))
		{
			g_pLog->warn("attestation: unable to identify loaded SLSsteam.so\n");
		}
		else
		{
			const std::string candidateId = Utils::getFileSHA256(selfModule.path);
			if (candidateId.empty()
			    || !RuntimeAttestation::initialize(candidateId))
			{
				g_pLog->warn("attestation: failed to initialize runtime evidence\n");
			}
		}
	}

	auto path = std::filesystem::path(g_modSteamClient.path);
	auto dir = path.parent_path();

	LOG_INFO
	(
		"steamclient.so loaded from %s/%s at 0x%x to 0x%x\n",
		dir.filename().c_str(),
		path.filename().c_str(),
		g_modSteamClient.base,
		g_modSteamClient.end
	);
	LOG_INFO
	(
		"steamui.so loaded at 0x%x to 0x%x\n",
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
		const auto logBuildId = [](const char* modName, const char* component)
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
			if (RuntimeAttestation::enabled())
			{
				RuntimeAttestation::emit
				(
					"module-loaded",
					{
						RuntimeAttestation::Field::text("module", component),
						RuntimeAttestation::Field::text("module_file", modName),
						RuntimeAttestation::Field::text("build_id", id),
						RuntimeAttestation::Field::text
						(
							"sha256", Utils::getFileSHA256(mod.path)
						),
						RuntimeAttestation::Field::number("base", mod.base),
						RuntimeAttestation::Field::number("size", mod.size),
					}
				);
			}
		};
		logBuildId("SLSsteam.so", "candidate");
		logBuildId("steamclient.so", "steamclient");
		logBuildId("steamui.so", "steamui");
		logBuildId("cloud_redirect.so", "cloud-redirect");
	}


	if (!Steam::init())
	{
		LOG_NOTIFYERROR("Failed to find steam exports!\n");
		g_pLog->notifyUser(UserMsg::InitializationFailed);
		return;
	}

	{
		BootProf::Span vftProfile(g_pLog.get(), "vftable_scan");
		if (!VFTIndexes::init())
		{
			g_pLog->warn("Failed to parse VFTables! Aborting...\n");
			g_pLog->notifyUser(UserMsg::InitializationFailed);
			return;
		}
	}

	{
		BootProf::Span patternProfile(g_pLog.get(), "pattern_scan");
		if (!Patterns::init())
		{
			g_pLog->warn("Failed to find all patterns! Aborting...");
			g_pLog->notifyUser(UserMsg::InitializationFailed);
			return;
		}
	}

	if (!Hooks::setup())
	{
		// A required hook failed to resolve or to be placed: Steam runs unmodified,
		// so log it (otherwise it only shows up as missing ownership).
		g_pLog->warn("Hooks::setup() reported failure; aborting injection and "
		             "running unmodified\n");
		g_pLog->notifyUser(UserMsg::InitializationFailed);
		unload();
		return;
	}

	SLSAPI::init();
	// Lua plugins resolve VFTables at runtime, so the decompiler's tables stay
	// while plugins may run (upstream keeps them always).
	if (!g_config.plugins.get())
	{
		Decompiler::cleanUp();
	}

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
			// No startup Wine prewarm: executable processing is opt-in per
			// app via the `--steamless` launch option (default runs the stub
			// in place), so the vast majority of launches never touch the
			// Wine prefix. onLaunchApp warms it on demand the first time an
			// app actually opts in, so those users still get a warmed prefix
			// before processing runs.
		}
		else
		{
			g_pLog->debug("SteamStub: could not resolve SLSsteam.so path; feature disabled\n");
		}
	}

	// Import Lua scripts and provision manifest files.  Must run
	// AFTER setup so g_config.getDir() is valid and AFTER hooks so
	// g_pLog is alive.
	{
		BootProf::Span importProfile(g_pLog.get(), "lua_imports");
		ManifestFetch::clearOfflineFlags();
		DepotKey::onStartup();
		Achievements::loadOwners();
	}

	// Keep the broader local DLC set used by launch-time checks. Runtime
	// package ownership itself is published below from the managed source union.
	{
		std::lock_guard<std::mutex> passLock(
		    AppInfoProvision::provisioningPassMutex());
		bool dlcCollectionComplete = false;
		const auto dlcIds = AppInfoProvision::collectDlcAppIdsForAddedApps(
		    &dlcCollectionComplete);
		if (dlcCollectionComplete)
		{
			Apps::setDiscoveredAppDlcIds(dlcIds.appDlc);
		}
	}

	// Hooks are now installed and source discovery/imports are complete. Bind
	// the appinfo guard to its process-lifetime Store and submit the first
	// complete package snapshot. The legacy config AdditionalApps list is not
	// part of this runtime state.
	HotReload::initialize();

	// Opt-in Lua plugins start last, with every hook and the hot-add path in
	// place. With Plugins: no this returns before touching anything.
	Lua::init(true);

	if (g_config.notifyInit.get())
	{
		const auto now = std::chrono::time_point { std::chrono::system_clock::now() };
		const auto ymd = std::chrono::year_month_day { std::chrono::floor<std::chrono::days>(now) };

		//Funsy easter egg :)
		if (static_cast<unsigned int>(ymd.month()) == 2 && static_cast<unsigned int>(ymd.day()) == 22)
		{
			g_pLog->notify("Happy birthday SLSsteam!");
			g_pLog->notifyUser(UserMsg::BirthdayGreeting);
		}
		else
		{
			g_pLog->notify("Loaded successfully");
			g_pLog->notifyUser(UserMsg::LoadSuccess);
		}
	}
	}
	catch (const std::exception& e)
	{
		// Tear the hooks down and say why, so Steam runs unmodified visibly.
		// The handler often runs after a bad_alloc: logging may throw too.
		try
		{
			if (g_pLog) g_pLog->warn("load() aborted by exception: %s\n", e.what());
		}
		catch (...)
		{
		}
		try
		{
			unload();
		}
		catch (...)
		{
		}
		return;
	}
	catch (...)
	{
		try
		{
			if (g_pLog) g_pLog->warn("%s", "load() aborted by a non-std exception\n");
		}
		catch (...)
		{
		}
		try
		{
			unload();
		}
		catch (...)
		{
		}
		return;
	}
}

#include <thread>
#include <chrono>
#include <link.h>
#include <spawn.h>

// ───────────────────────────────────────────────────────────────────────
// Injection model: LD_AUDIT (rtld-audit).
//
// SLSsteam is loaded as the only audit module via
//   LD_AUDIT="SLSsteam.so"
// (with a second audit module loaded, any exception thrown in this module
// aborts the client, even one caught in the same function),
// which places it in the dynamic linker's *auditing* link namespace,
// separate from the application's namespace.  This is essential: the .so
// statically links protobuf / yaml-cpp / an old-ABI libstdc++ and therefore
// carries thousands of those symbols.  In the audit namespace they are
// invisible to the application, so they cannot interpose on the copies
// Steam's own libraries resolve at runtime.
//
// LD_PRELOAD is not an option: it puts those symbols in the process-global
// scope, Steam binds to OUR protobuf/std copies, and the resulting ABI
// mismatch aborts Steam during store load.  Hiding the symbols isn't viable
// either: a `local:*` version script collapses yaml-cpp's C++ vague-linkage
// and crashes config parsing.  LD_AUDIT avoids both problems.
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

// Client-namespace libcurl lookups (CloudRedirect) get the host's 32-bit copy
// instead of the runtime's pinned one. The loader never calls this for lookups
// made by an audit module, so SLSsteam's own libcurl is unaffected. Callbacks
// run under the loader's lock and it copies the result, so one buffer serves.
extern "C" char* la_objsearch(const char* name,
                              __attribute__((unused)) uintptr_t* cookie,
                              unsigned int flag)
{
	static char path[PATH_MAX];
	if (flag != LA_SER_ORIG)
		return const_cast<char*>(name);
	return const_cast<char*>(AuditBinding::systemLibraryFor(
		name, AuditBinding::systemLibraryDirs, path));
}

// ───────────────────────────────────────────────────────────────────────
// CEF debug-port rewrite (frees TCP 8080).
//
// Steam launches the CEF webhelper with a HARD-CODED
// `--remote-debugging-port=8080` (the .cef-enable-remote-debugging flag's
// content is ignored, and Steam restores its launcher scripts from bootstrap
// on every boot, so static edits do not persist).
// That squats on TCP 8080. We rewrite the argument in flight to a free
// loopback port and publish it to ~/.local/share/Lumen/cef_port for the Lumen
// sidecar to read.
//
// Mechanism: rtld-audit symbol binding. la_objopen flags every object
// BINDFROM|BINDTO so la_symbind32 is invoked for each binding; for the exec
// family we return a wrapper that rewrites argv, otherwise the original
// address (a no-op redirect). The client launches the webhelper via execv,
// with the switch embedded inside a `sh -c "exec steamwebhelper.sh ..."`
// wrapper string, so a substring rewrite is used (CefPort::rewriteArgv).
// This lives in SLSsteam.so itself — no extra preloaded library.
// ───────────────────────────────────────────────────────────────────────
namespace
{
	using execv_t  = int (*)(const char*, char* const[]);
	using execve_t = int (*)(const char*, char* const[], char* const[]);
	using spawn_t  = int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
	                         const posix_spawnattr_t*, char* const[], char* const[]);

	execv_t  g_realExecv     = nullptr;
	execv_t  g_realExecvp    = nullptr;
	execve_t g_realExecve    = nullptr;
	execve_t g_realExecvpe   = nullptr;
	spawn_t  g_realSpawn     = nullptr;
	spawn_t  g_realSpawnp    = nullptr;

	std::atomic<unsigned int> g_auditDiagnosticMask {0};

	const char* auditSymbolName(AuditBinding::Symbol symbol) noexcept
	{
		switch (symbol)
		{
			case AuditBinding::Symbol::Execv: return "execv";
			case AuditBinding::Symbol::Execvp: return "execvp";
			case AuditBinding::Symbol::Execve: return "execve";
			case AuditBinding::Symbol::Execvpe: return "execvpe";
			case AuditBinding::Symbol::PosixSpawn: return "posix_spawn";
			case AuditBinding::Symbol::PosixSpawnp: return "posix_spawnp";
			case AuditBinding::Symbol::LocalStatsEpoch: return "slsteam_local_stats_epoch_v1";
			default: return "<unknown>";
		}
	}
	const char* auditObjectName(uintptr_t* cookie) noexcept
	{
		if (cookie == nullptr || *cookie == 0)
			return "<unknown>";
		const auto* map = reinterpret_cast<const link_map*>(*cookie);
		if (map == nullptr || map->l_name == nullptr || map->l_name[0] == '\0')
			return "<main executable>";
		return map->l_name;
	}

	void writeRawLog(const char* text) noexcept
	{
		AuditLog::write(g_rawLog, g_rawLogPath, text);
	}

	void writeRawUnsigned(unsigned int value) noexcept
	{
		char digits[11] = {};
		size_t pos = sizeof(digits) - 1;
		do
		{
			digits[--pos] = static_cast<char>('0' + (value % 10));
			value /= 10;
		}
		while (value != 0);
		writeRawLog(digits + pos);
	}

	void logAuditBinding(AuditBinding::Symbol symbol,
	                     uintptr_t* refcook,
	                     uintptr_t* defcook) noexcept
	{
		// la_symbind32 is a loader callback: diagnostics use only a lock-free
		// one-shot bit and write() to a descriptor opened by setup().  Never call
		// CLog, getenv, std::string formatting, or allocation here.
		if (!g_auditDiagnosticsEnabled.load(std::memory_order_acquire))
			return;
		const unsigned int bit = 1u << static_cast<unsigned int>(symbol);
		if ((g_auditDiagnosticMask.fetch_or(bit, std::memory_order_relaxed) & bit) != 0)
			return;
		writeRawLog("[Info] audit: ");
		writeRawLog(auditSymbolName(symbol));
		writeRawLog(" referenced by ");
		writeRawLog(auditObjectName(refcook));
		writeRawLog(" (definition ");
		writeRawLog(auditObjectName(defcook));
		writeRawLog(")\n");
	}

	void logCefRewrite(uint16_t port, bool published) noexcept
	{
		// This runs in a fork child immediately before exec.  The fd was opened
		// in the parent during setup, so this path performs no env lookup,
		// allocation, formatted I/O, or C++ logger locking after fork.
		writeRawLog("[Info] CEF: rewrote --remote-debugging-port to ");
		writeRawUnsigned(port);
		writeRawLog(published ? " (8080 freed, contract published)\n"
		                      : " (8080 freed, contract NOT published)\n");
	}

	// One rewritten argv at a time; a concurrent spawn from another thread
	// keeps its argv as it is.
	CefPort::ArgvArena g_cefArena;
	std::atomic_flag g_cefArenaBusy;  // clear (C++20)

	// The argv with the CEF debug port rewritten to the session port, or
	// nullptr to keep the original. This runs in the fork child right before
	// exec (or in the spawning thread for posix_spawn), where a lock another
	// thread held at fork time stays held forever: only static storage,
	// prepared data and plain system calls are used here, no allocation, no
	// environment lookup and no logger.
	char** cefRewriteArgv(char* const argv[]) noexcept
	{
		// Decky coexistence keeps Steam on its hard-coded 8080, and without a
		// port from this copy's setup() there is nothing to rewrite to.
		if (!argv || g_cefKeepDefaultPort || g_cefSessionPort == 0)
			return nullptr;
		if (g_cefArenaBusy.test_and_set(std::memory_order_acquire))
			return nullptr;
		char** out = CefPort::rewriteArgv(argv, g_cefSessionPort, g_cefArena);
		if (!out)
		{
			g_cefArenaBusy.clear(std::memory_order_release);
			return nullptr;
		}

		// Publish the contract file the Lumen sidecar reads — NOW, because this
		// client tree is actually launching the webhelper, so the session port
		// is the live CEF port. Writing here (not in setup()) is what makes the
		// contract robust against a concurrent second Steam instance at login:
		// that loser runs setup() but exits at the single-instance lock before
		// ever reaching this exec hook, so it never overwrites the contract.
		// Every webhelper launch publishes the same text again.
		//
		// The contract also carries this client's identity (pid + start time,
		// captured in setup()), so the sidecar can ignore a contract left behind
		// by a previous session instead of polling its dead port — and so a
		// vanilla Steam launch after an injected one falls back to 8080 instead
		// of chasing a stale ephemeral port forever.
		logCefRewrite(g_cefSessionPort, CefPort::publishPrepared(g_cefContract));
		return out;
	}

	// After the real call returned (a failed exec, or posix_spawn in the
	// parent), the arena may be used again.
	void cefRewriteDone(char** rewritten) noexcept
	{
		if (rewritten)
			g_cefArenaBusy.clear(std::memory_order_release);
	}

	int cefExecv(const char* path, char* const argv[])
	{
		char** rw = cefRewriteArgv(argv);
		const int result = g_realExecv(path, rw ? rw : argv);
		cefRewriteDone(rw);
		return result;
	}
	int cefExecvp(const char* file, char* const argv[])
	{
		char** rw = cefRewriteArgv(argv);
		const int result = g_realExecvp(file, rw ? rw : argv);
		cefRewriteDone(rw);
		return result;
	}
	int cefExecve(const char* path, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		const int result = g_realExecve(path, rw ? rw : argv, envp);
		cefRewriteDone(rw);
		return result;
	}
	int cefExecvpe(const char* file, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		const int result = g_realExecvpe(file, rw ? rw : argv, envp);
		cefRewriteDone(rw);
		return result;
	}
	int cefSpawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* fa,
	             const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		const int result = g_realSpawn(pid, path, fa, attr, rw ? rw : argv, envp);
		cefRewriteDone(rw);
		return result;
	}
	int cefSpawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* fa,
	              const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		const int result = g_realSpawnp(pid, file, fa, attr, rw ? rw : argv, envp);
		cefRewriteDone(rw);
		return result;
	}
}

extern "C" uintptr_t la_symbind32(Elf32_Sym* sym,
                                  __attribute__((unused)) unsigned int ndx,
                                  uintptr_t* refcook,
                                  uintptr_t* defcook,
                                  __attribute__((unused)) unsigned int* flags,
                                  const char* symname)
{
	if (sym == nullptr)
		return 0;

	const AuditBinding::Symbol symbol = AuditBinding::classify(symname);
	if (symbol == AuditBinding::Symbol::None)
		return sym->st_value;

	logAuditBinding(symbol, refcook, defcook);
	const auto orig = static_cast<uintptr_t>(sym->st_value);
	switch (symbol)
	{
		case AuditBinding::Symbol::LocalStatsEpoch:
			return reinterpret_cast<uintptr_t>(&slsteam_local_stats_epoch_v1);
		case AuditBinding::Symbol::Execv:
			if (!g_realExecv) g_realExecv = reinterpret_cast<execv_t>(orig);
			return reinterpret_cast<uintptr_t>(&cefExecv);
		case AuditBinding::Symbol::Execvp:
			if (!g_realExecvp) g_realExecvp = reinterpret_cast<execv_t>(orig);
			return reinterpret_cast<uintptr_t>(&cefExecvp);
		case AuditBinding::Symbol::Execve:
			if (!g_realExecve) g_realExecve = reinterpret_cast<execve_t>(orig);
			return reinterpret_cast<uintptr_t>(&cefExecve);
		case AuditBinding::Symbol::Execvpe:
			if (!g_realExecvpe) g_realExecvpe = reinterpret_cast<execve_t>(orig);
			return reinterpret_cast<uintptr_t>(&cefExecvpe);
		case AuditBinding::Symbol::PosixSpawn:
			if (!g_realSpawn) g_realSpawn = reinterpret_cast<spawn_t>(orig);
			return reinterpret_cast<uintptr_t>(&cefSpawn);
		case AuditBinding::Symbol::PosixSpawnp:
			if (!g_realSpawnp) g_realSpawnp = reinterpret_cast<spawn_t>(orig);
			return reinterpret_cast<uintptr_t>(&cefSpawnp);
		case AuditBinding::Symbol::None:
			break;
	}
	return sym->st_value;
}

// The decompiler reads link-time offsets out of .data.rel.ro, so each module is
// parsed from la_objopen, before the loader relocates it. Only the vtables of
// the module just opened are analysed.
static void decompileBeforeRelocation(const char* path)
{
	const bool client = AuditBinding::hasSuffix(path, "/steamclient.so");
	lm_module_t& mod = client ? g_modSteamClient : g_modSteamUI;
	if (mod.base || !LM_FindModule(client ? "steamclient.so" : "steamui.so", &mod))
	{
		return;
	}

	// This runs inside la_objopen, where an exception must not escape. A
	// module the parser cannot read leaves no vtables, so VFTIndexes::init()
	// fails and load() leaves Steam unmodified with a notice.
	try
	{
		BootProf::Span profile(g_pLog.get(), client ? "decompile_steamclient" : "decompile_steamui");
		Decompiler::parseModule(mod);
		for (auto& [name, vft] : Decompiler::vftables)
		{
			if (vft.moduleBase == mod.base)
			{
				vft.analyze();
			}
		}
	}
	catch (...)
	{
		Decompiler::cleanUp();
		try
		{
			if (g_pLog) g_pLog->warn("%s", "decompiler failed on a client module\n");
		}
		catch (...)
		{
		}
	}
}

// An exception must not cross an rtld-audit callback. A setup that threw is not
// retried, and load() then leaves Steam unmodified.
static bool setupAborted = false;
static void setupGuarded()
{
	if (setupAborted) return;
	try
	{
		setup();
		return;
	}
	catch (const std::exception& e)
	{
		// The handler often runs after a bad_alloc: logging may throw too.
		try
		{
			if (g_pLog) g_pLog->warn("setup() aborted by exception: %s\n", e.what());
		}
		catch (...)
		{
		}
	}
	catch (...)
	{
		try
		{
			if (g_pLog) g_pLog->warn("%s", "setup() aborted by a non-std exception\n");
		}
		catch (...)
		{
		}
	}
	setupAborted = true;
	setupSuccess = false;
	try
	{
		unload();
	}
	catch (...)
	{
	}
}

extern "C" unsigned int la_objopen(struct link_map* map,
                                   __attribute__((unused)) Lmid_t lmid,
                                   __attribute__((unused)) uintptr_t* cookie)
{
	if (map && map->l_name &&
	    (AuditBinding::hasSuffix(map->l_name, "/steamclient.so") ||
	     AuditBinding::hasSuffix(map->l_name, "/steamui.so")))
	{
		if (!setupSuccess)
		{
			setupGuarded();
		}
		if (setupSuccess)
		{
			decompileBeforeRelocation(map->l_name);
		}
		load();
	}

	// Bind-all is the default and the conservative behavior. Narrowing is
	// opt-in through SLSSTEAM_AUDIT_NARROW=1; SLSSTEAM_AUDIT_BINDALL=1
	// overrides it.  The policy-ready flag keeps getenv() out of this loader
	// callback; until setup publishes the policy, every object gets bind-all.
	const bool bindAll = !g_auditPolicyReady.load(std::memory_order_acquire)
		|| g_auditBindAll.load(std::memory_order_acquire);
	return AuditBinding::flagsForObject(map ? map->l_name : nullptr, bindAll);
}

extern "C" void la_preinit(__attribute__((unused)) uintptr_t* cookie)
{
	setupGuarded();
}
