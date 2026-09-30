// TDD regression test for bounded Curl::downloadString requests.
//
// Curl::downloadString runs the system curl binary, so the stall is only exercised
// where one is installed; without it the call fails fast, which the checks
// below also accept.

#include "curl.hpp"
#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}
}

int main()
{
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	check(listener >= 0, "local listener created");
	if (listener < 0) return 1;

	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
	      "local listener bound");
	check(listen(listener, 1) == 0, "local listener started");
	if (failures != 0)
	{
		close(listener);
		return 1;
	}

	socklen_t addressLength = sizeof(address);
	getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressLength);
	const unsigned port = ntohs(address.sin_port);

	std::thread server([listener]
	{
		const int client = accept(listener, nullptr, nullptr);
		if (client >= 0)
		{
			char request[4096];
			(void)recv(client, request, sizeof(request), 0);
			pollfd waiter{client, POLLIN | POLLHUP | POLLERR, 0};
			(void)poll(&waiter, 1, 15000);
			close(client);
		}
		close(listener);
	});

	setenv("NO_PROXY", "127.0.0.1,localhost", 1);
	setenv("no_proxy", "127.0.0.1,localhost", 1);
	std::string response;
	const auto started = std::chrono::steady_clock::now();
	const int result = Curl::downloadString(
		("http://127.0.0.1:" + std::to_string(port) + "/stall").c_str(),
		response, 10);
	const auto elapsed = std::chrono::steady_clock::now() - started;
	// Wake the server if no request ever arrived (no curl binary installed).
	shutdown(listener, SHUT_RDWR);
	server.join();

	check(result != 0, "stalled request reports a curl error");
	check(elapsed < std::chrono::seconds(12),
	      "stalled request returns within the worker time budget");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_curl_timeout: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("curl timeout tests passed");
	return 0;
}
