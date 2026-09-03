#include <dlfcn.h>
#include "afftrace.hpp"
#include "ascii.hpp"
#include "sdk/steam.hpp"
#include "api.hpp"
#include "runtimedir.hpp"
#include "audit_log.hpp"
#include "audit_policy.hpp"
#include "audit_symbols.hpp"
#include "config.hpp"
#include "globals.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "bootprof.hpp"
#include "ownerwork.hpp"
#include "patterns.hpp"
#include "runtime_attestation.hpp"
#include "runtime_dependencies.hpp"
#include "update.hpp"
#include "utils.hpp"
#include "utils/process_lock.hpp"

#include "feats/appinfo_provision.hpp"
#include "feats/provision_schedule.hpp"
#include "feats/appinfo_vdf.hpp"
#include "feats/cmclient.hpp"
#include "feats/apps.hpp"
#include "feats/cefport.hpp"
#include "feats/depotkey.hpp"
#include "feats/hotreload.hpp"
#include "feats/manifestid.hpp"
#include "feats/steamstub.hpp"
#include "feats/stats_policy.hpp"

#include "libmem/libmem.h"

#include <cerrno>
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
	// An audited non-Steam process returns from setup before logging/hooks
	// exist. Hooks::remove logs, so this early no-op must precede teardown.
	if (!g_pLog) return;
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

// CEF debug port chosen ONCE for this Steam-client session (see la_symbind32
// block below). Picked in load() so it's decided in the long-lived client and
// inherited by every webhelper-launching fork-child via fork() — the port then
// stays stable across webhelper restarts (no re-pick, no bindability re-check),
// which is what keeps the Lumen sidecar reconnecting to the same endpoint.
static uint16_t g_cefSessionPort = 0;

// When true, we deliberately leave Steam's CEF endpoint on its hard-coded 8080
// instead of rewriting it to an ephemeral port. Set in setup() when Decky
// Loader is present: Decky's injector is hard-coded to 8080 and can't follow
// our port, so we share 8080 (Lumen falls back to it). This gates BOTH the
// setup() port pick and the exec-time rewrite (cefRewriteArgv), so the lazy
// fallback resolve in the exec hook can't pick a port behind our back.
static bool g_cefKeepDefaultPort = false;

// Identity of THIS Steam client, captured in setup() (i.e. inside the client
// itself, before it forks anything) and stamped into the CEF port contract when
// the exec hook publishes it. It lets the Lumen sidecar tell a live contract
// from a leftover one written by a previous session: the pid must still exist
// and must still have this start time. Captured here — not at publish time —
// because the exec hook runs in a fork child, whose pid is not the client's.
static long g_clientPid = 0;
static unsigned long long g_clientStartTicks = 0;

