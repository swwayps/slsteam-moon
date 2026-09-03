// SPDX-License-Identifier: AGPL-3.0-only
//
// See steamstub.hpp for the design notes.

#include "steamstub.hpp"

#include "../ascii.hpp"
#include "../config.hpp"
#include "../globals.hpp"
#include "../log.hpp"
#include "../thread_start.hpp"
#include "steamstub_warmup.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>


namespace
{
	// 4-byte signature at file offset 0x40 in PE files wrapped by the
	// SteamStub v2 packer.  The v3 packer (x86 + x64) drops this fixed
	// magic and instead puts its header inside a PE section named
	// ".bind" — both variants are recognised below.
	constexpr uint8_t kVlvSig[4] = { 'V', 'L', 'V', 0x00 };
	constexpr off_t   kVlvSigOff = 0x40;

	std::string g_helperScript;        // run-steamless.sh
	std::string g_steamlessHome;       // dir containing Steamless.CLI.exe
	std::atomic<bool> g_enabled{false};

	std::mutex g_processedMu;
	std::unordered_set<std::string> g_processedExes;

	// Warmup synchronisation. warmupAsync() spawns a detached thread for each
	// generation. Successful generations are one-shot; a failed generation
	// wakes launchers and leaves the start gate retryable until the next
	// generation claims it. onLaunchApp() waits for the active generation and
	// retries once when a failure completes while the victim is waiting.
	std::atomic<bool> g_warmupStarted{false};
	std::atomic<bool> g_warmupDone{false};
	std::atomic<bool> g_warmupFailed{false};
	std::uint64_t g_warmupGeneration = 0;
	std::mutex g_warmupMu;
	std::condition_variable g_warmupCV;

	// Returns true when the file at `path` carries the VLV magic at
	// offset 0x40 (SteamStub v2) OR has a ".bind" PE section
	// (SteamStub v3 x86/x64).  Best-effort: any I/O error or a
	// malformed header returns false.  A missed positive only costs
	// the user the wine round-trip with an early exit; a false
	// positive falls through to Steamless which itself rejects
	// non-stub'd inputs.
	bool fileHasStubMarker(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		if (!f.is_open()) return false;

		// v2 magic at 0x40.
		f.seekg(kVlvSigOff, std::ios::beg);
		uint8_t buf[4] = {};
		f.read(reinterpret_cast<char*>(buf), sizeof(buf));
		if (f.gcount() == static_cast<std::streamsize>(sizeof(buf))
		    && std::memcmp(buf, kVlvSig, sizeof(buf)) == 0)
		{
			return true;
		}

		// v3 detection — walk the PE section table looking for ".bind".
		f.clear();
		f.seekg(0, std::ios::beg);

		uint8_t mz[2] = {};
		f.read(reinterpret_cast<char*>(mz), sizeof(mz));
		if (f.gcount() != 2 || mz[0] != 'M' || mz[1] != 'Z') return false;

		uint32_t e_lfanew = 0;
		f.seekg(0x3c, std::ios::beg);
		f.read(reinterpret_cast<char*>(&e_lfanew), sizeof(e_lfanew));
		if (f.gcount() != sizeof(e_lfanew)) return false;
		if (e_lfanew == 0 || e_lfanew > 0x10000) return false; // sanity

		// PE\0\0 magic.
		uint8_t peSig[4] = {};
		f.seekg(e_lfanew, std::ios::beg);
		f.read(reinterpret_cast<char*>(peSig), sizeof(peSig));
		if (f.gcount() != 4
		    || peSig[0] != 'P' || peSig[1] != 'E'
		    || peSig[2] != 0   || peSig[3] != 0)
		{
			return false;
		}

		// COFF header: NumberOfSections @ +6, SizeOfOptionalHeader @ +0x14.
		uint16_t nsec = 0, optsize = 0;
		f.seekg(e_lfanew + 6, std::ios::beg);
		f.read(reinterpret_cast<char*>(&nsec), sizeof(nsec));
		f.seekg(e_lfanew + 0x14, std::ios::beg);
		f.read(reinterpret_cast<char*>(&optsize), sizeof(optsize));
		if (!f.good() || nsec == 0 || nsec > 96) return false;

		const std::streamoff secTable = static_cast<std::streamoff>(e_lfanew)
		                              + 0x18 + optsize;
		for (uint16_t i = 0; i < nsec; ++i)
		{
			char name[8] = {};
			f.seekg(secTable + i * 40, std::ios::beg);
			f.read(name, sizeof(name));
			if (!f.good()) return false;
			// Section names are zero-padded ASCII; ".bind" is the
			// container the Steamless v3 unpacker looks for.
			if (std::strncmp(name, ".bind", 5) == 0 && (name[5] == 0 || name[5] == ' '))
			{
				return true;
			}
		}
		return false;
	}

