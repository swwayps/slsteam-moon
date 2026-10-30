// Validate the optional runtime-capability locators against the installed
// 64-bit Steam client.
//
// These are the compiled locators whose failure silently degrades a
// feature instead of failing the load, so a signature that stops matching after
// a client update is easy to miss:
//
//   CUser::ProcessPendingLicenseUpdates  PackagePatch loses the runtime license
//                                        refresh, so adding a game needs a
//                                        Steam restart again.
//   SteamUI RunFrame/GetAppByID/         LibraryRemoval turns off, so a removed
//   MarkAppChange/OwnershipFlagsReference game stays in the library until a
//                                        restart.
//   CAppInfoCache (4 locators)           AppInfoState turns off, so an added
//                                        game needs a restart again.
//   CUser::SpawnGameId                   LaunchOptions turns off.
//   ParentalSignatureCheck               DisableParentalRestrictions turns off.
//
// Each signature is asserted to be present verbatim in src/patterns.cpp, so
// this test can never pass against a stale private copy of the pattern, and to
// resolve to exactly one match in the module it belongs to.
//
// Build and run with:
//   make test-optional-locators
// or point it at another install:
//   make test-optional-locators STEAMCLIENT=... STEAMUI=...


#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

void expect(bool condition, std::string_view message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}

struct Byte
{
	std::uint8_t value;
	bool wildcard;
};

std::vector<Byte> parse(std::string_view pattern)
{
	std::vector<Byte> bytes;
	std::size_t cursor = 0;
	while (cursor < pattern.size())
	{
		while (cursor < pattern.size() && pattern[cursor] == ' ')
			++cursor;
		if (cursor >= pattern.size())
			break;
		const std::size_t end = pattern.find(' ', cursor);
		const std::string token(pattern.substr(
			cursor, end == std::string_view::npos ? end : end - cursor));
		bytes.push_back({
			static_cast<std::uint8_t>(
				token == "?" ? 0 : std::stoul(token, nullptr, 16)),
			token == "?",
		});
		cursor = end == std::string_view::npos ? pattern.size() : end;
	}
	return bytes;
}

std::vector<std::size_t> findMatches(
	const std::string& image, const std::vector<Byte>& pattern)
{
	std::vector<std::size_t> matches;
	if (pattern.empty() || image.size() < pattern.size())
		return matches;
	for (std::size_t offset = 0; offset + pattern.size() <= image.size(); ++offset)
	{
		bool match = true;
		for (std::size_t index = 0; index < pattern.size(); ++index)
		{
			if (!pattern[index].wildcard
			    && static_cast<std::uint8_t>(image[offset + index])
			           != pattern[index].value)
			{
				match = false;
				break;
			}
		}
		if (match)
			matches.push_back(offset);
	}
	return matches;
}

std::string readFile(const char* path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		std::cerr << "could not open " << path << '\n';
		std::exit(2);
	}
	return std::string{std::istreambuf_iterator<char>(input),
	                   std::istreambuf_iterator<char>()};
}

// Returns the single match offset, or npos after reporting the failure.
std::size_t resolveUnique(
	const std::string& image,
	std::string_view name,
	std::string_view signature,
	const std::string& source)
{
	expect(source.find(signature) != std::string::npos,
	       std::string(name) + ": signature is not the one compiled in "
	                           "src/patterns.cpp");
	const auto matches = findMatches(image, parse(signature));
	if (matches.size() == 1)
		return matches.front();
	std::cerr << "FAIL: " << name << ": expected exactly one match, found "
	          << matches.size() << '\n';
	++failures;
	return std::string::npos;
}

