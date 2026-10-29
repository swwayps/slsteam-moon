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
//   CSteamApp::OwnershipFlagsReference    LibraryRemoval cannot derive the
//                                        ownership-flags field offset, so
//                                        visual library removal turns off.
//   CUser::SpawnGameId                   LaunchOptions turns off.
//   ParentalSignatureCheck               DisableParentalRestrictions turns off.
//
// Each signature is asserted to be present verbatim in src/patterns.cpp, so
// this test can never pass against a stale private copy of the pattern, and to
// resolve to exactly one match in the module it belongs to. Locators not yet
// ported to x86-64 must compile with an empty signature, which never matches,
// so their feature stays off instead of patching the wrong code.
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
// The locator is compiled with an empty signature: `"<name>", ""`.
void expectUnported(const std::string& source, const std::string& name)
{
	const std::string literal = "\"" + name + "\",";
	std::size_t at = source.find(literal);
	if (at != std::string::npos)
		at = source.find_first_not_of(" \t\r\n", at + literal.size());
	expect(at != std::string::npos && source.compare(at, 3, "\"\",") == 0,
	       name + ": not ported to x86-64 yet, so its signature must stay empty");
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

	// i386 signatures that have no x86-64 counterpart yet (see
	// .kiro/research/x86_64-port/STATE.md).
	expectUnported(source, "CUser::ProcessPendingLicenseUpdates");
	expectUnported(source, "CSteamApp::OwnershipFlagsReference");
	expectUnported(source, "ParentalSignatureCheck");
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
	(void)ui; // the steamui.so locators checked here are all unported
	// PackagePatch: LoadPackage's entry and the Grow call inside its AppIdVec
	// append. Without them added apps own no depots and Steam plans nothing.
	(void)resolveUnique(
		client, "CPackageInfoCache::LoadPackage",
		"41 57 41 56 49 89 F6 41 55 41 54 55 53 48 89 FB 48 81 EC ? ? 00 00 4C 8B 25 ? ? ? ? 89 54 24 ? 48 63 69 10",
		source);
	(void)resolveUnique(client, "CUtlMemory::Grow", "E8 ? ? ? ? 8B 43 50 8D 70 01 EB", source);
	if (failures != 0)
	{
		std::cerr << failures << " optional locator check(s) failed\n";
		return 1;
	}
	std::cout << "optional runtime-capability locators resolve uniquely\n";
	return 0;
}
