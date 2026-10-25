
#include "depotkey.hpp"

#include "depotkey_scope.hpp"
#include "depotkey_import.hpp"
#include "depotkey_index.hpp"
#include "appinfo_provision.hpp"
#include "manifeststore_io.hpp"
#include "stats_policy.hpp"

#include "../config.hpp"
#include "../config_discovery.hpp"
#include "../globals.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/process_lock.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/types.hpp"

#include "base64/base64.hpp"
#include "yaml-cpp/emitter.h"
#include "yaml-cpp/emittermanip.h"
#include "yaml-cpp/yaml.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace DepotKey
{

namespace
{

std::mutex g_cacheMu;
DepotKey::LazyIndex<uint32_t, SavedKey> g_keyIndex;
DepotKey::ManagedDepotIndex g_managedDepotIndex;
bool g_managedDepotIndexReady = false;

std::mutex g_pendingMu;
std::map<uint32_t /*depotId*/, uint32_t /*appId*/> g_pendingReqs;

bool g_startupDone = false;
LuaScriptImportGate g_importGate;

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
		// setup() and the config watcher call this: an unresolvable candidate
		// is skipped, not thrown.
		std::error_code ec;
		if (std::filesystem::exists(candidate + "/steam.sh", ec))
		{
			return candidate;
		}
	}
	return {};
}

bool parseCachedKeyFile(const std::filesystem::path& path, SavedKey& out)
{
	try
	{
		auto node = YAML::LoadFile(path.string());
		if (!node["appId"] || !node["depotId"] || !node["key"])
			return false;
		out = {};
		out.appId = node["appId"].as<uint32_t>();
		out.depotId = node["depotId"].as<uint32_t>();
		out.key = std::string(base64::from_base64(node["key"].as<std::string>()));
		if (node["managed"]) out.managed = node["managed"].as<bool>();
		return out.depotId != 0;
	}
	catch (const std::exception& e)
	{
		g_pLog->debug("DepotKey: failed to load %s: %s\n",
		              path.c_str(), e.what());
		return false;
	}
}

// Caller holds g_cacheMu.  The catalog is deliberately loaded lazily: most
// boots never need a depot key before the importer, and one directory walk
// plus one YAML parse per file is cheaper than reloading the same file for
// every Lua-script match.
void loadKeyIndexLocked()
{
	g_keyIndex.loadOnce([](auto& entries) {
		const auto dir = getKeyDir();
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec) || ec) return;

		static const std::regex fileRe("depotkey_(\\d+)\\.yaml");
		std::filesystem::directory_iterator it(dir, ec);
		const std::filesystem::directory_iterator end;
		for (; it != end && !ec; it.increment(ec))
		{
			if (!it->is_regular_file()) continue;
			const auto name = it->path().filename().string();
			if (!std::regex_match(name, fileRe)) continue;

			SavedKey key;
			if (parseCachedKeyFile(it->path(), key))
				entries[key.depotId] = std::move(key);
		}
	});
	if (!g_managedDepotIndexReady)
	{
		g_managedDepotIndex.rebuild(g_keyIndex.entries());
		g_managedDepotIndexReady = true;
	}
}

} // namespace


std::string getKeyDir()
{
	std::stringstream ss;
	ss << g_config.getDir().c_str() << "/cache";
	// Runs on Steam's network and IPC paths: never throw into Steam.
	const auto dir = ss.str();
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
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
	std::lock_guard<std::mutex> lk(g_cacheMu);
	loadKeyIndexLocked();
	const auto* cached = g_keyIndex.find(depotId);
	return cached ? *cached : SavedKey{};
}