// MemHlp::findPrologue: walk up from `match` to the nearest byte run that
// reads `reversed` backwards (-1 = any byte). Returns the run's first byte.
std::size_t findPrologueAbove(const std::string& image, std::size_t match,
                              const std::vector<int>& reversed)
{
	for (std::size_t up = 0; up < 0x10000 && up + reversed.size() <= match + 1; ++up)
	{
		bool found = true;
		for (std::size_t j = 0; j < reversed.size() && found; ++j)
			found = reversed[j] < 0
			        || static_cast<std::uint8_t>(image[match - up - j]) == reversed[j];
		if (found)
			return match - up - reversed.size() + 1;
	}
	return std::string::npos;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 3 || argc > 4)
	{
		std::cerr << "usage: test_optional_locators <steamclient.so> "
		             "<steamui.so> [src/patterns.cpp]\n";
		return 2;
	}
	const std::string client = readFile(argv[1]);
	const std::string ui = readFile(argv[2]);
	const std::string source = readFile(argc == 4 ? argv[3] : "src/patterns.cpp");

	// DisableParentalRestrictions: the cached-settings signature branch and
	// the settings receiver.
	const std::size_t parental = resolveUnique(client, "ParentalSignatureCheck",
		"84 C0 75 ? 8B 93 ? ? 00 00 48 8D AC 24 ? ? 00 00 31 C0 48 8D 35 ? ? ? ? 48 89 EF E8", source);
	expect(parental == std::string::npos ||
	       static_cast<std::uint8_t>(client[parental + 2]) == 0x75,
	       "ParentalSignatureCheck: expected the jne the unlock turns into jmp");
	(void)resolveUnique(client, "ParentalSettingsReceived",
		"41 57 41 56 45 89 C6 41 55 49 89 F5 41 54 41 89 D4 55 48 89 FD 53 48 89 CB 48 81 EC ? ? 00 00 48 8D 15 ? ? ? ?", source);
	// Upstream dev-X86_64: the GameID decode near the top of the function.
	// PrologueUpwards then walks up to `push rbp; mov rbp, rsp`.
	const std::size_t spawn = resolveUnique(
		client, "CUser::SpawnGameId", "31 C0 41 80 78", source);
	expect(source.find("std::vector<int16_t> { 0xE5, 0x89, 0x48, 0x55 }") != std::string::npos,
	       "CUser::SpawnGameId: prologue is not the one compiled in src/patterns.cpp");
	if (spawn != std::string::npos)
	{
		const std::size_t start = findPrologueAbove(client, spawn, {0xE5, 0x89, 0x48, 0x55});
		expect(start != std::string::npos,
		       "CUser::SpawnGameId: no function prologue above the match");
		if (start != std::string::npos)
			std::cout << "CUser::SpawnGameId starts 0x" << std::hex << spawn - start
			          << std::dec << " bytes above the match\n";
	}
	// LibraryRemoval: the UI frame and change hooks, the app lookup and the
	// ownership read whose displacement gives the field offset.
	(void)resolveUnique(ui, "CSteamUIAppController::RunFrame",
		"41 57 41 56 41 55 41 54 55 53 48 83 EC 38 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 28 48 89 FB E8 ? ? ? ? 66 0F EF C9 66 0F EF D2 F2 0F 5A C0 F3 0F 5A D0 F3 0F 5A 8B ? ? 00 00", source);
	(void)resolveUnique(ui, "CSteamUIAppController::GetAppByID",
		"41 57 41 56 49 89 FE 41 55 41 54 55 53 48 83 EC 38 89 74 24 0C 89 54 24 08 64 48 8B 04 25 28 00 00 00 48 89 44 24 28 48 8B 05 ? ? ? ? 8B 00 85 C0 0F 85 ? ? ? ? 48 C7 44 24 10 00 00 00 00 85 F6 89 F5", source);
	(void)resolveUnique(ui, "CUpdateManager::MarkAppChange",
		"41 57 41 89 F7 41 56 41 55 41 54 55 48 89 FD 53 48 83 EC 28 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 18 89 D3 8B 10 85 DB 0F 94 C0 85 F6 0F 94 C1", source);
	const std::size_t ownership = resolveUnique(ui, "CSteamApp::OwnershipFlagsReference",
		"41 8B ? 1C 48 8B BD ? ? ? ? 48 8D 35 ? ? ? ? E8", source);
	expect(ownership == std::string::npos ||
	       static_cast<std::uint8_t>(ui[ownership + 2]) == 0x56,
	       "CSteamApp::OwnershipFlagsReference: expected mov edx,[r14+0x1c]");
	// ManifestCode: the request Steam sends when a manifest is not on disk.
	(void)resolveUnique(client, "CDepotDownloadMgr::BYldRequestDepotManifest",
		"41 57 48 8D 41 FF 41 89 D7 41 56 41 55 41 54 4D 89 C4 55 48 89 CD 53 48 89 FB 48 81 EC ? ? 00 00 48 83 F8 FD", source);
	// DepotQuarantine: both chunk-completion callbacks.
	(void)resolveUnique(client, "CDepotDownloadMgr::OnChunkUnpacked[cdecl]",
		"55 48 89 E5 41 57 41 56 49 89 D6 41 55 41 89 CD 41 54 4D 89 C4 53 48 89 FB 48 81 EC ? ? 00 00 48 8B 05 ? ? ? ? 48 89 B5 ? ? FF FF", source);
	(void)resolveUnique(client, "CDepotDownloadMgr::OnChunkUnpacked[regparm3]",
		"55 48 89 E5 41 57 41 56 49 89 F6 41 55 49 89 D5 41 54 41 89 CC 53 48 81 EC ? ? 00 00 48 8B 05 ? ? ? ? 48 89 BD ? ? FF FF 4C 89 85 ? ? FF FF", source);
	// AppInfoState: live appinfo reload entry, disk reader call site and the
	// two CAppData layout reads.
	(void)resolveUnique(client, "CAppInfoCache::GetOrAddAppData",
		"41 57 41 56 41 55 41 54 49 89 FC 55 89 F5 53 48 83 EC 18 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 08 89 D3", source);
	(void)resolveUnique(client, "CAppInfoCache::ThreadedReadFromDisk",
		"E8 ? ? ? ? E8 ? ? ? ? 48 8B 14 24 48 89 44 24 08", source);
	(void)resolveUnique(client, "CAppInfoCache::SkipFlagReference",
		"0F B6 40 ? 48 83 C3 01 20 44 24 ? 39 5D ?", source);
	(void)resolveUnique(client, "CAppInfoCache::ShaReference",
		"48 39 70 ? 75 ? 48 8B 7A 08 48 39 78 ? 75 ? 8B 7A 10 39 78 ?", source);
	// PackagePatch: LoadPackage's entry and the Grow call inside its AppIdVec
	// append. Without them added apps own no depots and Steam plans nothing.
	(void)resolveUnique(
		client, "CPackageInfoCache::LoadPackage",
		"41 57 41 56 49 89 F6 41 55 41 54 55 53 48 89 FB 48 81 EC ? ? 00 00 4C 8B 25 ? ? ? ? 89 54 24 ? 48 63 69 10",
		source);
	(void)resolveUnique(client, "CUtlMemory::Grow", "E8 ? ? ? ? 8B 43 50 8D 70 01 EB", source);
	// Runtime license refresh: mark package 0, process the pending set and
	// broadcast LicensesUpdated_t.
	(void)resolveUnique(client, "CUser::MarkLicenseAsChanged",
		"41 57 48 8D 87 ? ? 00 00 41 89 F7 41 56 41 55 41 54 55 53 48 83 EC 28 88 54 24 18 8B 97 ? ? 00 00 89 74 24 1C", source);
	(void)resolveUnique(client, "CUser::ProcessPendingLicenseUpdates",
		"41 54 49 89 FC 55 53 8B 9F ? ? 00 00 83 EB 01 78 ? 48 63 C3 48 8D 2C 80 48 C1 E5 04 EB ? 90 83 EB 01 48 83 ED 50 83 FB FF 74 ? 49 8B 84 24 ? ? 00 00 48 01 E8 83 78 14 50", source);
	(void)resolveUnique(client, "CUser::NotifyLicensesUpdated",
		"41 57 41 56 41 55 41 54 55 53 48 81 EC ? ? 00 00 44 8B 8F ? ? 00 00 48 89 7C 24 28 64 48 8B 04 25 28 00 00 00 48 89 84 24 ? ? 00 00 31 C0 45 85 C9 0F 8E", source);
	// ManifestBind: the depotcache check before a manifest request, and the
	// download step that calls it with the same arguments.
	(void)resolveUnique(
		client, "CDepotDownloadMgr::ProcessDepotManifest",
		"41 57 41 56 41 89 CE 41 55 41 54 49 89 FC 55 53 4C 89 C3 48 83 EC 38 48 89 34 24 89 54 24 0C 4C 89 4C 24 10",
		source);
	(void)resolveUnique(
		client, "CDepotDownloadMgr::PrepareDepotDownload",
		"41 57 41 56 41 55 41 54 55 53 48 89 FB 48 83 EC 78 48 89 74 24 08 89 4C 24 04 64 4C 8B 24 25 28 00 00 00 4C 89 64 24 68 4D 89 C4 E8",
		source);
	if (failures != 0)
	{
		std::cerr << failures << " optional locator check(s) failed\n";
		return 1;
	}
	std::cout << "optional runtime-capability locators resolve uniquely\n";
	return 0;
}
