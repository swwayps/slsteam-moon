#include "lua.hpp"

#include "sdk/sdk.hpp"
#include "sdk/CNetPacket.hpp"

#include "api.hpp"
#include "config.hpp"
#include "curl.hpp"
#include "filewatcher.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "memhlp.hpp"
#include "vftableinfo.hpp"

#include "libmem/libmem.h"
#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include "LuaBridge/Array.h"
#include "LuaBridge/List.h"
#include "LuaBridge/UnorderedSet.h"
#include "LuaBridge/Vector.h"


extern void* place_lua_hook(const int index, const void* pTarget)
{
	if (!LuaHook::hooks.contains(index))
	{
		return nullptr;
	}

	LuaHook* hook = LuaHook::hooks.at(index);
	hook->hookFn = reinterpret_cast<lm_address_t>(pTarget);

	return hook->place();
}

extern void* unpack_user_data(const void* pData)
{
	return reinterpret_cast<const luabridge::detail::Userdata*>(pData)->getPointer();
}

namespace LuaConfig
{
	CConfig* get()
	{
		return &g_config;
	}

	std::unordered_set<AppId_t> getAdditionalApps(CConfig* config)
	{
		return config->addedAppIds.copy();
	}

	double getDouble(CConfig* config, const char* name, const double defaultValue)
	{
		return config->getSetting<double>(config->rootNode, name, defaultValue);
	}

	int64_t getInt(CConfig* config, const char* name, const uint64_t defaultValue)
	{
		return config->getSetting<int64_t>(config->rootNode, name, defaultValue);
	}

	std::string getString(CConfig* config, const char* name, const std::string& defaultValue)
	{
		return config->getSetting<std::string>(config->rootNode, name, defaultValue);
	}

	std::unordered_set<double> getDoubleList(CConfig* config, const char* name)
	{
		return config->getList<double>(config->rootNode, name);
	}

	std::unordered_set<int64_t> getIntList(CConfig* config, const char* name)
	{
		return config->getList<int64_t>(config->rootNode, name);
	}

	std::unordered_set<std::string> getStringList(CConfig* config, const char* name)
	{
		return config->getList<std::string>(config->rootNode, name);
	}

	YAML::Node getNode(CConfig* config, const std::string& name)
	{
		return config->rootNode[name];
	}

	void setNode(CConfig* config, const std::string& name, const YAML::Node& node)
	{
		config->rootNode[name] = node;
	}
}

namespace LuaCurl
{
	std::string downloadStringWithHeaders(const char* url, const std::vector<std::string>& headers, const int timeOut)
	{
		std::string out;
		int res = Curl::downloadString(url, headers, out, timeOut);
		if (res != 0)
		{
			return "";
		}

		return out;
	}

	std::string downloadString(const char* url, const int timeOut)
	{
		return downloadStringWithHeaders(url, { }, timeOut);
	}
}

namespace LuaLog
{
	void debug(const char* msg)
	{
		LOG_DEBUG("%s\n", msg);
	}

	void warn(const char* msg)
	{
		LOG_WARN("%s\n", msg);
	}

	void error(const char* msg)
	{
		LOG_ERROR("%s\n", msg);
	}

	void info(const char* msg)
	{
		LOG_INFO("%s\n", msg);
	}

	void notify(const char* msg)
	{
		LOG_NOTIFY("%s", msg);
	}

	void notifyWarn(const char* msg)
	{
		LOG_NOTIFYWARN("%s", msg);
	}

	void notifyError(const char* msg)
	{
		LOG_NOTIFYERROR("%s", msg);
	}

	void custom(const LogLevelFlags_t lvl, const char* msg)
	{
		LOG_CUSTOM(lvl, "%s\n", msg);
	}
}

class LuaMutex
{
	std::recursive_mutex* mutex;

public:
	LuaMutex() : mutex(&Lua::stateMutex)
	{
		lock();
	}

	~LuaMutex()
	{
		unlock();
	}

	void lock()
	{
		mutex->lock();
	}

	void unlock()
	{
		mutex->unlock();
	}
};

namespace LuaSDK
{
	CSteamEngine* getEngine()
	{
		return g_pSteamEngine;
	}