// Audit callbacks and post-fork exec wrappers must not consult C++ logging
// state.  setup() opens this descriptor before any webhelper fork; callbacks
// only use write(2)-style operations against the inherited fd. Steam may close
// inherited descriptors in the fork child, so keep a fixed path for an
// async-signal-safe O_APPEND reopen at the last possible moment.
static int g_rawLogFd = -1;
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
	if (g_rawLogFd < 0 && g_rawLogPath[0] != '\0')
	{
		g_rawLogFd = open(g_rawLogPath, O_WRONLY | O_APPEND | O_CLOEXEC);
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
		// Only skip when the lock is genuinely ours and held by another auditor
		// namespace. An unusable lock (no private runtime directory, or a file at
		// that path we cannot vouch for) must NOT stop setup: this lock used to
		// live in /tmp, where pre-creating it and holding an flock was enough for
		// any local process to switch injection off entirely.
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
	// Thread-affinity diagnostics: opt-in only (SLSSTEAM_AFFTRACE=1), a no-op
	// otherwise. Must come before the watchers and hooks so the very first
	// callback and IPC frame are covered when it IS enabled.
	if (AffTrace::init())
	{
		g_pLog->info("Affinity trace enabled -> %s (bounded, 0600, no identifiers)\n",
		             AffTrace::defaultPath().c_str());
	}
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


	if(!g_config.init())
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

	RuntimeDependencies::check();

	// Decide this session's CEF debug port ONCE, as EARLY as possible (this is
	// la_preinit, before the client can spawn the webhelper) so g_cefSessionPort
	// is already set when the exec hook fires and stays fixed across webhelper
	// restarts. We deliberately do NOT write the contract file here: at login
	// autostart two Steam instances can run setup() concurrently, and only the
	// one that wins Steam's single-instance race goes on to spawn a webhelper.
	// Writing the contract here would let the LOSING instance (which picks a
	// different free port, then exits before launching anything) overwrite it
	// with a port nothing ends up listening on — the Lumen sidecar would then
	// connect to a dead port and never inject. So we only resolve the port now
	// and publish it later, from the exec hook, when THIS tree actually launches
	// the webhelper (see cefRewriteArgv). resolveSessionPortNoPersist reuses a
	// still-bindable port from a previous session, else picks a fresh one.
	//
	// EXCEPTION — Decky coexistence: Decky Loader's injector is hard-coded to
	// localhost:8080 and runs as a persistent daemon, so it can't follow our
	// ephemeral port. When Decky is installed we leave CEF on 8080 (no rewrite,
	// no contract) and let Lumen fall back to 8080; both share the endpoint
	// (multiple CDP clients coexist on one CEF target — verified on Bazzite). We
	// also drop any STALE contract from a previous ephemeral session so Lumen
	// doesn't connect to a dead port instead of falling back to 8080.
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
		if (g_cefSessionPort != 0)
		{
			g_pLog->info("CEF: remote-debugging port -> %u (frees 8080); published when this client launches the webhelper\n",
			             g_cefSessionPort);
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
		const bool warmCurl = AppInfoProvision::shouldWarmCurlBeforePics(
		    !g_config.managedAppIds.get().empty());

		// Loading libcurl's TLS dependency tree from the first PICS worker
		// callback can crash the 32-bit Steam client. Warm it up here while
		// setup() still owns the safe preinit call stack; the callback then
		// only reuses already-resolved function pointers. This intentionally
		// does not depend on appinfo.vdf being discoverable: the PICS callback
		// can still run and must never be the first libcurl loader.
		if (warmCurl && !CmClient::prepareForThreadedFetch())
		{
			g_pLog->warn("CmClient: unable to preload libcurl before PICS worker\n");
		}

		// Runtime provisioning defers Proton mappings because Steam's live
		// ConfigStore writers do not participate in our advisory lock. Apply
		// the pending set now, while setup() still owns the preinit window.
		AppInfoProvision::flushPendingProtonMappings();

		const auto candidate = AppInfoVdf::findExistingPath();
		if (!candidate.empty())
		{
			const auto preinitAction =
			    AppInfoProvision::preinitProvisionAction(asyncProvision);
			if (preinitAction ==
			    AppInfoProvision::PreinitProvisionAction::SynchronousProvision)
			{
				// The kill-switch restores the former synchronous behavior for
				// field rollback: refresh every managed app before the splice.
				DepotKey::importLuaScripts();
				ManifestId::importLuaScripts();
				BootProf::Span provisionProfile(g_pLog.get(), "provision.sync_preinit");
				AppInfoProvision::provisionAllAddedApps(candidate);
			}
			else
			{
				// Async mode still keeps the genuine first-run path synchronous,
				// but only for apps without a complete validated cache pair. This
				// is deliberately before the appinfo splice: writing the pair from
				// PICS after Steam has loaded appinfo would require a second restart.
				DepotKey::importLuaScripts();
				ManifestId::importLuaScripts();
				BootProf::Span provisionProfile(g_pLog.get(), "provision.cold_preinit");
				const auto startupCandidates = g_config.managedAppIds.get();
				AppInfoProvision::provisionColdStartApps(
				    candidate,
				    startupCandidates,
				    AppInfoProvision::ColdStartMode::StartupRequireUsablePair,
				    nullptr,
				    true);
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

	try
	{
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
		// The lock used to be /tmp/.slssteam.load.<pid>, opened without O_EXCL or
		// O_NOFOLLOW. Any local process could pre-create that path and hold an
		// exclusive flock on it; our flock(LOCK_NB) then failed and we SKIPPED
		// the hooking pass — a lock file that doubled as an injection kill
		// switch. It now lives in the user's private 0700 runtime directory, is
		// opened with O_NOFOLLOW, and is only honoured when the descriptor really
		// is a private regular file we own. Anything else is treated as "no
		// usable lock" and hooking proceeds: failing OPEN here is the safe
		// direction, because the only cost is a redundant pass.
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
		logBuildId("library-inject.so", "audit-loader");
		logBuildId("steamclient.so", "steamclient");
		logBuildId("steamui.so", "steamui");
		logBuildId("cloud_redirect.so", "cloud-redirect");
	}

	if (!Updater::verifySafeModeHash())
	{
		if (g_config.safeMode.get())
		{
			g_pLog->warn("Unknown steamclient.so hash! Aborting...");
			g_pLog->notifyUser(UserMsg::SteamVersionUnsupported);
			unload();
			return;
		}
		else if (g_config.warnHashMissmatch.get())
		{
			g_pLog->warn("steamclient.so hash missmatch! Please update :)");
			g_pLog->notifyUser(UserMsg::SteamVersionMismatch);
		}
	}

	if (!Steam::init())
	{
		g_pLog->warn("Failed to find steam exports!\n");
		return;
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
	{
		BootProf::Span importProfile(g_pLog.get(), "lua_imports");
		DepotKey::onStartup();
		ManifestId::importLuaScripts();
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

	if (g_config.notifyInit.get())
	{
		const auto now = std::chrono::time_point{std::chrono::system_clock::now()};
		const auto ymd = std::chrono::year_month_day{std::chrono::floor<std::chrono::days>(now)};

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
	catch (...)
	{
		// Never let an exception cross the rtld-audit callback boundary: the
		// dynamic linker cannot unwind through la_objopen safely.
		try
		{
			unload();
		}
		catch (...)
		{
			// Cleanup is best effort after a failed load.
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
// wrapper string, so a substring rewrite is used (CefPort::rewritePortArg).
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
		AuditLog::write(g_rawLogFd, g_rawLogPath, text);
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

	void logCefRewrite(uint16_t port) noexcept
	{
		// This runs in a fork child immediately before exec.  The fd was opened
		// in the parent during setup, so this path performs no env lookup,
		// allocation, formatted I/O, or C++ logger locking after fork.
		writeRawLog("[Info] CEF: rewrote --remote-debugging-port to ");
		writeRawUnsigned(port);
		writeRawLog(" (8080 freed)\n");
	}

	// Returns a heap argv copy with the CEF debug port rewritten to a free
	// loopback port, or nullptr if argv carries no such switch (caller then
	// uses the original). The copy is intentionally leaked: the caller is
	// about to exec.
	char** cefRewriteArgv(char* const argv[])
	{
		if (!argv)
		{
			return nullptr;
		}

		// Decky coexistence: leave Steam on its hard-coded 8080 — no rewrite, no
		// contract. Gating here (not just via g_cefSessionPort) is essential: the
		// lazy fallback below would otherwise pick a fresh ephemeral port and
		// rewrite anyway.
		if (g_cefKeepDefaultPort)
		{
			return nullptr;
		}

		int n = 0;
		bool any = false;
		for (; argv[n]; ++n)
		{
			const char* p = std::strstr(argv[n], CefPort::kSwitchPrefix);
			if (p && Ascii::isDigit(static_cast<unsigned char>(
				p[std::strlen(CefPort::kSwitchPrefix)])))
			{
				any = true;
			}
		}
		if (!any)
		{
			return nullptr;
		}

		static uint16_t port = 0;
		if (port == 0)
		{
			// Prefer the session port decided in setup() (stable across webhelper
			// restarts). Fall back to a lazy resolve only if a webhelper launch
			// somehow beats setup() (it shouldn't: the UI/webhelper come up well
			// after steamclient.so maps). Neither path writes the contract here;
			// publishing happens below, once, when we actually rewrite the arg.
			port = g_cefSessionPort != 0
			           ? g_cefSessionPort
			           : CefPort::resolveSessionPortNoPersist(CefPort::contractPath());
		}
		if (port == 0)
		{
			return nullptr; // no port available: leave Steam on 8080
		}

		// Publish the contract file the Lumen sidecar reads — NOW, because this
		// client tree is actually launching the webhelper, so `port` is the live
		// CEF port. Writing here (not in setup()) is what makes the contract
		// robust against a concurrent second Steam instance at login: that loser
		// runs setup() but exits at the single-instance lock before ever reaching
		// this exec hook, so it never overwrites the contract. Idempotent within
		// the tree (guarded); contract == live by construction.
		//
		// The contract also carries this client's identity (pid + start time,
		// captured in setup()), so the sidecar can ignore a contract left behind
		// by a previous session instead of polling its dead port — and so a
		// vanilla Steam launch after an injected one falls back to 8080 instead
		// of chasing a stale ephemeral port forever.
		static bool published = false;
		if (!published && CefPort::writePortFile(CefPort::contractPath(), port,
		                                        g_clientPid, g_clientStartTicks))
		{
			published = true;
			if (g_pLog)
			{
				g_pLog->info("CEF: published debug port %u to %s (owner pid %ld, start %llu)\n",
				             port, CefPort::contractPath().c_str(), g_clientPid,
				             g_clientStartTicks);
			}
		}

		char** out = static_cast<char**>(std::malloc(sizeof(char*) * (n + 1)));
		if (!out)
		{
			return nullptr;
		}
		for (int i = 0; i < n; ++i)
		{
			auto [s, changed] = CefPort::rewritePortArg(argv[i], port);
			out[i] = changed ? strdup(s.c_str()) : argv[i];
		}
		out[n] = nullptr;

		logCefRewrite(port);
		return out;
	}

	int cefExecv(const char* path, char* const argv[])
	{
		char** rw = cefRewriteArgv(argv);
		return g_realExecv(path, rw ? rw : argv);
	}
	int cefExecvp(const char* file, char* const argv[])
	{
		char** rw = cefRewriteArgv(argv);
		return g_realExecvp(file, rw ? rw : argv);
	}
	int cefExecve(const char* path, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		return g_realExecve(path, rw ? rw : argv, envp);
	}
	int cefExecvpe(const char* file, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		return g_realExecvpe(file, rw ? rw : argv, envp);
	}
	int cefSpawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* fa,
	             const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		return g_realSpawn(pid, path, fa, attr, rw ? rw : argv, envp);
	}
	int cefSpawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* fa,
	              const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
	{
		char** rw = cefRewriteArgv(argv);
		return g_realSpawnp(pid, file, fa, attr, rw ? rw : argv, envp);
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
			setup();
		}
		load();
	}

	// Keep the historical bind-all policy unless narrowing is explicitly
	// enabled after its live importer/rewrite gates have been measured.
	// SLSSTEAM_AUDIT_BINDALL=1 remains a visible rollback switch for a
	// narrowed session, while the default is already the safe rollback.  The
	// preinit-ready guard avoids getenv() in this loader callback; before setup
	// publishes policy, bind-all is the conservative behavior.
	const bool bindAll = !g_auditPolicyReady.load(std::memory_order_acquire)
		|| g_auditBindAll.load(std::memory_order_acquire);
	return AuditBinding::flagsForObject(map ? map->l_name : nullptr, bindAll);
}

extern "C" void la_preinit(__attribute__((unused)) uintptr_t* cookie)
{
	setup();
}
