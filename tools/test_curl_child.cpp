// Child-process hygiene of Curl::downloadString. The executable's own pipe, pipe2
// and execve definitions take precedence over libc's for the linked curl.cpp,
// so no curl binary is needed. Build and run: make test-curl-child
#include "curl.hpp"
#include "log.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

namespace
{
enum class Exec { Echo, Missing, Pauses };
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

void pauseMs(long ms)
{
	timespec left { ms / 1000, (ms % 1000) * 1000000 };
	while (nanosleep(&left, &left) == -1 && errno == EINTR) {}
}

// Echo mode prints argv and envp to the pipe instead of running curl. Pauses
// mode writes its output in two parts and closes the pipe before it exits,
// pausing in between, so the parent waits in read() and then in waitpid().
extern "C" int execve(const char*, char* const argv[], char* const envp[])
{
	if (execMode == Exec::Missing)
	{
		errno = ENOENT;
		return -1;
	}
	if (execMode == Exec::Pauses)
	{
		const std::string first = "first|";
		const std::string second = "second";
		if (write(STDOUT_FILENO, first.data(), first.size()) < 0) _exit(2);
		pauseMs(300);
		if (write(STDOUT_FILENO, second.data(), second.size()) < 0) _exit(2);
		// Nothing was exec'd, so the close-on-exec pipe end is still open here.
		for (int fd = 1; fd < 1024; fd++) close(fd);
		pauseMs(300);
		_exit(0);
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
	check(Curl::downloadString("http://127.0.0.1:9/", out) == 0, "the stand-in curl ran");
	check(out.rfind("curl\n", 0) == 0, "argv[0] is the program name");
	check(out.find("\n--silent\n") != std::string::npos, "--silent is passed as an option");
	check(out.find("\nenv:PATH=/usr/bin:/bin\n") != std::string::npos, "the child PATH is a plain list");
	check(out.find("\n--max-time\n15\n") != std::string::npos, "the default timeout bounds the whole transfer");
	check(Curl::downloadString("http://127.0.0.1:9/", {"Accept: text/plain"}, out, 3) == 0
	          && out.find("\n-H\nAccept: text/plain\n") != std::string::npos
	          && out.find("\n--max-time\n3\n") != std::string::npos
	          && out.find("\nhttp://127.0.0.1:9/\n") != std::string::npos,
	      "headers and the timeout reach curl as separate arguments");

	parentPid = getpid();
	const int fd = mkstemp(marker);
	close(fd);
	unlink(marker);
	std::atexit(createMarker);
	execMode = Exec::Missing;
	check(Curl::downloadString("http://127.0.0.1:9/", out) != 0, "a missing curl is an error");
	check(access(marker, F_OK) != 0, "the child leaves without the parent's exit handlers");
	unlink(marker);

	spawnOnPipe = true;
	const auto started = std::chrono::steady_clock::now();
	(void)Curl::downloadString("http://127.0.0.1:9/", out);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	spawnOnPipe = false;
	check(elapsed < std::chrono::seconds(2),
	      "a program started meanwhile does not keep the pipe open");
	while (wait(nullptr) > 0) {}

	// A signal handler installed without SA_RESTART makes blocking calls in
	// this thread return EINTR; Steam's own handlers can do the same.
	struct sigaction onAlarm {};
	onAlarm.sa_handler = [](int) {};
	sigemptyset(&onAlarm.sa_mask);
	sigaction(SIGALRM, &onAlarm, nullptr);
	const itimerval every20ms { { 0, 20000 }, { 0, 20000 } };
	setitimer(ITIMER_REAL, &every20ms, nullptr);
	execMode = Exec::Pauses;
	const int interrupted = Curl::downloadString("http://127.0.0.1:9/", out);
	const itimerval off {};
	setitimer(ITIMER_REAL, &off, nullptr);
	check(interrupted == 0 && out == "first|second",
	      "signals during the transfer neither cut the output nor fail it");

	return failures ? 1 : 0;
}
