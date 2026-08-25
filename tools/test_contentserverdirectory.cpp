// SPDX-License-Identifier: AGPL-3.0-only
//
// Pure tests for Valve's GetServersForSteamPipe response handling.  The
// production fetch stays in ManifestFetch; this test pins the untrusted JSON
// boundary and URL construction without touching the network.

#include "../src/utils/contentserverdirectory.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

int main()
{
	const std::string response = R"json(
{
  "response": {
    "servers": [
      {
        "type": "SteamCache",
        "host": "cache-primary.example.com",
        "vhost": "cache-primary.example.com",
        "https_support": "mandatory"
      },
      {
        "type": "CDN",
        "host": "steampipe.akamaized.net",
        "vhost": "steampipe.akamaized.net",
        "https_support": "optional"
      },
      {
        "type": "CDN",
        "host": "legacy.cdn.steampipe.steamcontent.com",
        "vhost": "legacy-vhost.cdn.steampipe.steamcontent.com",
        "https_support": "unavailable"
      },
      {
        "type": "SteamCache",
        "host": "cache-primary.example.com",
        "vhost": "cache-primary.example.com",
        "https_support": "mandatory"
      },
      {
        "type": "Unsupported",
        "host": "ignored.example.com",
        "https_support": "mandatory"
      },
      {
        "type": "SteamCache",
        "host": "proxy.example.com",
        "vhost": "cache.example.com",
        "use_as_proxy": true,
        "proxy_request_path_template": "/proxy/{path}"
      },
      {
        "type": "SteamCache",
        "host": "bad.example.com/path",
        "https_support": "mandatory"
      }
    ]
  }
}
)json";

	const auto servers = ContentServerDirectory::parseServerList(response);
	CHECK(servers.size() == 3,
	      "keeps direct SteamCache/CDN entries and drops invalid, duplicate, or proxy entries");

	if (servers.size() == 3)
	{
		CHECK(servers[0].host == "cache-primary.example.com",
		      "preserves Valve's server order");
		CHECK(ContentServerDirectory::manifestUrl(
		          servers[0], 991353, 1762605700935460747ULL,
		          17590594762285008732ULL) ==
		      "https://cache-primary.example.com/depot/991353/manifest/"
		      "1762605700935460747/5/17590594762285008732",
		      "mandatory HTTPS server builds an HTTPS manifest URL");
		CHECK(ContentServerDirectory::manifestUrl(
		          servers[1], 42, 43, 44) ==
		      "https://steampipe.akamaized.net/depot/42/manifest/43/5/44",
		      "optional HTTPS server prefers HTTPS");
		CHECK(ContentServerDirectory::manifestUrl(
		          servers[2], 7, 8, 9) ==
		      "http://legacy.cdn.steampipe.steamcontent.com/depot/7/manifest/8/5/9",
		      "server without HTTPS support uses HTTP");
		CHECK(ContentServerDirectory::hostHeader(servers[2]) ==
		      "legacy-vhost.cdn.steampipe.steamcontent.com",
		      "separate vhost is preserved for the HTTP Host header");
	}

	CHECK(ContentServerDirectory::parseServerList("not json").empty(),
	      "malformed directory response fails closed");
	CHECK(ContentServerDirectory::parseServerList("{\"response\":{}}").empty(),
	      "response without a server list fails closed");

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
