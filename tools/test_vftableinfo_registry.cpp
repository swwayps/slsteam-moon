// Regression test for the VFTableInfo_t registry: every entry must stay
// registered under the release toolchain (GCC 11, where std::vector is not
// constexpr), and init() must fail closed when the client was not parsed.
#include "decompiler.hpp"
#include "log.hpp"
#include "vftableinfo.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

std::unique_ptr<CLog> g_pLog = std::make_unique<CLog>("");
CLog::CLog(const char*) {}
CLog::~CLog() = default;
LogLevel CLog::getMinLevel() { return LogLevel::Warn; }

std::unordered_map<std::string, VFTable> Decompiler::vftables;
std::map<std::string, unsigned int> Decompiler::parseInterfaceMapBase(const char*) { return {}; }
unsigned int VFTable::analzye() { return 0; }

namespace
{
int failures = 0;
void check(bool condition, const char* message)
{
	std::printf("%s %s\n", condition ? "ok:  " : "FAIL:", message);
	if (!condition) ++failures;
}
bool registered(const VFTableInfo_t& entry)
{
	const auto& all = VFTIndexes::functions();
	return std::find(all.begin(), all.end(), &entry) != all.end();
}
}

int main()
{
	const auto& all = VFTIndexes::functions();
	check(registered(VFTIndexes::CClientUnifiedServiceTransport::SendAndRecvMsg),
	      "the first entry defined in the registry is registered");
	check(registered(VFTIndexes::IClientUser::RequiresLegacyCDKey),
	      "the last entry defined in the registry is registered");
	check(std::set<const VFTableInfo_t*>(all.begin(), all.end()).size() == all.size(),
	      "each entry is registered once");
	check(!VFTIndexes::init(),
	      "init fails closed when no client vtable was parsed");
	return failures == 0 ? 0 : 1;
}