	std::string getAppData(IClientApps* apps, const AppId_t appId, const char* name)
	{
		char buf[4096] { };
		const int32_t size = apps->getAppData(appId, name, buf, sizeof(buf));
		if (size <= 0)
		{
			return "";
		}

		return std::string(buf, std::min<size_t>(static_cast<size_t>(size), sizeof(buf)));
	}

	void postCallback(CUser* pUser, const uint32_t type, const lm_address_t pCallback, const uint32_t callbackSize)
	{
		pUser->postCallback(static_cast<ECallbackType>(type), reinterpret_cast<void*>(pCallback), callbackSize);
	}
}

namespace LuaYAML
{
	double asDouble(const YAML::Node* node)
	{
		return node->as<double>();
	}

	int64_t asInt(const YAML::Node* node)
	{
		return node->as<int64_t>();
	}

	std::string asString(const YAML::Node* node)
	{
		return node->as<std::string>();
	}

	std::vector<std::pair<YAML::Node, YAML::Node>> asPairList(const YAML::Node* node)
	{
		auto vec = std::vector<std::pair<YAML::Node, YAML::Node>>();
		for (const auto& it : *node)
		{
			vec.emplace_back(it.first, it.second);
		}

		return vec;
	}

	bool addItem(YAML::Node* node, const YAML::Node& nitm)
	{
		if (!node->IsSequence())
		{
			return false;
		}

		node->push_back(nitm);
		return true;
	}

	bool addPair(YAML::Node* node, const YAML::Node& first, const YAML::Node& second)
	{
		if (!node->IsMap())
		{
			return false;
		}

		(*node)[first] = second;
		return true;
	}

	void setDouble(YAML::Node* node, const double val)
	{
		*node = val;
	}

	void setInt(YAML::Node* node, const int64_t val)
	{
		*node = val;
	}

	void setString(YAML::Node* node, const std::string& val)
	{
		*node = val;
	}
}

std::atomic<bool> Lua::active {false};
lua_State* Lua::state = nullptr;
std::recursive_mutex Lua::stateMutex;
CFileWatcher* Lua::watcher = nullptr;
std::unordered_map<std::string, std::vector<luabridge::LuaRef>> Lua::callbacks = std::unordered_map<std::string, std::vector<luabridge::LuaRef>>();

namespace
{
	std::filesystem::path pluginDir()
	{
		return std::filesystem::path(g_config.getDir()) / "plugins";
	}

	// Collects the directory's .lua files in name order (directory_iterator
	// is unsorted) and runs them. Called with stateMutex held.
	void runPlugins(const std::filesystem::path& dir)
	{
		auto files = std::set<std::filesystem::path>();
		std::error_code ec;
		for (const auto& file : std::filesystem::directory_iterator { dir, ec })
		{
			const auto path = std::filesystem::path(file);
			if (path.extension() != ".lua")
			{
				continue;
			}

			if (!Lua::fixPerms(path))
			{
				continue;
			}

			files.emplace(path);
		}

		//There is no API in linux to freeze single threads, so we just wing it
		for (const auto& lua : files)
		{
			Lua::runLua(lua);
		}
	}

	// Runs on the reloading thread, inside CConfig::loadSettings().
	void onConfigLoading(YAML::Node& node)
	{
		std::lock_guard guard(Lua::stateMutex);
		try
		{
			// Plugins see and may change this load's tree through config:getNode
			// and setNode; the settings are then read from a private copy, so a
			// later plugin edit cannot race the parse. reset() rebinds a handle,
			// assignment would rewrite the tree it points to.
			g_config.rootNode.reset(node);
			Lua::fireCallback(Lua::Callbacks::SLSsteam_ConfigLoading);
			node.reset(YAML::Clone(node));
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Lua configLoading failed: %s\n", e.what());
		}
	}

	void onConfigLoaded()
	{
		Lua::fireCallback(Lua::Callbacks::SLSsteam_ConfigLoaded);
	}
}

