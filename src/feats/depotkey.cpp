
#include "depotkey.hpp"

#include "depotkey_scope.hpp"

#include "../config.hpp"
#include "../globals.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/EResult.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/emittermanip.h"
#include "yaml-cpp/yaml.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace DepotKey
{

namespace
{

std::mutex g_cacheMu;
std::map<uint32_t, SavedKey> g_keyMap;

std::mutex g_pendingMu;
std::map<uint32_t /*depotId*/, uint32_t /*appId*/> g_pendingReqs;

bool g_startupDone = false;

std::string hexToBytes(const std::string& hex)
{
	if (hex.size() != 64) return {};
	std::string out;
	out.reserve(32);
	auto nib = [](char c) -> int
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
		if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2)
	{
		int hi = nib(hex[i]);
		int lo = nib(hex[i + 1]);
		if (hi < 0 || lo < 0) return {};
		out.push_back(static_cast<char>((hi << 4) | lo));
	}
	return out;
}

std::vector<std::string> steamPathCandidates()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	return {
		std::string(home) + "/.steam/steam",
		std::string(home) + "/.steam/debian-installation",
		std::string(home) + "/.local/share/Steam",
	};
}

std::string findSteamRoot()
{
	for (const auto& candidate : steamPathCandidates())
	{
		if (std::filesystem::exists(candidate + "/steam.sh"))
		{
			return candidate;
		}
	}
	return {};
}

} // namespace


std::string getKeyDir()
{
	std::stringstream ss;
	ss << g_config.getDir().c_str() << "/cache";
	const auto dir = ss.str();
	if (!std::filesystem::exists(dir.c_str()))
	{
		std::filesystem::create_directory(dir.c_str());
	}
	return dir;
}

std::string getKeyPath(uint32_t depotId)
{
	std::stringstream ss;
	ss << getKeyDir().c_str() << "/depotkey_" << depotId << ".yaml";
	return ss.str();
}


SavedKey getCachedKey(uint32_t depotId)
{
	{
		std::lock_guard<std::mutex> lk(g_cacheMu);
		auto it = g_keyMap.find(depotId);
		if (it != g_keyMap.end()) return it->second;
	}

	SavedKey k;
	const auto path = getKeyPath(depotId);
	if (!std::filesystem::exists(path.c_str())) return k;

	try
	{
		auto node = YAML::LoadFile(path);
		k.appId = node["appId"].as<uint32_t>();
		k.depotId = node["depotId"].as<uint32_t>();
		k.key = std::string(base64::from_base64(node["key"].as<std::string>()));
		// Legacy catalog files (pre managed-tracking) lack the field; a
		// missing flag means "observed" (the safe default — Steam handles
		// it).  Lua re-import at startup upgrades genuine LuaTools depots.
		if (node["managed"]) k.managed = node["managed"].as<bool>();
	}
	catch (const std::exception& e)
	{
		g_pLog->debug("DepotKey: failed to load %s: %s\n", path.c_str(), e.what());
		return {};
	}

	std::lock_guard<std::mutex> lk(g_cacheMu);
	g_keyMap[depotId] = k;
	return k;
}

bool saveKeyToCache(uint32_t appId, uint32_t depotId, const std::string& key, bool managed)
{
	if (key.size() != 32)
	{
		g_pLog->debug("DepotKey: refusing to cache key for depot %u (size %zu, expected 32)\n",
		              depotId, key.size());
		return false;
	}

	SavedKey existing;
	const auto path = getKeyPath(depotId);
	bool fileExists = std::filesystem::exists(path.c_str());
	if (fileExists)
	{
		try
		{
			auto node = YAML::LoadFile(path);
			existing.appId = node["appId"].as<uint32_t>();
			existing.depotId = node["depotId"].as<uint32_t>();
			existing.key = std::string(base64::from_base64(node["key"].as<std::string>()));
			if (node["managed"]) existing.managed = node["managed"].as<bool>();
		}
		catch (...) { existing = {}; }
	}

	uint32_t finalAppId = appId ? appId : existing.appId;
	// managed is sticky: a passive re-observation must never downgrade a
	// LuaTools depot out of manifest scope (depotkey_scope.hpp).
	const bool finalManaged = mergeManagedFlag(existing.managed, managed);
	if (existing.depotId == depotId && existing.key == key
	    && existing.appId == finalAppId && existing.managed == finalManaged)
	{
		std::lock_guard<std::mutex> lk(g_cacheMu);
		g_keyMap[depotId] = existing;
		return true;
	}

	YAML::Emitter node;
	node << YAML::BeginMap;
	node << YAML::Key << "appId";
	node << YAML::Value << finalAppId;
	node << YAML::Key << "depotId";
	node << YAML::Value << depotId;
	node << YAML::Key << "key";
	node << YAML::Value << base64::to_base64(key);
	node << YAML::Key << "managed";
	node << YAML::Value << finalManaged;
	node << YAML::EndMap;

	std::ofstream ofs(path.c_str(), std::ios::out);
	if (!ofs.is_open())
	{
		g_pLog->debug("DepotKey: cannot write %s\n", path.c_str());
		return false;
	}
	ofs.write(node.c_str(), node.size());

	g_pLog->infoOnce("DepotKey: cached key for app=%u depot=%u\n", finalAppId, depotId);

	SavedKey k;
	k.appId = finalAppId;
	k.depotId = depotId;
	k.key = key;
	k.managed = finalManaged;
	std::lock_guard<std::mutex> lk(g_cacheMu);
	g_keyMap[depotId] = k;
	return true;
}