	// Resolve the install dir for `appId` from the Steam appmanifest.
	// We go through the appmanifest rather than IClientAppManager
	// because the manifest is the canonical source — and we don't
	// want to introduce another vfunc dependency that could break on
	// Steam updates.  Walks every Steam library:
	//   1. Default install:  ~/.steam/<distro>/steamapps/
	//   2. libraryfolders.vdf-listed external library paths.
	std::string findInstallDir(uint32_t appId)
	{
		const char* home = std::getenv("HOME");
		if (!home) return {};

		// Probe a handful of well-known Steam roots.  This list
		// matches what feats/depotkey.cpp uses so the behaviour stays
		// consistent across the codebase.
		static const char* steamRoots[] = {
			"/.steam/steam",
			"/.steam/debian-installation",
			"/.local/share/Steam",
		};

		std::vector<std::filesystem::path> libraryRoots;
		for (const char* suffix : steamRoots)
		{
			auto root = std::filesystem::path(home) / (std::string(suffix).substr(1));
			if (std::filesystem::exists(root))
			{
				libraryRoots.push_back(root / "steamapps");
				// External libraries from libraryfolders.vdf
				const auto libVdf = root / "steamapps" / "libraryfolders.vdf";
				if (std::filesystem::exists(libVdf))
				{
					std::ifstream f(libVdf);
					std::string line;
					while (std::getline(f, line))
					{
						// Crude but effective: any "path" key value.
						const auto pos = line.find("\"path\"");
						if (pos == std::string::npos) continue;
						const auto qOpen = line.find('"', pos + 6);
						if (qOpen == std::string::npos) continue;
						const auto qClose = line.find('"', qOpen + 1);
						if (qClose == std::string::npos) continue;
						auto extra = line.substr(qOpen + 1, qClose - qOpen - 1);
						libraryRoots.push_back(std::filesystem::path(extra) / "steamapps");
					}
				}
			}
		}

		for (const auto& libroot : libraryRoots)
		{
			const auto manifest = libroot / ("appmanifest_" + std::to_string(appId) + ".acf");
			if (!std::filesystem::exists(manifest)) continue;

			std::ifstream f(manifest);
			std::string line;
			std::string installdir;
			while (std::getline(f, line))
			{
				const auto pos = line.find("\"installdir\"");
				if (pos == std::string::npos) continue;
				const auto qOpen = line.find('"', pos + 12);
				if (qOpen == std::string::npos) continue;
				const auto qClose = line.find('"', qOpen + 1);
				if (qClose == std::string::npos) continue;
				installdir = line.substr(qOpen + 1, qClose - qOpen - 1);
				break;
			}
			if (!installdir.empty())
			{
				return (libroot / "common" / installdir).string();
			}
		}
		return {};
	}

	constexpr auto kHelperTimeout = std::chrono::seconds(180);

	bool waitForHelper(pid_t pid, int& status, const char* label)
	{
		const auto deadline = std::chrono::steady_clock::now() + kHelperTimeout;
		for (;;)
		{
			const pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited == pid) return true;
			if (waited < 0 && errno != EINTR)
			{
				g_pLog->warn("SteamStub: %s waitpid failed (errno=%d)\n",
				             label, errno);
				break;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				g_pLog->warn("SteamStub: %s timed out after %llds; terminating process group\n",
				             label,
				             static_cast<long long>(kHelperTimeout.count()));
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}

		if (kill(-pid, SIGKILL) != 0) (void)kill(pid, SIGKILL);
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		return false;
	}