void Lua::init(const bool fullReload)
{
	if (!g_config.plugins.get())
	{
		LOG_DEBUG("Lua plugins disabled\n");
		return;
	}

	// load() catches exceptions by tearing every hook down; a plugin runtime
	// failure must only cost the plugins.
	try
	{
		const bool firstStart = !active.load();
		const auto dir = pluginDir();
		{
			std::lock_guard guard(stateMutex);

			std::error_code ec;
			if (!std::filesystem::exists(dir, ec) && !std::filesystem::create_directory(dir, ec))
			{
				LOG_NOTIFYERROR("Failed to create plugins directory!\nPlugins will be unavailable\n");
				return;
			}

			if (!fixPerms(dir))
			{
				LOG_NOTIFYERROR("Failed to set appropiate permissions on the plugins directory!\nPlugins are disabled for this session");
				return;
			}

			if (firstStart)
			{
				// Plugins may read the config at file scope, before the reload
				// below hands them its tree.
				g_config.rootNode.reset(g_config.readTree());
			}

			if (fullReload || !state)
			{
				initLuaState();
			}
			else
			{
				// Same state (native hooks may still call into it), fresh callbacks.
				callbacks.clear();
			}

			active = true;
			runPlugins(dir);
		}

		if (firstStart)
		{
			g_config.pluginConfigLoading = &onConfigLoading;
			g_config.pluginConfigLoaded = &onConfigLoaded;
			SLSAPI::reloadPlugins = &Lua::reload;
		}

		// Upstream reloads the config after every plugin run: configLoading
		// lets plugins change it, and whatever an earlier run changed is reset.
		g_config.reload();

		// Before the interface hooks are placed, placeVFTHooks() fires it.
		if (Hooks::IClientUtils_GetOfflineMode.hooked)
		{
			fireCallback(Callbacks::SLSsteam_Initialized);
		}

		if (!watcher)
		{
			watcher = new CFileWatcher(onFileChange);
			if (!watcher->addFile(dir.c_str()) || !watcher->start())
			{
				LOG_NOTIFYERROR("Failed to watch plugin directory!\nHot reload will be unavailable");
			}
		}

		LOG_DEBUG("Lua initialized\n");
	}
	catch (const std::exception& e)
	{
		LOG_ERROR("Lua plugins failed to start: %s\n", e.what());
	}
	catch (...)
	{
		LOG_ERROR("Lua plugins failed to start\n");
	}
}

void Lua::reload()
{
	if (!active.load())
	{
		return;
	}

	Lua::fireCallback(Lua::Callbacks::SLSsteam_LuaReload);
	Lua::init();
}