bool isManagedDepot(uint32_t depotId)
{
	const auto k = getCachedKey(depotId);
	return k.managed && k.key.size() == 32;
}

std::vector<uint32_t> managedDepotsForApp(uint32_t appId)
{
	std::vector<uint32_t> out;
	if (appId == 0) return out;

	const auto dir = getKeyDir();
	std::error_code ec;
	if (!std::filesystem::exists(dir, ec)) return out;

	std::set<uint32_t> seen;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
	{
		if (ec) break;
		const auto& p = entry.path();
		const auto name = p.filename().string();
		if (name.rfind("depotkey_", 0) != 0 || p.extension() != ".yaml")
			continue;

		try
		{
			auto node = YAML::LoadFile(p.string());
			if (!node["appId"] || !node["depotId"]) continue;
			if (node["appId"].as<uint32_t>() != appId) continue;
			// Only MANAGED (Lua-injected) depots: an observed owned-game /
			// runtime key must never be synthesized into an app's appinfo.
			const bool managed = node["managed"] && node["managed"].as<bool>();
			if (!managed) continue;
			const uint32_t depotId = node["depotId"].as<uint32_t>();
			if (depotId && seen.insert(depotId).second)
				out.push_back(depotId);
		}
		catch (...) { continue; }
	}
	return out;
}


void importLuaScripts()
{
	const auto steamRoot = findSteamRoot();
	if (steamRoot.empty()) return;

	const auto stplug = steamRoot + "/config/stplug-in";
	if (!std::filesystem::exists(stplug.c_str())) return;


	static const std::regex addappidWithKeyRe(
		"addappid\\s*\\(\\s*(\\d+)\\s*,\\s*\\d+\\s*,\\s*\"([0-9A-Fa-f]{64})\"\\s*\\)"
	);

	int imported = 0;
	for (const auto& entry : std::filesystem::directory_iterator(stplug))
	{
		if (!entry.is_regular_file()) continue;
		const auto& path = entry.path();
		if (path.extension() != ".lua") continue;

		std::ifstream ifs(path);
		if (!ifs.is_open()) continue;

		uint32_t appIdGuess = 0;
		try { appIdGuess = static_cast<uint32_t>(std::stoul(path.stem().string())); }
		catch (...) {}

		// Line-by-line so we can strip Lua comments (`-- ...`); a
		// commented-out `--addappid(d,1,"key")` must NOT be imported.
		std::string line;
		while (std::getline(ifs, line))
		{
			const auto commentPos = line.find("--");
			if (commentPos != std::string::npos)
			{
				line.erase(commentPos);
			}

			auto begin = std::sregex_iterator(line.begin(), line.end(), addappidWithKeyRe);
			auto end = std::sregex_iterator();
			for (auto it = begin; it != end; ++it)
			{
				const uint32_t depotId = static_cast<uint32_t>(std::stoul((*it)[1].str()));
				const std::string keyHex = (*it)[2].str();
				const std::string keyBin = hexToBytes(keyHex);
				if (keyBin.size() != 32) continue;
				if (saveKeyToCache(appIdGuess ? appIdGuess : depotId, depotId, keyBin, /*managed=*/true))
				{
					++imported;
				}
			}
		}
	}
	if (imported > 0)
	{
		g_pLog->infoOnce("DepotKey: imported %d Lua-script depot keys from %s\n",
		             imported, stplug.c_str());
	}
}


void provisionManifests()
{
	const auto steamRoot = findSteamRoot();
	if (steamRoot.empty()) return;

	const auto src = steamRoot + "/config/depotcache";
	const auto dst = steamRoot + "/depotcache";
	if (!std::filesystem::exists(src.c_str())) return;
	if (!std::filesystem::exists(dst.c_str()))
	{
		try { std::filesystem::create_directories(dst.c_str()); }
		catch (...) { return; }
	}

	int copied = 0;
	for (const auto& entry : std::filesystem::directory_iterator(src))
	{
		if (!entry.is_regular_file()) continue;
		const auto& path = entry.path();
		if (path.extension() != ".manifest") continue;

		const auto target = std::filesystem::path(dst) / path.filename();
		std::error_code ec;
		bool needsCopy = !std::filesystem::exists(target);
		if (!needsCopy)
		{
			auto srcSize = std::filesystem::file_size(path, ec);
			auto dstSize = std::filesystem::file_size(target, ec);
			needsCopy = (!ec && srcSize != dstSize);
		}
		if (needsCopy)
		{
			std::filesystem::copy_file(
				path, target,
				std::filesystem::copy_options::overwrite_existing, ec);
			if (!ec) ++copied;
		}
	}
	if (copied > 0)
	{
		g_pLog->infoOnce("DepotKey: provisioned %d manifest(s) from %s -> %s\n",
		             copied, src.c_str(), dst.c_str());
	}
}

