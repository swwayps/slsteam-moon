// Regression test for the VFTableInfo_t registry: every entry must stay
// registered under the release toolchain (GCC 11, where std::vector is not
// constexpr), and init() must fail closed when the client was not parsed.
#include "config.hpp"
#include "decompiler.hpp"
#include "log.hpp"
#include "vftableinfo.hpp"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <string>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }
CConfig g_config;
CConfig::~CConfig() = default;

std::unordered_map<std::string, VFTable> Decompiler::vftables;
std::unordered_map<lm_address_t, std::string> Decompiler::strings;
std::map<std::string, unsigned int> Decompiler::parseInterfaceMapBase(const char*) { return {}; }
void Decompiler::parseFunction(const lm_address_t, std::unordered_map<lm_address_t, unsigned int>&) {}
unsigned int VFTable::analyze() { return 0; }

namespace
{
int failures = 0;
void check(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++failures;
}
// Entries are defined as an indented `VFTableInfo_t Name` line (commented-out
// entries start with `//`).
std::size_t definedEntries()
{
	std::ifstream source("src/vftableinfo.cpp");
	const std::regex definition(R"(^\s*VFTableInfo_t\s+\w+\s*$)");
	std::size_t count = 0;
	for (std::string line; std::getline(source, line);)
	{
		if (std::regex_match(line, definition)) ++count;
	}
	return count;
}
}

int main()
{
	const auto& all = VFTIndexes::functions();
	const std::size_t defined = definedEntries();
	std::printf("defined=%zu registered=%zu\n", defined, all.size());
	check(defined > 0 && all.size() == defined,
	      "every entry defined in src/vftableinfo.cpp is registered");
	check(std::set<const VFTableInfo_t*>(all.begin(), all.end()).size() == all.size(),
	      "each entry is registered once");
	check(!VFTIndexes::init(),
	      "init fails closed when no client vtable was parsed");
	// Only optional hooks use these four, so a missing one disables its
	// feature instead of the whole load.
	bool optionalOnly = true;
	for (VFTableInfo_t* entry : {&VFTIndexes::IClientCompat::BIsCompatLayerEnabled,
	                             &VFTIndexes::IClientConfigStore::SetString,
	                             &VFTIndexes::IClientFriends::GetFriendGamePlayed,
	                             &VFTIndexes::IClientRemoteStorage::IsCloudEnabledForApp})
		optionalOnly = optionalOnly && entry->init() && entry->address == 0;
	check(optionalOnly,
	      "an entry used only by optional hooks does not fail the load when missing");
	check(VFTIndexes::IClientUser::GetSteamID.typeName == "14IClientUserMap"
	      && VFTIndexes::IClientUser::GetSteamID.functionName == "GetSteamID",
	      "the SteamID slot is resolved by name from IClientUserMap");
	check(VFTIndexes::IClientUser::GetEncryptedAppTicket.typeName == "14IClientUserMap"
	      && VFTIndexes::IClientUser::GetEncryptedAppTicket.functionName
	         == "GetEncryptedAppTicket",
	      "the encrypted ticket slot is resolved by name from IClientUserMap");
	return failures == 0 ? 0 : 1;
}
