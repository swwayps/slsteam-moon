#include "api.hpp"

#include "sdk/IClientAppManager.hpp"
#include "sdk/IClientCompat.hpp"

#include "afftrace.hpp"
#include "config.hpp"
#include "filewatcher.hpp"
#include "runtimedir.hpp"
#include "utils.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <ios>
#include <unistd.h>
#include <cerrno>
#include <mutex>


namespace SLSAPI
{
	// Resolved at init() from $XDG_RUNTIME_DIR (or $HOME as a fallback). This
	// used to be the fixed "/tmp/SLSsteam.API": a control channel that installs
	// apps, living in a directory every local user can write to, with no access
	// control of its own. It is now a 0600 file inside a 0700 per-user directory.
	std::string contractPath;
	const char* path = "";
	std::fstream fstream;
	CFileWatcher* watcher;
	std::atomic<void(*)()> reloadPlugins {nullptr};

	std::mutex cmdMutex;
	std::vector<CompatOp_t> compatOps;
	std::vector<LibraryOp_t> libraryOps;
}

bool SLSAPI::isEnabled()
{
	return g_config.api.get() && fstream.is_open();
}

void SLSAPI::onFileChange()
{
	// Runs on the API watcher pthread. Every command enters Steam-owned code,
	// so it is queued for runIPCFrame() on Steam's IPC thread; parsing stays
	// here.
	auto watchSpan = AffTrace::watchSpan(AffTrace::Src::Api);

	//Hot reload support :)
	if (!isEnabled())
	{
		return;
	}

	//Shitty way to reopen the stream. We have to do this, otherwise the fstream gets invalidated when running echo >
	//
	// Revalidate on EVERY reopen, not just at init(): the check init() performs is
	// a snapshot, and this is the point where the file is re-derived from a path
	// rather than from a descriptor anyone ever vouched for. A symlink or a
	// replaced file at the contract path abandons the channel instead of being
	// read as a command.
	fstream.close();
	{
		const int probe = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (probe < 0)
		{
			g_pLog->info("API contract file could not be reopened -> channel closed\n");
			return;
		}
		const bool ok = RuntimeDir::descriptorIsPrivate(probe);
		close(probe);
		if (!ok)
		{
			g_pLog->info("API contract file is no longer private -> channel closed\n");
			return;
		}
	}
	fstream.open(path);
	if (!fstream.is_open())
	{
		g_pLog->info("API contract file could not be reopened for IO -> channel closed\n");
		return;
	}

	// One command per line (upstream 7186151). Bounded: the file is private,
	// but a runaway writer must not make the watcher read without limit.
	constexpr std::size_t kMaxContractBytes = 16 * 1024;
	std::string content(kMaxContractBytes, '\0');
	fstream.read(content.data(), static_cast<std::streamsize>(content.size()));
	content.resize(static_cast<std::size_t>(fstream.gcount()));

	for (const auto& cmd : Utils::strsplit(content.c_str(), "\r\n"))
	{
		parseCmd(cmd);
	}
}

void SLSAPI::parseCmd(const std::string& cmd)
{
	LOG_DEBUG("API Running %s\n", cmd.c_str());

	const auto split = Utils::strsplit(cmd.c_str(), "|");
	// Size first: an empty line (or one made only of separators) produces no
	// fields at all, and split[0] on an empty vector is out of bounds. The old
	// order read split[0] before checking, so a blank write to the command file
	// took the client down.
	if (split.empty())
	{
		return;
	}

	const std::string& name = split[0];

	// Lua. Runs the plugins and a config reload here, on the watcher thread,
	// without cmdMutex: Steam's IPC thread takes it on every frame.
	if (name == "reloadlua")
	{
		if (const auto reload = reloadPlugins.load())
		{
			reload();
			LOG_API("Ran reloadlua!\n");
		}
		else
		{
			LOG_API("reloadlua: the plugin runtime is not loaded\n");
		}
		return;
	}

	AppId_t appId = 0;
	const std::lock_guard guard(cmdMutex);

	//Compatibility Manager
	if (name == "getcompat" && split.size() > 1)
	{
		if (!Utils::tryConvertToNumber(split[1].c_str(), appId))
		{
			LOG_ERROR("Failed to get compat for %s (not a number)!\n", split[1].c_str());
			return;
		}

		compatOps.emplace_back(CompatOp_t { CompatOp_t::OpType::Get, appId, "" } );
	}
	else if (name == "setcompat" && split.size() > 1)
	{
		if (!Utils::tryConvertToNumber(split[1].c_str(), appId))
		{
			LOG_ERROR("Failed to set compat for %s (not a number)!\n", split[1].c_str());
			return;
		}

		compatOps.emplace_back(CompatOp_t { CompatOp_t::OpType::Set, appId, split.size() > 2 ? split[2] : "" } );
	}
	//Application Manager
	else if (name == "dumplibraries")
	{
		libraryOps.emplace_back(LibraryOp_t { LibraryOp_t::OpType::Dump, 0, 0 } );
	}
	else if (name == "install" && split.size() > 2)
	{
		uint32_t library = 0;
		if (!Utils::tryConvertToNumber(split[1].c_str(), appId))
		{
			LOG_ERROR("Failed to install %s (not a number)!\n", split[1].c_str());
			return;
		}

		if (!Utils::tryConvertToNumber(split[2].c_str(), library))
		{
			LOG_ERROR("Failed to install %u to %s (not a number)!\n", appId, split[2].c_str());
			return;
		}

		libraryOps.emplace_back(LibraryOp_t { LibraryOp_t::OpType::Install, appId, library } );
	}
	else if (name == "uninstall" && split.size() > 1)
	{
		if (!Utils::tryConvertToNumber(split[1].c_str(), appId))
		{
			LOG_ERROR("Failed to uninstall %s (not a number)!\n", split[1].c_str());
			return;
		}

		libraryOps.emplace_back(LibraryOp_t { LibraryOp_t::OpType::Uninstall, appId, 0 } );
	}
}