void Lua::initLuaState()
{
	callbacks.clear();

	lua_State* newState = luaL_newstate();
	luaL_openlibs(newState);

	luabridge::getGlobalNamespace(newState)

	.beginNamespace("curl")
		.addFunction("downloadString", &LuaCurl::downloadString, &LuaCurl::downloadStringWithHeaders)
	.endNamespace()

	.beginNamespace("log")
		.addVariable("LogLevelTrace", ELogLevel::k_ELogLevelTrace)
		.addVariable("LogLevelOnce", ELogLevel::k_ELogLevelOnce)
		.addVariable("LogLevelDebug", ELogLevel::k_ELogLevelDebug)
		.addVariable("LogLevelWarn", ELogLevel::k_ELogLevelWarn)
		.addVariable("LogLevelError", ELogLevel::k_ELogLevelError)
		.addVariable("LogLevelInfo", ELogLevel::k_ELogLevelInfo)
		.addVariable("LogLevelNotify", ELogLevel::k_ELogLevelNotifyShort)
		.addVariable("LogLevelNotifyLong", ELogLevel::k_ELogLevelNotifyLong)
		.addFunction("debug", &LuaLog::debug)
		.addFunction("warn", &LuaLog::warn)
		.addFunction("error", &LuaLog::error)
		.addFunction("info", &LuaLog::info)
		.addFunction("notify", &LuaLog::notify)
		.addFunction("notifyWarn", &LuaLog::notifyWarn)
		.addFunction("notifyError", &LuaLog::notifyError)
		.addFunction("custom", &LuaLog::custom)
	.endNamespace()

	.beginClass<lm_module_t>("lm_module_t")
		.addProperty("base", &lm_module_t::base)
		.addProperty("end", &lm_module_t::end)
		.addProperty("size", &lm_module_t::size)
	.endClass()

	.beginNamespace("memhlp")
		.addFunction("getModule", &MemHlp::getModule)
		.addFunction("getJmpTarget", [](const Ptr_t ptr) { return reinterpret_cast<Ptr_t>(MemHlp::getJmpTarget(reinterpret_cast<lm_address_t>(ptr))); })
		.addFunction("hexdump", &MemHlp::hexdump)
		.addFunction("findPrologue", [](const Ptr_t ptr, const std::vector<int16_t>& prologue)
		{
			// The local search stops at the start of the containing segment.
			const auto address = reinterpret_cast<lm_address_t>(ptr);
			lm_segment_t segment {};
			const lm_address_t lowerBound = LM_FindSegment(address, &segment) ? segment.base : address;
			return reinterpret_cast<Ptr_t>(MemHlp::findPrologue(address, lowerBound, prologue.data(), prologue.size()));
		})
		.addFunction("patternScan", [](const char* pattern, const lm_module_t& mod) { return reinterpret_cast<Ptr_t>(MemHlp::patternScan(pattern, mod)); })
	.endNamespace()

	.beginClass<VFTableInfo_t>("VFTableInfo_t")
		.addConstructor<void(*)(const char*, const char*), void(*)(const char*, const char*, unsigned int), void(*)(const char*, const char*, unsigned int, unsigned int)>()
		.addProperty("typeName", &VFTableInfo_t::typeName)
		.addProperty("functionName", &VFTableInfo_t::functionName)
		.addProperty("address", &VFTableInfo_t::address)
		.addProperty("ptr", [](const VFTableInfo_t& info) { return reinterpret_cast<Ptr_t>(info.address); })
		.addProperty("index", &VFTableInfo_t::index)
		.addFunction("init", &VFTableInfo_t::init)
		.addFunction("getPrintName", &VFTableInfo_t::getPrintName)
	.endClass()

	.beginClass<LuaMutex>("LuaMutex")
		.addConstructor<void(*)()>()
		.addFunction("lock", &LuaMutex::lock)
		.addFunction("unlock", &LuaMutex::unlock)
	.endClass()

	.beginClass<LuaHook>("LuaHook")
		.addConstructor<void(const char*, const Ptr_t)>()
		.addProperty("name", &LuaHook::name)
		.addProperty("fn", [](const LuaHook* hook) { return reinterpret_cast<Lua::Ptr_t>(hook->fn); })
		.addProperty("hookFn", [](const LuaHook* hook) { return reinterpret_cast<Lua::Ptr_t>(hook->hookFn); })
		.addProperty("tramp", [](const LuaHook* hook) { return reinterpret_cast<Lua::Ptr_t>(hook->tramp); })
		.addProperty("size", &LuaHook::size)
		.addProperty("index", &LuaHook::idx)

		.addFunction("place", &LuaHook::place)
		.addFunction("remove", &LuaHook::remove)
	.endClass()

	.beginClass<YAML::Node>("YAMLNode")
		.addConstructor<void(*)()>()

		.addProperty("isDefined", &YAML::Node::IsDefined)
		.addProperty("isNull", &YAML::Node::IsNull)
		.addProperty("isScalar", &YAML::Node::IsScalar)
		.addProperty("isSequence", &YAML::Node::IsSequence)
		.addProperty("isMap", &YAML::Node::IsMap)
		.addProperty("size", &YAML::Node::size)

		.addFunction("asDouble", &LuaYAML::asDouble)
		.addFunction("asInt", &LuaYAML::asInt)
		.addFunction("asString", &LuaYAML::asString)
		.addFunction("asPairList", &LuaYAML::asPairList)

		.addFunction("addItem", &LuaYAML::addItem)
		.addFunction("addPair", &LuaYAML::addPair)

		.addFunction("setDouble", &LuaYAML::setDouble)
		.addFunction("setInt", &LuaYAML::setInt)
		.addFunction("setString", &LuaYAML::setString)
	.endClass()

	.beginClass<CConfig>("CConfig")
		.addFunction("getAdditionalApps", &LuaConfig::getAdditionalApps)
		.addFunction("setAdditionalApps", &CConfig::setAdditionalApps)

		.addFunction("getDouble", &LuaConfig::getDouble)
		.addFunction("getInt", &LuaConfig::getInt)
		.addFunction("getString", &LuaConfig::getString)
		.addFunction("getIntList", &LuaConfig::getIntList)
		.addFunction("getDoubleList", &LuaConfig::getDoubleList)
		.addFunction("getStringList", &LuaConfig::getStringList)

		.addFunction("getNode", &LuaConfig::getNode)
		.addFunction("setNode", &LuaConfig::setNode)
	.endClass()

	.beginClass<CNetPacketBody>("CNetPacketBody")
		.addProperty("type", &CNetPacketBody::type)
		.addProperty("headerSize", &CNetPacketBody::headerSize)
	.endClass()

	// The local CNetPacket settles its field layout at runtime, so the fields
	// are read through its accessors; the reference count is not exposed.
	.beginClass<CNetPacket>("CNetPacket")
		.addProperty("body", &CNetPacket::body)
		.addProperty("size", &CNetPacket::sizeBytes)
	.endClass()

	.beginClass<CSteamEngine>("CSteamEngine")
		.addFunction("getUser", &CSteamEngine::getUser)
		.addFunction("getUtils", &CSteamEngine::getUtils)
	.endClass()

	.beginClass<CUser>("CUser")
		.addFunction("getClientApps", &CUser::getClientApps)
		.addFunction("getClientUser", &CUser::getClientUser)
		.addFunction("getAppManager", &CUser::getAppManager)
		.addFunction("isSubscribed", &CUser::isSubscribed)
		.addFunction("postCallback", &LuaSDK::postCallback)
	.endClass()

	.beginClass<IClientApps>("IClientApps")
		.addFunction("getAppData", &LuaSDK::getAppData)
		.addFunction("getAppType", &IClientApps::getAppType)
	.endClass()

	.beginClass<IClientUser>("IClientUser")
		.addFunction("loggedOn", &IClientUser::loggedOn)
	.endClass()

	.beginClass<IClientUtils>("IClientUtils")
		.addFunction("getAppId", &IClientUtils::getAppId)
		.addFunction("getCurrentSteamPipe", &IClientUtils::getCurrentSteamPipe)
	.endClass()

	.beginNamespace("SLS")
		.addProperty("config", &LuaConfig::get)
		.addProperty("steamEngine", &LuaSDK::getEngine)
		.addFunction("registerCallback", &Lua::registerCallback)
	.endNamespace();

	lua_State* old = state;
	state = newState;

	if (old)
	{
		lua_close(old);
	}
}

