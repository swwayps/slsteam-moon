// Validate the optional runtime-capability locators against the installed
// 32-bit Steam client.
//
// These are the compiled locators whose failure silently degrades a
// feature instead of failing the load, so a signature that stops matching after
// a client update is easy to miss:
//
//   CUser::ProcessPendingLicenseUpdates  PackagePatch loses the runtime license
//                                        refresh, so adding a game needs a
//                                        Steam restart again.
//   CCMInterface::RecvPkt                Family Share message filtering turns
//                                        off.
//   CSteamApp::OwnershipFlagsReference    LibraryRemoval cannot derive the
//                                        ownership-flags field offset, so
//                                        visual library removal turns off.
//
// Each signature is asserted to be present verbatim in src/patterns.cpp, so
// this test can never pass against a stale private copy of the pattern, and to
// resolve to exactly one match in the module it belongs to.
//
// Build and run with:
//   make test-optional-locators
// or point it at another install:
//   make test-optional-locators STEAMCLIENT=... STEAMUI=...

#include "feats/libraryremoval_policy.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
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

	// The trailing `lea eax,[ebx+disp32]` reaches a data symbol through the PIC
	// base, so its displacement moves whenever the GOT layout shifts (it went
	// 0x3B314 -> 0x3B714 between the 2026-08-03 and 2026-08-16 clients). The
	// hook only calls the resolved entry, so the displacement is location-only
	// and must stay wildcarded; the member offsets it loads (0x1C7C/0x1C70) are
	// read by the function itself and stay pinned as identity.
	(void)resolveUnique(
		client, "CUser::ProcessPendingLicenseUpdates",
		"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 44 24 40 8B 88 7C 1C "
		"00 00 85 C9 0F 8E ? ? ? ? 05 70 1C 00 00 89 44 24 18 8D 83 ? ? ? ? 8B 30",
		source);

	(void)resolveUnique(
		client, "CCMInterface::RecvPkt",
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC CC 04 00 00 8B 45 08 "
		"8B 7D 0C 89 85 50 FB FF FF 65 A1 14 00 00 00 89 45 E4 31 C0 8B 86",
		source);

	// FillInAppOverview serializes several fields with this very shape, one
	// after another, so the discriminator has to be the field offset. The
	// ownership read stays at 0x18 on both the 2026-08-03 and the 2026-08-16
	// client, while the block immediately after it reads 0x50. What moved is
	// only the register holding the app pointer (ecx -> edx), so the modrm byte
	// is the wildcard and the displacement stays pinned. Wildcarding the
	// displacement instead also matches exactly once on the newer client, but
	// it lands on that neighbouring field, and LibraryRemoval would then write
	// hidden ownership bits into it.
	const std::size_t ownership = resolveUnique(
		ui, "CSteamApp::OwnershipFlagsReference",
		"8B ? 18 8B 9D ? ? ? ? 83 EC 04 50 8D 83 ? ? ? ? 50 FF B5 ? ? ? ? "
		"E8 ? ? ? ?",
		source);
	if (ownership != std::string::npos)
	{
		const auto* bytes =
			reinterpret_cast<const std::uint8_t*>(ui.data()) + ownership;
		const auto offset = LibraryRemovalPolicy::deriveOwnershipOffset(
			std::span<const std::uint8_t>(bytes, 3));
		expect(offset.has_value(),
		       "CSteamApp::OwnershipFlagsReference: the matched instruction does "
		       "not decode to a usable ownership-flags offset");
		if (offset)
		{
			std::cout << "CSteamApp::OwnershipFlags offset = 0x" << std::hex
			          << *offset << std::dec << '\n';
		}
	}

	if (failures != 0)
	{
		std::cerr << failures << " optional locator check(s) failed\n";
		return 1;
	}
	std::cout << "optional runtime-capability locators resolve uniquely\n";
	return 0;
}
