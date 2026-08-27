#include "api.hpp"

#include "sdk/IClientAppManager.hpp"

#include "afftrace.hpp"
#include "config.hpp"
#include "filewatcher.hpp"
#include "ownerwork.hpp"
#include "runtimedir.hpp"
#include "utils.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <ios>
#include <unistd.h>


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
}

bool SLSAPI::isEnabled()
{
	return g_config.api.get() && fstream.is_open();
}

void SLSAPI::onFileChange()
{
	// Runs on the API watcher pthread. The install request below enters
	// Steam-owned code, so it is handed to the owner IPC thread without waiting
	// (see ownerwork.hpp); parsing stays here.
	auto watchSpan = AffTrace::watchSpan(AffTrace::Src::Api);

	//Hot reload support :)
	if (!isEnabled())
	{
		return;
	}

	//Shitty way to reopen the stream. We have to do this, otherwise the fstream gets invalidated when running echo >
	fstream.close();
	fstream.open(path);

	char cmd[128];
	fstream.getline(cmd, sizeof(cmd));

	g_pLog->debug("API Running %s\n", cmd);

	auto split = Utils::strsplit(cmd, "|");
	// Size first: an empty line (or one made only of separators) produces no
	// fields at all, and split[0] on an empty vector is out of bounds. The old
	// order read split[0] before checking, so a blank write to the command file
	// took the client down.
	if (split.size() > 2 && split[0] == "install")
	{
		try
		{
			uint32_t appId = std::strtoul(split[1].c_str(), nullptr, 10);
			uint32_t library = std::strtoul(split[2].c_str(), nullptr, 10);

			if (!g_pClientAppManager)
			{
				g_pLog->info("API g_pClientAppManager is nullptr! Aborting...\n");
				return;
			}

			g_pLog->info("API Installing %s to %s\n", split[1].c_str(), split[2].c_str());

			// Same call, same arguments — executed on the owner IPC thread.
			// Non-blocking: the watcher does not wait for the owner.
			const auto mode = OwnerWork::submitInstallApp(appId, library);
			g_pLog->debug("API install request dispatched %s\n", OwnerWork::modeName(mode));
		}
		catch(...)
		{
			g_pLog->info("API Failed to parse %s or %s!\n", split[1].c_str(), split[2].c_str());
		}
	}
}

void SLSAPI::init()
{
	// Only prepare the channel when the user asked for it. It is off by default,
	// so in the default configuration there is nothing to open and nothing to
	// attack.
	if (!g_config.api.get())
	{
		g_pLog->debug("SLSsteam API disabled by configuration\n");
		return;
	}

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
	watcher->addFile(path);
	watcher->start();

	g_pLog->debug("SLSsteam API initialized at %s\n", path);
}
