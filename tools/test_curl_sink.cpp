// The provider, CM-directory and manifest-archive GETs share one bounded,
// non-throwing body sink and one redirect rule (make test-curl-sink).
#include "../src/utils/curl_sink.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg)                                               \
	do {                                                               \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
		else         { std::printf("ok:   %s\n", msg); }               \
	} while (0)

int main()
{
	std::string body;
	CurlBodySink sink{&body, 10};
	CHECK(curlBodySinkWrite("abcdef", 1, 6, &sink) == 6 && body == "abcdef",
	      "a chunk under the limit is kept");
	CHECK(curlBodySinkWrite("ghijk", 1, 5, &sink) == 0 && body == "abcdef",
	      "a chunk that would pass the limit ends the transfer and is not kept");
	CHECK(curlBodySinkWrite("ghij", 1, 4, &sink) == 4 && body.size() == 10,
	      "the body may fill the limit exactly");
	CHECK(curlBodySinkWrite("x", SIZE_MAX, 2, &sink) == 0 && body.size() == 10,
	      "an overflowing size * count is refused");
	CHECK(curlRedirectProtocols("https://api.steamcmd.net/v1/info/42") == CURLPROTO_HTTPS,
	      "an https request is redirected to https only");
	CHECK(curlRedirectProtocols("http://example.test/") == (CURLPROTO_HTTP | CURLPROTO_HTTPS),
	      "a plain http request may be redirected to http or https");
	if (g_failures == 0) std::printf("\nall curl sink checks passed\n");
	return g_failures == 0 ? 0 : 1;
}