	// Run `<script> <exePath>` synchronously with the STEAMLESS_HOME
	// env var pointing at our bundled binaries.  Returns the helper's
	// exit code, or -1 on spawn failure.
	int runHelper(const std::string& exePath)
	{
		const pid_t pid = fork();
		if (pid < 0)
		{
			g_pLog->warn("SteamStub: fork() failed (errno=%d)\n", errno);
			return -1;
		}
		if (pid == 0)
		{
			(void)setpgid(0, 0);
			// Child.  Set STEAMLESS_HOME, exec the helper.
			setenv("STEAMLESS_HOME", g_steamlessHome.c_str(), 1);
			// QUIET=1 keeps the SLSsteam log clean of Steamless's
			// banner; the helper still logs its own one-liner per
			// step plus errors via stderr.
			setenv("QUIET", "1", 1);
			execlp("/bin/bash", "bash", g_helperScript.c_str(),
			       exePath.c_str(), nullptr);
			// If exec returns we are in a wedged state; bail loudly.
			_exit(127);
		}

		(void)setpgid(pid, pid);
		// Parent.  Wait synchronously — we want the unpacked exe in
		// place before LaunchApp returns and Proton starts.
		int status = 0;
		if (!waitForHelper(pid, status, "Steamless helper"))
			return -1;
		if (WIFEXITED(status))
		{
			return WEXITSTATUS(status);
		}
		g_pLog->warn("SteamStub: helper terminated abnormally (status=%d)\n", status);
		return -1;
	}

	// Run `<script> --prewarm` synchronously (in a worker thread,
	// not the calling thread).  Same env, same waitpid, just no exe.
	int runPrewarm()
	{
		const pid_t pid = fork();
		if (pid < 0)
		{
			g_pLog->warn("SteamStub: prewarm fork() failed (errno=%d)\n", errno);
			return -1;
		}
		if (pid == 0)
		{
			(void)setpgid(0, 0);
			setenv("STEAMLESS_HOME", g_steamlessHome.c_str(), 1);
			setenv("QUIET", "1", 1);
			execlp("/bin/bash", "bash", g_helperScript.c_str(),
			       "--prewarm", nullptr);
			_exit(127);
		}
		(void)setpgid(pid, pid);
		int status = 0;
		if (!waitForHelper(pid, status, "Steamless prewarm"))
			return -1;
		if (WIFEXITED(status))
		{
			return WEXITSTATUS(status);
		}
		return -1;
	}
}

namespace SteamStub
{

void setup(const char* installRoot)
{
	if (!installRoot || !installRoot[0])
	{
		g_pLog->debug("SteamStub: install root missing; feature disabled\n");
		return;
	}

	std::filesystem::path root(installRoot);

	// Helper script and binary kit live under tools/. Probe two
	// locations for both:
	//   1. Next to SLSsteam.so (dev tree, bundled releases).
	//   2. User-local mirror under ~/.local/share/SLSsteam/ for
	//      system-packaged installs where the .so is read-only.
	const std::filesystem::path scriptCandidates[] = {
		root / "tools" / "steamstub-bypass" / "run-steamless.sh",
		std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/")
			/ ".local" / "share" / "SLSsteam" / "steamstub-bypass"
			/ "run-steamless.sh",
	};
	const std::filesystem::path binCandidates[] = {
		root / "tools" / "steamless-bin",
		std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "/")
			/ ".local" / "share" / "SLSsteam" / "steamless-bin",
	};

	bool haveScript = false;
	for (const auto& cand : scriptCandidates)
	{
		if (std::filesystem::exists(cand))
		{
			g_helperScript = cand.string();
			haveScript = true;
			break;
		}
	}
	bool haveBinary = false;
	for (const auto& cand : binCandidates)
	{
		if (std::filesystem::exists(cand / "Steamless.CLI.exe"))
		{
			g_steamlessHome = cand.string();
			haveBinary = true;
			break;
		}
	}

	if (!haveScript || !haveBinary)
	{
		g_pLog->warn
		(
			"SteamStub: helper missing (script=%d binary=%d, root=%s); "
			"DRM removal DISABLED — games with Steam DRM will fail with "
			"\"Application load error 6\". Reinstall, or run "
			"tools/steamstub-bypass/install-steamless.sh to enable.\n",
			haveScript, haveBinary, root.c_str()
		);
		return;
	}