bool saveKeyToCache(uint32_t appId, uint32_t depotId, const std::string& key, bool managed)
{
	if (key.size() != 32)
	{
		g_pLog->debug("DepotKey: refusing to cache key for depot %u (size %zu, expected 32)\n",
		              depotId, key.size());
		return false;
	}

	const auto path = getKeyPath(depotId);
	std::lock_guard<std::mutex> lk(g_cacheMu);
	loadKeyIndexLocked();

	SavedKey existing;
	if (const auto* cached = g_keyIndex.find(depotId)) existing = *cached;

	uint32_t finalAppId = appId ? appId : existing.appId;
	// managed is sticky: a passive re-observation must never downgrade a
	// LuaTools depot out of manifest scope (depotkey_scope.hpp).
	const bool finalManaged = mergeManagedFlag(existing.managed, managed);
	if (existing.depotId == depotId && existing.key == key
	    && existing.appId == finalAppId && existing.managed == finalManaged)
	{
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

	// Replace the file whole, so a failed write never leaves a torn key over
	// the saved one. No fsync: a lost key is imported or observed again.
	const std::string temp = path + ".tmp." + std::to_string(getpid());
	{
		std::ofstream ofs(temp, std::ios::out | std::ios::trunc | std::ios::binary);
		ofs.write(node.c_str(), node.size());
		ofs.close();
		if (!ofs || std::rename(temp.c_str(), path.c_str()) != 0)
		{
			std::remove(temp.c_str());
			g_pLog->debug("DepotKey: cannot write %s\n", path.c_str());
			return false;
		}
	}

	g_pLog->infoOnce("DepotKey: cached key for app=%u depot=%u\n", finalAppId, depotId);

	SavedKey saved;
	saved.appId = finalAppId;
	saved.depotId = depotId;
	saved.key = key;
	saved.managed = finalManaged;
	g_managedDepotIndex.replace(
		depotId, existing.appId, existing.managed,
		saved.appId, saved.managed);
	g_keyIndex.upsert(depotId, std::move(saved));
	return true;
}


bool isManagedDepot(uint32_t depotId)
{
	const auto k = getCachedKey(depotId);
	return k.managed && k.key.size() == 32;
}

bool manifestInManagedScope(uint32_t appId, uint32_t depotId,
	                         bool depotHasPin, uint32_t knownContentAppId)
{
	const auto key = getCachedKey(depotId);
	uint32_t baseAppId = 0;
	if (appId && g_config.isAddedAppId(appId))
		baseAppId = appId;
	else if (key.appId && g_config.isAddedAppId(key.appId))
		baseAppId = key.appId;
	else if (depotId && g_config.isAddedAppId(depotId))
		baseAppId = depotId;

	uint32_t depotDlcAppId = knownContentAppId;
	if (!depotDlcAppId && baseAppId)
	{
		std::string wire;
		if (AppInfoProvision::readValidatedCacheBuffer(baseAppId, wire))
		{
			depotDlcAppId = AppInfoProvision::dlcAppIdForDepot(
				wire, baseAppId, depotId);
		}
	}
	const uint32_t contentAppId = manifestContentAppId(
		baseAppId, appId, key.appId, depotDlcAppId);
	const bool nativeLicense = contentAppId &&
		StatsPolicy::hasNativeLicense(contentAppId);
	if (nativeLicense && g_pLog)
	{
		g_pLog->infoOnce(
			"ManifestScope: app=%u content=%u depot=%u has a native license; using Steam's manifest path\n",
			baseAppId, contentAppId, depotId);
	}

	return depotInManifestScope(
		baseAppId != 0,
		depotId && g_config.isAddedAppId(depotId),
		key.managed && key.key.size() == 32,
		depotHasPin,
		nativeLicense);
}

std::vector<uint32_t> managedDepotsForApp(uint32_t appId)
{
	std::vector<uint32_t> out;
	if (appId == 0) return out;

	std::lock_guard<std::mutex> lk(g_cacheMu);
	loadKeyIndexLocked();
	// Only MANAGED (Lua-injected) depots: an observed owned-game / runtime
	// key must never be synthesized into an app's appinfo.
	return g_managedDepotIndex.forApp(appId);
}


namespace
{
bool luaImportReady(std::string& stplug)
{
	const auto steamRoot = findSteamRoot();
	if (steamRoot.empty()) return false;

	stplug = steamRoot + "/config/stplug-in";
	std::error_code ec;
	return std::filesystem::is_directory(stplug, ec);
}

void importLuaScriptsFrom(const std::string& stplug)
{
	static const std::regex addappidWithKeyRe(
		"addappid\\s*\\(\\s*(\\d+)\\s*,\\s*\\d+\\s*,\\s*\"([0-9A-Fa-f]{64})\"\\s*\\)"
	);

	const auto managedApps = g_config.managedAppIds.get();
	int imported = 0;
	std::error_code dirEc;
	for (std::filesystem::directory_iterator it(stplug, dirEc), end;
	     !dirEc && it != end; it.increment(dirEc))
	{
		const auto& entry = *it;
		std::error_code fileEc;
		if (!entry.is_regular_file(fileEc) || fileEc) continue;
		const auto& path = entry.path();
		if (path.extension() != ".lua") continue;

		const uint32_t appIdGuess = ConfigDiscovery::appIdFromScriptName(
			path.filename().string());
		if (appIdGuess == 0 || !managedApps.contains(appIdGuess)) continue;

		std::ifstream ifs(path);
		if (!ifs.is_open()) continue;

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
				// The regex admits any digit run; skip an id that does not fit.
				const std::string digits = (*it)[1].str();
				uint32_t depotId = 0;
				if (std::from_chars(digits.data(), digits.data() + digits.size(),
				                    depotId).ec != std::errc())
					continue;
				const std::string keyHex = (*it)[2].str();
				const std::string keyBin = hexToBytes(keyHex);
				if (keyBin.size() != 32) continue;
				if (saveKeyToCache(appIdGuess, depotId, keyBin, /*managed=*/true))
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
} // namespace

void importLuaScripts()
{
	std::string stplug;
	(void)g_importGate.run(
		[&]() { return luaImportReady(stplug); },
		[&]() { importLuaScriptsFrom(stplug); });
}

void reloadLuaScripts()
{
	std::string stplug;
	(void)g_importGate.rerun(
		[&]() { return luaImportReady(stplug); },
		[&]() { importLuaScriptsFrom(stplug); });
}


void provisionManifests()
{
	const auto steamRoot = findSteamRoot();
	if (steamRoot.empty()) return;

	const auto src = steamRoot + "/config/depotcache";
	const auto dst = steamRoot + "/depotcache";
	std::error_code dirEc;
	if (!std::filesystem::is_directory(src, dirEc)) return;
	std::filesystem::create_directories(dst, dirEc);
	if (dirEc) return;

	int copied = 0;
	for (std::filesystem::directory_iterator it(src, dirEc), end;
	     !dirEc && it != end; it.increment(dirEc))
	{
		const auto& entry = *it;
		std::error_code fileEc;
		if (!entry.is_regular_file(fileEc) || fileEc) continue;
		const auto& path = entry.path();
		if (path.extension() != ".manifest") continue;

		const auto target = std::filesystem::path(dst) / path.filename();
		std::error_code ec;
		bool needsCopy = !std::filesystem::exists(target, ec);
		if (!needsCopy)
		{
			auto srcSize = std::filesystem::file_size(path, ec);
			auto dstSize = std::filesystem::file_size(target, ec);
			needsCopy = (!ec && srcSize != dstSize);
		}
		// Only a complete manifest, and never a partly written copy of one.
		if (needsCopy && ManifestStoreIO::isValidManifest(path)
		    && ManifestStoreIO::atomicCopy(path, target))
		{
			++copied;
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
	std::error_code existsError;
	if (!std::filesystem::exists(path, existsError)) return;

	const auto configLockPath = AppInfoProvision::cacheLockPath();
	const auto lockParent = std::filesystem::path(configLockPath).parent_path();
	std::error_code lockDirError;
	std::filesystem::create_directories(lockParent, lockDirError);
	if (lockDirError && !std::filesystem::exists(lockParent, existsError))
	{
		g_pLog->debug("DepotKey: cannot create config.vdf lock directory: %s\n",
		              lockDirError.message().c_str());
		return;
	}
	ProcessLock::FileLock configLock(configLockPath, false);
	if (!configLock.acquired())
	{
		g_pLog->debug("DepotKey: config.vdf writer lock is busy\n");
		return;
	}

	AtomicFile::FileIdentity expected{};
	if (!AtomicFile::readIdentity(path, expected)) return;

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

	std::string writeError;
	if (!AtomicFile::writeIfUnchanged(path, expected, content, writeError))
	{
		g_pLog->debug(
		    "DepotKey: config.vdf changed before conditional publish: %s\n",
		    writeError.c_str());
		return;
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
}


bool recvDepotKey(CMsgClientGetDepotDecryptionKeyResponse* resp)
{
	if (!resp) return false;

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

	const uint32_t depotId = resp->depot_id();
	const bool eresultOk = resp->eresult() == k_EResultOK;
	const bool okKeyIs32 = resp->depot_encryption_key().size() == 32;

	// The cache lookup is only needed on the error path; defer it so a normal
	// OK response never pays for it.
	SavedKey cached;
	bool haveCachedKey = false;
	if (!eresultOk)
	{
		cached = getCachedKey(depotId);
		haveCachedKey = cached.depotId != 0 && cached.key.size() == 32;
	}
	const bool isAddedDepot = !eresultOk && g_config.isAddedAppId(depotId);

	const auto action = DepotKey::classifyRecv(
		eresultOk, okKeyIs32, haveCachedKey, isAddedDepot);

	g_pLog->debug("DepotKey: incoming response for depot=%u eresult=%u keysize=%zu (correlated app=%u)\n",
	              depotId, resp->eresult(),
	              resp->depot_encryption_key().size(), appId);

	switch (action)
	{
		case DepotKey::RecvAction::CacheObserved:
			// Observed from a legitimate Steam response — cache it for
			// possible substitution, but mark it unmanaged so it does NOT
			// drag an owned game / runtime into our manifest scope.
			saveKeyToCache(appId, depotId, resp->depot_encryption_key(), /*managed=*/false);
			return false;

		case DepotKey::RecvAction::SubstituteCached:
		{
			g_pLog->infoOnce("DepotKey: substituting cached key for depot %u (Steam said eresult=%u)\n",
			             depotId, resp->eresult());
			CMsgClientGetDepotDecryptionKeyResponse fresh;
			fresh.set_eresult(k_EResultOK);
			fresh.set_depot_id(depotId);
			fresh.set_depot_encryption_key(cached.key);
			resp->ParseFromString(fresh.SerializeAsString());
			return true;
		}

		case DepotKey::RecvAction::SynthZero:
		{
			const std::string zeroKey(32, '\0');
			g_pLog->infoOnce("DepotKey: synthesising zero key for AdditionalApps depot %u (Steam said eresult=%u)\n",
			             depotId, resp->eresult());
			CMsgClientGetDepotDecryptionKeyResponse fresh;
			fresh.set_eresult(k_EResultOK);
			fresh.set_depot_id(depotId);
			fresh.set_depot_encryption_key(zeroKey);
			resp->ParseFromString(fresh.SerializeAsString());
			return true;
		}

		case DepotKey::RecvAction::None:
		default:
			if (!eresultOk)
			{
				g_pLog->debug("DepotKey: no cached key for depot %u (Steam said eresult=%u)\n",
				              depotId, resp->eresult());
			}
			return false;
	}
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

void sendMsg(CNetPacket* packet)
{
	// Observe only: remember which app asked for the key so the response can
	// be cached against it. The outgoing frame is never rewritten.
	if (!packet || packet->getProtoBufType() != k_EMsgClientGetDepotDecryptionKey)
		return;
	auto req = packet->deserializeBody<CMsgClientGetDepotDecryptionKey>();
	sendDepotKey(&req);
}

} // namespace DepotKey
