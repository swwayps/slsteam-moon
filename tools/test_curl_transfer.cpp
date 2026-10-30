// runCurlTransfer: on a glibc with the audit TLS bug (< 2.43) the transfer
// runs on a thread of its own and the caller waits; elsewhere it runs on the
// caller's thread. The result and any exception reach the caller either way.
// Build: g++ -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -isystem include -I src tools/test_curl_transfer.cpp -pthread -o /tmp/test_curl_transfer && /tmp/test_curl_transfer
#include "utils/curl_api.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

static int g_failures = 0;
static void check(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++g_failures;
}

int main()
{
	const auto caller = std::this_thread::get_id();
	std::thread::id ranOn;
	const int value = runCurlTransfer([&] {
		ranOn = std::this_thread::get_id();
		return 42;
	});
	check(value == 42, "the transfer's result reaches the caller");
	check((ranOn != caller) == Utils::isGLIBCBugged(),
	      "the transfer gets its own thread exactly on an affected glibc");

	std::string body;
	(void)runCurlTransfer([&] { body = "filled"; return true; });
	check(body == "filled", "the transfer writes through the caller's references");

	bool caught = false;
	try
	{
		(void)runCurlTransfer([]() -> int { throw std::runtime_error("boom"); });
	}
	catch (const std::runtime_error&)
	{
		caught = true;
	}
	check(caught, "an exception from the transfer reaches the caller");

	int major = 0;
	int minor = 0;
	(void)Utils::isGLIBCBugged(&major, &minor);
	std::printf("glibc %d.%d affected=%d\n", major, minor, Utils::isGLIBCBugged() ? 1 : 0);
	return g_failures == 0 ? 0 : 1;
}
