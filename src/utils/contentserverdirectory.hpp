// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ContentServerDirectory
{

enum class HttpsSupport
{
	Mandatory,
	Optional,
	Unavailable,
};

struct Server
{
	std::string type;
	std::string host;
	std::string vhost;
	HttpsSupport httpsSupport = HttpsSupport::Optional;
};

// Parse the JSON returned by Valve's GetServersForSteamPipe endpoint.
// The order is meaningful: the service has already ranked sources for the
// requesting client. Unsupported proxy entries and malformed hosts are
// ignored rather than guessed at.
std::vector<Server> parseServerList(std::string_view response);

// Build the standard SteamPipe manifest request for one directory entry.
std::string manifestUrl(const Server& server, uint32_t depotId,
                        uint64_t manifestGid, uint64_t requestCode);

// vhost is the HTTP Host header advertised by the directory. It normally
// equals host, but keeping the distinction makes CDN proxying explicit.
std::string hostHeader(const Server& server);

} // namespace ContentServerDirectory
