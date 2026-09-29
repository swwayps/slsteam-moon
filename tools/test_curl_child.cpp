// Child-process hygiene of Curl::getString. The executable's own pipe, pipe2
// and execve definitions take precedence over libc's for the linked curl.cpp,
// so no curl binary is needed. Build and run: make test-curl-child
#include "curl.hpp"
#include "log.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

namespace
{
enum class Exec { Echo, Missing };
Exec execMode = Exec::Echo;
bool spawnOnPipe = false;
char marker[] = "/tmp/test_curl_child_XXXXXX";
pid_t parentPid = 0;
int failures = 0;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}

// Another Steam thread starting a program right after the pipe exists.
void spawnSleeper()
{
	if (fork() == 0)
	{
		char arg0[] = "sleep";
		char arg1[] = "3";
		char* const argv[] = { arg0, arg1, nullptr };
		char* const envp[] = { nullptr };
		syscall(SYS_execve, "/bin/sleep", argv, envp);
		_exit(127);
	}
}

// Exit handler: only a forked child that runs exit() creates the marker.
void createMarker()
{
	if (getpid() == parentPid) return;
	if (FILE* f = std::fopen(marker, "w"))
	{
		std::fclose(f);
	}
}
}

extern "C" int pipe(int fds[2])
{
	const int rc = static_cast<int>(syscall(SYS_pipe2, fds, 0));
	if (rc == 0 && spawnOnPipe) spawnSleeper();
	return rc;
}

extern "C" int pipe2(int fds[2], int flags)
{
	const int rc = static_cast<int>(syscall(SYS_pipe2, fds, flags));
	if (rc == 0 && spawnOnPipe) spawnSleeper();
	return rc;
}

// Echo mode prints argv and envp to the pipe instead of running curl.
extern "C" int execve(const char*, char* const argv[], char* const envp[])
{
	if (execMode == Exec::Missing)
	{
		errno = ENOENT;
		return -1;
	}
	std::string lines;
	for (int i = 0; argv[i]; i++) lines.append(argv[i]).append("\n");
	for (int i = 0; envp[i]; i++) lines.append("env:").append(envp[i]).append("\n");
	if (write(STDOUT_FILENO, lines.data(), lines.size()) < 0) _exit(2);
	_exit(0);
}

int main()
{
	std::string out;
	execMode = Exec::Echo;
	check(Curl::getString("http://127.0.0.1:9/", out) == 0, "the stand-in curl ran");
	check(out.rfind("curl\n", 0) == 0, "argv[0] is the program name");
	check(out.find("\n--silent\n") != std::string::npos, "--silent is passed as an option");
	check(out.find("\nenv:PATH=/usr/bin:/bin\n") != std::string::npos, "the child PATH is a plain list");

	parentPid = getpid();
	const int fd = mkstemp(marker);
	close(fd);
	unlink(marker);
	std::atexit(createMarker);
	execMode = Exec::Missing;
	check(Curl::getString("http://127.0.0.1:9/", out) != 0, "a missing curl is an error");
	check(access(marker, F_OK) != 0, "the child leaves without the parent's exit handlers");
	unlink(marker);

	spawnOnPipe = true;
	const auto started = std::chrono::steady_clock::now();
	(void)Curl::getString("http://127.0.0.1:9/", out);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	spawnOnPipe = false;
	check(elapsed < std::chrono::seconds(2),
	      "a program started meanwhile does not keep the pipe open");
	while (wait(nullptr) > 0) {}

	return failures ? 1 : 0;
}