void SLSAPI::init()
{
	// The contract file is prepared regardless of the current API setting, so a
	// config hot-reload from "no" to "yes" still has a watcher to notice a
	// command. isEnabled() is what gates acting on one, and it re-reads the
	// config every time. (An earlier revision returned here when the API was
	// off, which silently made enabling it require a Steam restart.)
	//
	// Preparing it costs one 0600 file in a 0700 per-user directory and no
	// thread beyond the watcher this function already created before.
	const std::string dir = RuntimeDir::resolveBase(getenv("XDG_RUNTIME_DIR"),
	                                                getenv("HOME"));
	if (dir.empty() || !RuntimeDir::ensureDir(dir))
	{
		g_pLog->info("API could not prepare a private runtime directory -> API unavailable\n");
		return;
	}

	contractPath = dir + "/api";
	path = contractPath.c_str();

	// Create the file ourselves: O_EXCL so a pre-created file is not adopted,
	// O_NOFOLLOW so a symlink is not followed, mode 0600 from the start rather
	// than narrowed afterwards. An existing file that IS already private is
	// reused, so a Steam restart keeps the channel.
	int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
	{
		fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
		if (fd >= 0 && !RuntimeDir::descriptorIsPrivate(fd))
		{
			g_pLog->info("API contract file is not private -> replacing it\n");
			close(fd);
			unlink(path);
			fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		}
	}
	if (fd < 0)
	{
		g_pLog->info("API could not open its contract file -> API unavailable\n");
		return;
	}
	close(fd);

	fstream = std::fstream(path, std::ios::in | std::ios::out);
	if (!fstream.is_open())
	{
		g_pLog->info("API contract file could not be opened for IO -> API unavailable\n");
		return;
	}

	watcher = new CFileWatcher(onFileChange);
	if (!watcher->addFile(path))
	{
		g_pLog->warn("API could not watch %s -> API unavailable\n", path);
		return;
	}
	watcher->start();

	g_pLog->debug("SLSsteam API initialized at %s\n", path);
}

void SLSAPI::runCompatOps()
{
	// Captured by the CCompatManager detour; commands wait in the queue until
	// Steam has called into the compatibility manager once.
	IClientCompat* const compat = g_pClientCompat;
	if (!compat)
	{
		return;
	}

	while (compatOps.size())
	{
		const auto op = compatOps.begin();
		switch(op->type)
		{
			case CompatOp_t::OpType::Get:
			{
				const char* name = compat->isCompatToolEnabled(op->appId)
					? compat->getCompatToolName(op->appId)
					: nullptr;
				if (!name)
				{
					LOG_API("Get compatibility tool for %u: Compatibility tool is disabled!\n", op->appId);
					break;
				}

				const char* displayName = compat->getDisplayName(name);
				LOG_API("Get compatibility tool for %u: \"%s\" (%s)\n", op->appId, displayName ? displayName : "", name);
				break;
			}
			case CompatOp_t::OpType::Set:
			{
				//Steam calls them with the same values
				compat->specifyCompatTool(op->appId, op->tool.c_str(), "", 250);
				LOG_API("Set compatibility tool for %u to %s\n", op->appId, op->tool.c_str());
				break;
			}
		}

		compatOps.erase(op);
	}
}

void SLSAPI::runInstallOps()
{
	IClientAppManager* const appManager = g_pClientAppManager;
	if (!appManager)
	{
		return;
	}

	while (libraryOps.size())
	{
		const auto op = libraryOps.begin();
		switch(op->type)
		{
			case LibraryOp_t::OpType::Dump:
			{
				const uint32_t num = appManager->getNumLibraryFolders();
				for (uint32_t i = 0; i < num; i++)
				{
					// Same sizes Steam passes. Zeroed and terminated here, so the
					// output never depends on what the returned lengths count.
					char pathBuf[0x1000] {};
					char labelBuf[0x80] {};
					const uint32_t pathLen = appManager->getLibraryFolderPath(i, pathBuf, sizeof(pathBuf));
					const uint32_t labelLen = appManager->getLibraryFolderLabel(i, labelBuf, sizeof(labelBuf));
					pathBuf[sizeof(pathBuf) - 1] = '\0';
					labelBuf[sizeof(labelBuf) - 1] = '\0';

					if (labelLen && labelBuf[0])
					{
						LOG_API("Library \"%s\" at \"%s\" has index %u\n", labelBuf, pathLen ? pathBuf : "", i);
					}
					else
					{
						LOG_API("Library at \"%s\" has index %u\n", pathLen ? pathBuf : "", i);
					}
				}
				break;
			}
			case LibraryOp_t::OpType::Install:
			{
				appManager->installApp(op->appId, op->libraryIndex);
				LOG_API("Installed %u to %u\n", op->appId, op->libraryIndex);
				break;
			}
			case LibraryOp_t::OpType::Uninstall:
			{
				appManager->uninstallApp(op->appId);
				LOG_API("Uninstalled %u\n", op->appId);
				break;
			}
		}

		libraryOps.erase(op);
	}
}

void SLSAPI::runIPCFrame()
{
	const std::lock_guard guard(cmdMutex);
	runCompatOps();
	runInstallOps();
}
