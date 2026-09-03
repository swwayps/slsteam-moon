// SPDX-License-Identifier: AGPL-3.0-only

#include "contentserverdirectory.hpp"

#include "../ascii.hpp"

#include "yaml-cpp/yaml.h"

#include <unordered_set>
#include <utility>

namespace ContentServerDirectory
{
namespace
{

std::string scalarString(const YAML::Node& node, const char* key)
{
	const YAML::Node value = node[key];
	if (!value || !value.IsScalar()) return {};
	try { return value.as<std::string>(); }
	catch (...) { return {}; }
}

bool scalarBool(const YAML::Node& node, const char* key)
{
	const YAML::Node value = node[key];
	if (!value || !value.IsScalar()) return false;
	try { return value.as<bool>(); }
	catch (...) { return false; }
}

bool safeAuthority(std::string_view value)
{
	if (value.empty() || value.size() > 255) return false;
	for (const unsigned char ch : value)
	{
		if (Ascii::isAlnum(ch) || ch == '.' || ch == '-' || ch == '_'
		    || ch == ':' || ch == '[' || ch == ']')
		{
			continue;
		}
		return false;
	}
	return true;
}

HttpsSupport parseHttpsSupport(std::string_view value)
{
	if (value == "mandatory") return HttpsSupport::Mandatory;
	if (value == "unavailable") return HttpsSupport::Unavailable;
	return HttpsSupport::Optional;
}

} // namespace

std::vector<Server> parseServerList(std::string_view response)
{
	std::vector<Server> out;
	try
	{
		const YAML::Node root = YAML::Load(std::string(response));
		const YAML::Node servers = root["response"]["servers"];
		if (!servers || !servers.IsSequence()) return out;

		std::unordered_set<std::string> seen;
		for (const auto& node : servers)
		{
			if (!node.IsMap() || scalarBool(node, "use_as_proxy")) continue;

			Server server;
			server.type = scalarString(node, "type");
			if (server.type != "SteamCache" && server.type != "CDN") continue;

			server.host = scalarString(node, "host");
			server.vhost = scalarString(node, "vhost");
			if (server.vhost.empty()) server.vhost = server.host;
			if (!safeAuthority(server.host) || !safeAuthority(server.vhost)) continue;

			server.httpsSupport = parseHttpsSupport(
			    scalarString(node, "https_support"));
			const std::string key = server.host + "\n" + server.vhost;
			if (!seen.insert(key).second) continue;
			out.push_back(std::move(server));
		}
	}
	catch (...)
	{
		out.clear();
	}
	return out;
}

std::string manifestUrl(const Server& server, uint32_t depotId,
                        uint64_t manifestGid, uint64_t requestCode)
{
	const char* scheme = server.httpsSupport == HttpsSupport::Unavailable
	                   ? "http://" : "https://";
	return std::string(scheme) + server.host + "/depot/"
	       + std::to_string(depotId) + "/manifest/"
	       + std::to_string(manifestGid) + "/5/"
	       + std::to_string(requestCode);
}

std::string hostHeader(const Server& server)
{
	return server.vhost.empty() ? server.host : server.vhost;
}

} // namespace ContentServerDirectory
