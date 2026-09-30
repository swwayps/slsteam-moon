#include "curl.hpp"
#include "log.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>


//Spawn an external instance of curl, read it's stdout into out and return it's exit code
//It's necessary because SteamOS seems broken. Curling certain URLs
//will crash inside libssl.3.so (might have to do with broken certs, idk for sure).
int Curl::downloadString(const char* url, std::string& out, const int timeOut)
{
	return downloadString(url, { }, out, timeOut);
}

int Curl::downloadString(const char* url, const std::vector<std::string>& headers, std::string& out, const int timeOut)
{
	LOG_DEBUG("Curl::downloadString(%s)\n", url);

	// Close-on-exec: a program Steam starts on another thread meanwhile would
	// otherwise inherit the write end and hold the read below open.
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) == -1)
	{
		LOG_ERROR("Failed to create pipe!\n");
		return 1;
	}

	LOG_DEBUG("Created pipe %i : %i\n", pipefd[0], pipefd[1]);

	constexpr static const char* env[] =
	{
		"PATH=/usr/bin:/bin",
		nullptr
	};

	// timeOut bounds the whole transfer, the connect phase included: a
	// stalled server must not hold the calling Steam thread. argv is built
	// before fork(), the child only execs.
	const std::string seconds = std::to_string(timeOut);
	std::vector<const char*> args =
	{
		"curl",
		"--silent",
		"--connect-timeout", seconds.c_str(),
		"--max-time", seconds.c_str(),
	};
	for (const auto& header : headers)
	{
		args.push_back("-H");
		args.push_back(header.c_str());
	}
	args.push_back(url);
	args.push_back(nullptr);

	const pid_t pid = fork();
	if (pid == -1)
	{
		close(pipefd[0]);
		close(pipefd[1]);

		LOG_ERROR("Failed to fork!\n");
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

		execve("/bin/curl", const_cast<char**>(args.data()), const_cast<char**>(env));
		execve("/usr/bin/curl", const_cast<char**>(args.data()), const_cast<char**>(env));
		//NixOS
		execve("/run/current-system/sw/bin/curl", const_cast<char**>(args.data()), const_cast<char**>(env));
		_exit(127);
	}

	//No need for writing
	close(pipefd[1]);

	LOG_DEBUG("Child PID %i\n", pid);

	// A signal handled on this thread interrupts read() and waitpid() with
	// EINTR; that is not the end of the transfer.
	std::ostringstream bufSS;
	char buf[8192];
	ssize_t numRead;
	bool readFailed = false;
	while((numRead = read(pipefd[0], buf, sizeof(buf))) != 0)
	{
		if (numRead < 0)
		{
			if (errno == EINTR) continue;
			readFailed = true;
			break;
		}
		bufSS.write(buf, numRead);
	}

	close(pipefd[0]);

	int status;
	pid_t waited;
	do
	{
		waited = waitpid(pid, &status, 0);
	} while (waited == -1 && errno == EINTR);
	if (waited == -1 || readFailed)
	{
		return 1;
	}

	if (!WIFEXITED(status))
	{
		return 1;
	}

	status = WEXITSTATUS(status);
	LOG_DEBUG("Exit Status: %i\n", status);

	out = bufSS.str();

	return status;
}