	g_enabled.store(true, std::memory_order_release);
	g_pLog->debug
	(
		"SteamStub: enabled (script=%s, steamless=%s)\n",
		g_helperScript.c_str(), g_steamlessHome.c_str()
	);
}

void warmupAsync()
{
	if (!g_enabled.load(std::memory_order_acquire)) return;

	// First call wins; the generation claim and its flag reset must be
	// serialised with the worker's exit callback.  Otherwise an old exit
	// can publish done=true into a newly-started generation.
	if (!SteamStub::tryBeginWarmup(g_warmupMu,
	                               g_warmupStarted,
	                               g_warmupDone,
	                               g_warmupFailed,
	                               g_warmupGeneration))
	{
		return;
	}
	const bool started = ThreadStart::startDetached(
		[]
		{
			ThreadStart::runGuarded(
				[]
				{
					g_pLog->debug("SteamStub: prewarming Wine prefix in background\n");
					const int rc = runPrewarm();
					if (rc == 0)
					{
						g_pLog->debug("SteamStub: prewarm complete\n");
					}
					else
					{
						SteamStub::recordWarmupResult(g_warmupFailed, rc);
						g_pLog->debug
						(
							"SteamStub: prewarm exited with rc=%d "
							"(launch-time unpack will pay the cost)\n",
							rc
						);
					}
				},
				[]
				{
					g_warmupFailed.store(true, std::memory_order_release);
				},
				[]
				{
					SteamStub::finishWarmup(g_warmupMu,
					                        g_warmupStarted,
					                        g_warmupDone,
					                        g_warmupFailed);
					g_warmupCV.notify_all();
				});
		},
		[]
		{
			std::lock_guard<std::mutex> lk(g_warmupMu);
			g_warmupFailed.store(false, std::memory_order_release);
			g_warmupStarted.store(false, std::memory_order_release);
			g_warmupDone.store(true, std::memory_order_release);
			g_warmupCV.notify_all();
		},
		[]
		{
			g_pLog->warn("SteamStub: prewarm detach failed; joining worker\n");
		});
	if (!started)
	{
		g_pLog->warn("SteamStub: unable to start prewarm worker; launch will warm inline\n");
	}

}

