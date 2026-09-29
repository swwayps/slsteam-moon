#include "curl.hpp"

#include "log.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


//Spawn an external instance of curl, read it's stdout into out and return it's exit code
//It's necessary because SteamOS seems broken. Curling certain URLs
//will crash inside libssl.3.so (might have to do with broken certs, idk for sure).
int Curl::getString(const char* url, std::string& out)
{
	g_pLog->debug("Curl::getString(%s)\n", url);

	// Close-on-exec: a program Steam starts on another thread meanwhile would
	// otherwise inherit the write end and hold the read below open.
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) == -1)
	{
		g_pLog->debug("Failed to create pipe!\n");
		return 1;
	}

	g_pLog->debug("Created pipe %i : %i\n", pipefd[0], pipefd[1]);

	constexpr static const char* env[] =
	{
		"PATH=/usr/bin:/bin",
		nullptr
	};

	// Bounded like the in-process libcurl client this replaced: a stalled
	// server must not hold the calling Steam thread. --max-time caps the whole
	// transfer at 10 s, the connect phase included.
	const char* args[] =
	{
		"curl",
		"--silent",
		"--connect-timeout", "15",
		"--max-time", "10",
		url,
		nullptr
	};

	const pid_t pid = fork();
	if (pid == -1)
	{
		close(pipefd[0]);
		close(pipefd[1]);

		g_pLog->debug("Failed to fork!\n");
		return 1;
	}

	if (pid == 0)
	{
		// Only async-signal-safe calls until exec: in a copy of the threaded
		// Steam process the log's lock or exit()'s handlers can deadlock or
		// run Steam's teardown. The pipe ends close on exec.
		if (dup2(pipefd[1], STDOUT_FILENO) == -1)
		{
			_exit(1);
		}

		execve("/bin/curl", const_cast<char**>(args), const_cast<char**>(env));
		execve("/usr/bin/curl", const_cast<char**>(args), const_cast<char**>(env));
		//NixOS
		execve("/run/current-system/sw/bin/curl", const_cast<char**>(args), const_cast<char**>(env));

		_exit(127);
	}

	//No need for writing
	close(pipefd[1]);

	g_pLog->debug("Child PID %i\n", pid);

	std::ostringstream bufSS;
	char buf[8192];
	int numRead;

	while((numRead = read(pipefd[0], buf, sizeof(buf))) > 0)
	{
		bufSS << std::string(buf, numRead);
	}

	close(pipefd[0]);

	int status;
	if(waitpid(pid, &status, 0) == -1)
	{
		return 1;
	}

	if(!WIFEXITED(status))
	{
		return 1;
	}

	status = WEXITSTATUS(status);

	g_pLog->debug("Exit Status: %i\n", status);

	out = bufSS.str();

	return status;
}