void disableShaderCache()
{
	const auto added = g_config.addedAppIds.get();
	if (added.empty()) return;   // nothing faked → leave shader cache alone

	const auto root = findSteamRoot();
	if (root.empty()) return;

	const auto path = root + "/config/config.vdf";
	if (!std::filesystem::exists(path)) return;

	std::string content;
	{
		std::ifstream ifs(path);
		if (!ifs.is_open()) return;
		std::stringstream ss;
		ss << ifs.rdbuf();
		content = ss.str();
	}

	if (content.find("\"DisableShaderCache\"") != std::string::npos)
	{
		g_pLog->debug("DepotKey: DisableShaderCache already in config.vdf\n");
		return;
	}

	const auto anchor = content.find("\"ShaderCacheManager\"");
	if (anchor == std::string::npos) return;

	const auto brace = content.find('{', anchor);
	if (brace == std::string::npos) return;

	const std::string insertion =
		"\n\t\t\t\t\t\t\"DisableShaderCache\"\t\t\"1\"";
	content.insert(brace + 1, insertion);

	{
		std::ofstream ofs(path, std::ios::trunc);
		if (!ofs.is_open()) return;
		ofs << content;
	}
	g_pLog->infoOnce("DepotKey: injected DisableShaderCache=1 into config.vdf "
	             "(AdditionalApps present)\n");
}


void onStartup()
{
	if (g_startupDone) return;
	g_startupDone = true;
	importLuaScripts();
	provisionManifests();
	disableShaderCache();
}


void recvDepotKey(CMsgClientGetDepotDecryptionKeyResponse* resp)
{
	if (!resp) return;

	uint32_t appId = 0;
	{
		std::lock_guard<std::mutex> lk(g_pendingMu);
		auto it = g_pendingReqs.find(resp->depot_id());
		if (it != g_pendingReqs.end())
		{
			appId = it->second;
			g_pendingReqs.erase(it);
		}
	}

	g_pLog->debug("DepotKey: incoming response for depot=%u eresult=%u keysize=%zu (correlated app=%u)\n",
	              resp->depot_id(), resp->eresult(),
	              resp->depot_encryption_key().size(), appId);

	if (resp->eresult() == ERESULT_OK)
	{
		if (resp->depot_encryption_key().size() == 32)
		{
			// Observed from a legitimate Steam response — cache it for
			// possible substitution, but mark it unmanaged so it does NOT
			// drag an owned game / runtime into our manifest scope.
			saveKeyToCache(appId, resp->depot_id(), resp->depot_encryption_key(), /*managed=*/false);
		}
		return;
	}

	auto cached = getCachedKey(resp->depot_id());
	if (cached.depotId != 0 && cached.key.size() == 32)
	{
		g_pLog->infoOnce("DepotKey: substituting cached key for depot %u (Steam said eresult=%u)\n",
		             resp->depot_id(), resp->eresult());

		CMsgClientGetDepotDecryptionKeyResponse fresh;
		fresh.set_eresult(ERESULT_OK);
		fresh.set_depot_id(resp->depot_id());
		fresh.set_depot_encryption_key(cached.key);
		resp->ParseFromString(fresh.SerializeAsString());
		return;
	}

	const uint32_t depotId = resp->depot_id();
	const bool isAdditional = g_config.isAddedAppId(depotId);
	if (isAdditional)
	{
		const std::string zeroKey(32, '\0');
		g_pLog->infoOnce("DepotKey: synthesising zero key for AdditionalApps depot %u (Steam said eresult=%u)\n",
		             depotId, resp->eresult());
		CMsgClientGetDepotDecryptionKeyResponse fresh;
		fresh.set_eresult(ERESULT_OK);
		fresh.set_depot_id(depotId);
		fresh.set_depot_encryption_key(zeroKey);
		resp->ParseFromString(fresh.SerializeAsString());
		return;
	}

	g_pLog->debug("DepotKey: no cached key for depot %u (Steam said eresult=%u)\n",
	              depotId, resp->eresult());
}

void sendDepotKey(CMsgClientGetDepotDecryptionKey* req)
{
	if (!req) return;
	{
		std::lock_guard<std::mutex> lk(g_pendingMu);
		g_pendingReqs[req->depot_id()] = req->app_id();
	}
	g_pLog->debug("DepotKey: outgoing request for app=%u depot=%u\n",
	              req->app_id(), req->depot_id());
}

void recvMsg(CProtoBufMsgBase* msg)
{
	switch (msg->type)
	{
		case EMSG_GET_DEPOT_DECRYPTION_KEY_RESPONSE:
			recvDepotKey(msg->getBody<CMsgClientGetDepotDecryptionKeyResponse>());
			break;
		default:
			break;
	}
}

void sendMsg(CProtoBufMsgBase* msg)
{
	switch (msg->type)
	{
		case EMSG_GET_DEPOT_DECRYPTION_KEY:
			sendDepotKey(msg->getBody<CMsgClientGetDepotDecryptionKey>());
			break;
		default:
			break;
	}
}

} // namespace DepotKey