bool Lua::fixPerms(const std::filesystem::path& path)
{
	std::error_code ec;
	const auto perms = std::filesystem::status(path, ec).permissions();
	if (ec)
	{
		LOG_ERROR("Failed to read permissions of %s!\n", path.filename().c_str());
		return false;
	}

	if ((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) != std::filesystem::perms::none)
	{
		std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
		if (ec)
		{
			LOG_ERROR("Failed to set permissions on %s!\n", path.filename().c_str());
			return false;
		}

		LOG_DEBUG("Fixed permissions for %s!\n", path.filename().c_str());
	}

	return true;
}

void Lua::onFileChange()
{
	// The local watcher reports a batch, not a file: any change in the plugin
	// directory reloads.
	Lua::reload();
}

bool Lua::runLua(const std::filesystem::path& path)
{
	//We disable the plugins from ever getting ran, the rest of the system
	//stays active to allow for hot reloading
	if (!g_config.plugins.get())
	{
		return false;
	}

	if (luaL_dofile(state, path.c_str()) != LUA_OK)
	{
		LOG_ERROR("Failed to run %s!\n%s\n", path.filename().c_str(), lua_tostring(state, -1));
		return false;
	}

	LOG_CUSTOM(k_ELogLevelInfo | k_ELogLevelOnce, "Ran %s\n", path.filename().c_str());
	return true;
}

void Lua::registerCallback(const std::string& name, luabridge::LuaRef fn)
{
	callbacks[name].emplace_back(fn);
	LOG_DEBUG("Registered lua callback for %s\n", name.c_str());
}