void onLaunchApp(uint32_t appId)
{
	if (!g_enabled.load(std::memory_order_acquire)) return;
	if (!appId) return;
	if (!g_config.isAddedAppId(appId)) return;

	const auto installDir = findInstallDir(appId);
	if (installDir.empty() || !std::filesystem::exists(installDir))
	{
		g_pLog->debug("SteamStub: no install dir resolved for %u; skip\n", appId);
		return;
	}

	g_pLog->debug("SteamStub: scanning %s for %u\n", installDir.c_str(), appId);

	// Walk a small recursion (most stub-wrapped exes live at the
	// install dir root, but a few games tuck them in subdirs like
	// Bin/ or Binaries/).  Cap depth to avoid runaway.
	for (auto it = std::filesystem::recursive_directory_iterator(installDir);
	     it != std::filesystem::recursive_directory_iterator();
	     ++it)
	{
		if (it.depth() > 3)
		{
			it.disable_recursion_pending();
			continue;
		}
		if (!it->is_regular_file()) continue;

		const auto& p = it->path();
		// Cheap extension filter before opening the file.
		auto ext = p.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](unsigned char c){ return Ascii::toLower(c); });
		if (ext != ".exe") continue;

		const auto pathStr = p.string();

		// Skip the helper's own artefacts: it leaves `<exe>.original.exe`
		// as a backup and may transiently produce `<exe>.unpacked.exe`.
		// Re-running steamless on those would corrupt the backup chain
		// (verified once in the wild: produced .original.exe.original.exe
		// and a swapped-in unpacked stand-in).
		const auto fname = p.filename().string();
		auto endsWith = [](const std::string& s, const char* suffix) {
			const size_t n = std::strlen(suffix);
			return s.size() >= n
			    && std::strncmp(s.data() + s.size() - n, suffix, n) == 0;
		};
		if (endsWith(fname, ".original.exe") || endsWith(fname, ".unpacked.exe"))
		{
			continue;
		}

		// Marker file lets us skip already-processed exes without
		// re-reading them on every launch.  Validate the marker
		// against the exe's mtime so a Steam update that rewrites
		// the binary (and therefore re-applies the stub) forces a
		// reprocess instead of being silently skipped.
		const auto markerPath = pathStr + ".steamless_done";
		if (std::filesystem::exists(markerPath))
		{
			std::error_code ec1, ec2;
			const auto exeMtime    = std::filesystem::last_write_time(p, ec1);
			const auto markerMtime = std::filesystem::last_write_time(markerPath, ec2);
			if (!ec1 && !ec2 && markerMtime >= exeMtime)
			{
				continue;
			}
			// Marker stale (exe newer than marker, or stat failed).
			// Drop it; the helper will rewrite it after a successful
			// run.  If stat failed, fall through and let the sig
			// check decide.
			std::error_code rmEc;
			std::filesystem::remove(markerPath, rmEc);
		}

		// Avoid redoing the same exe twice in one Steam session
		// (e.g. user clicks Stop then Play again).
		{
			std::lock_guard<std::mutex> lk(g_processedMu);
			if (g_processedExes.count(pathStr))
			{
				continue;
			}
		}

		if (!fileHasStubMarker(pathStr))
		{
			continue;
		}

		// A real victim may be the first launch after setup(), or may arrive
		// after a failed prewarm generation released the start gate. In either
		// case, start the next generation here before taking the wait lock. The
		// gate inside warmupAsync() serialises concurrent launch retries.
		if (!g_warmupStarted.load(std::memory_order_acquire))
		{
			warmupAsync();
		}

		// We have a real victim — wait for the background prewarm
		// to finish before invoking the helper for real. Each waiter follows a
		// generation identity: if another victim claims the replacement first,
		// join that generation and wait for its terminal state rather than
		// proceeding on a snapshot of shared booleans.
		std::uint64_t observedGeneration = 0;
		bool retriedWarmup = false;
		for (;;)
		{
			bool retryWarmup = false;
			{
				std::unique_lock<std::mutex> lk(g_warmupMu);
				if (observedGeneration == 0)
					observedGeneration = g_warmupGeneration;

				if (g_warmupGeneration != observedGeneration)
				{
					observedGeneration = g_warmupGeneration;
					retriedWarmup = true;
				}

				const auto waitingGeneration = observedGeneration;
				if (g_warmupStarted.load(std::memory_order_acquire)
				    && !g_warmupDone.load(std::memory_order_acquire))
				{
					g_pLog->debug("SteamStub: waiting for prewarm generation %llu\n",
					             static_cast<unsigned long long>(waitingGeneration));
					g_warmupCV.wait(lk, [waitingGeneration]
					{
						return g_warmupGeneration != waitingGeneration ||
						       g_warmupDone.load(std::memory_order_acquire);
					});
					if (g_warmupGeneration != waitingGeneration)
					{
						observedGeneration = g_warmupGeneration;
						retriedWarmup = true;
						continue;
					}
				}

				if (SteamStub::shouldRetryWarmup(
						g_warmupStarted.load(std::memory_order_acquire),
						g_warmupDone.load(std::memory_order_acquire),
						g_warmupFailed.load(std::memory_order_acquire))
				    && !retriedWarmup)
				{
					retriedWarmup = true;
					retryWarmup = true;
				}
				else
				{
					break;
				}
			}

			if (retryWarmup)
			{
				g_pLog->debug("SteamStub: retrying failed prewarm before launch\n");
				warmupAsync();
			}
		}

		g_pLog->info("SteamStub: processing %s\n", pathStr.c_str());
		const int rc = runHelper(pathStr);
		switch (rc)
		{
			case 0:
				g_pLog->info("SteamStub: processed (%s)\n", pathStr.c_str());
				break;
			case 2:
				// Already in target shape — race between sig check
				// and helper invocation.  Harmless.
				g_pLog->debug("SteamStub: helper reported nothing to do (%s)\n", pathStr.c_str());
				break;
			default:
				g_pLog->warn
				(
					"SteamStub: helper failed for %s (rc=%d)\n",
					pathStr.c_str(), rc
				);
				g_pLog->notifyUser(UserMsg::DrmRemovalFailed);
				break;
		}

		std::lock_guard<std::mutex> lk(g_processedMu);
		g_processedExes.insert(pathStr);
	}
}

}  // namespace SteamStub
