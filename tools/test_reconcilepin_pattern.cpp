// Validate the ReconcilePin locators against a 64-bit steamclient: the
// EvaluateConfigChanges signature compiled in src/patterns.cpp matches once,
// the production call-site locator finds its call to the builder that
// Patterns::CUserAppManager::BuildDepotDependency resolves to, and the context
// depot vector decodes from the function's own per-entry loop.
// Build: make test-reconcilepin-pattern (STEAMCLIENT=<path> names the client)
#include "feats/reconcilepin.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
namespace
{
struct Byte
{
	uint8_t value;
	bool wildcard;
};
std::vector<Byte> parse(const char* pattern)
{
	std::vector<Byte> bytes;
	while (*pattern)
	{
		while (*pattern == ' ') ++pattern;
		if (!*pattern) break;
		const char* end = std::strchr(pattern, ' ');
		const std::string token(pattern, end ? end : pattern + std::strlen(pattern));
		bytes.push_back({static_cast<uint8_t>(
		                     token == "?" ? 0 : std::stoul(token, nullptr, 16)),
		                 token == "?"});
		pattern = end ? end : pattern + std::strlen(pattern);
	}
	return bytes;
}
std::vector<size_t> findMatches(const std::string& image, const std::vector<Byte>& pattern)
{
	std::vector<size_t> matches;
	for (size_t offset = 0; offset + pattern.size() <= image.size(); ++offset)
	{
		bool match = true;
		for (size_t i = 0; i < pattern.size(); ++i)
		{
			if (!pattern[i].wildcard
			    && static_cast<uint8_t>(image[offset + i]) != pattern[i].value)
			{
				match = false;
				break;
			}
		}
		if (match) matches.push_back(offset);
	}
	return matches;
}
std::string readFile(const char* path)
{
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
// MemHlp::findPrologue: walk up from `match` to the nearest byte run that
// reads `reversed` backwards. Returns the run's first byte.
size_t findPrologueAbove(const std::string& image, size_t match,
                         const std::vector<uint8_t>& reversed)
{
	for (size_t up = 0; up < 0x10000 && up + reversed.size() <= match + 1; ++up)
	{
		bool found = true;
		for (size_t j = 0; j < reversed.size() && found; ++j)
			found = static_cast<uint8_t>(image[match - up - j]) == reversed[j];
		if (found)
			return match - up - reversed.size() + 1;
	}
	return std::string::npos;
}
} // namespace
int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: test_reconcilepin_pattern /path/to/steamclient.so\n";
		return 2;
	}
	const std::string image = readFile(argv[1]);
	const std::string source = readFile("src/patterns.cpp");
	if (image.empty() || source.empty())
	{
		std::cerr << "could not read " << argv[1] << " or src/patterns.cpp\n";
		return 2;
	}
	constexpr const char* kEvaluate =
	    "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ? ? 00 00 89 95 ? ? FF FF "
	    "8B 57 08 48 89 B5 ? ? FF FF 64 48 8B 04 25 28 00 00 00 48 89 45 C8 31 C0 "
	    "41 89 D4 41 83 E4 01";
	if (source.find(kEvaluate) == std::string::npos
	    || source.find("\"4D 85 FF 88 44 24\"") == std::string::npos)
	{
		std::cerr << "a signature is not the one compiled in src/patterns.cpp\n";
		return 1;
	}
	const auto evaluates = findMatches(image, parse(kEvaluate));
	if (evaluates.size() != 1)
	{
		std::cerr << "expected one EvaluateConfigChanges; found " << evaluates.size() << '\n';
		return 1;
	}
	// Patterns::CUserAppManager::BuildDepotDependency (PrologueUpwards to
	// `push r15; mov r15,r9`).
	const auto builderSites = findMatches(image, parse("4D 85 FF 88 44 24"));
	if (builderSites.size() != 1)
	{
		std::cerr << "expected one CUserAppManager::BuildDepotDependency match; found "
		          << builderSites.size() << '\n';
		return 1;
	}
	const size_t builder =
	    findPrologueAbove(image, builderSites.front(), {0xCF, 0x89, 0x4D, 0x57, 0x41});
	if (builder == std::string::npos)
	{
		std::cerr << "no builder prologue above the match\n";
		return 1;
	}
	// File offsets stand in for addresses: both sites lie in .text, so their
	// rel32 targets compare in the same space.
	const size_t entry = evaluates.front();
	const auto* code = reinterpret_cast<const uint8_t*>(image.data()) + entry;
	const size_t callLen = std::min(image.size() - entry,
	    ReconcilePin::detail::kBuilderCallOff + ReconcilePin::detail::kBuilderCallWindow + 8);
	const long call = ReconcilePin::detail::findBuilderCall(code, callLen, entry, builder);
	if (call != static_cast<long>(ReconcilePin::detail::kBuilderCallOff))
	{
		std::cerr << "expected the builder call at EvaluateConfigChanges+0x" << std::hex
		          << ReconcilePin::detail::kBuilderCallOff << "; locator returned " << std::dec
		          << call << '\n';
		return 1;
	}
	const long vec = ReconcilePin::detail::findCtxDepotVectorOffset(
	    code, std::min(image.size() - entry, ReconcilePin::detail::kCtxDepotLoopScanLen));
	if (vec != 0xc0)
	{
		std::cerr << "expected the context depot vector at +0xc0; decoded " << vec << '\n';
		return 1;
	}
	std::cout << "ReconcilePin: builder call at EvaluateConfigChanges+0x" << std::hex << call
	          << ", context depot vector at +0x" << vec << '\n';
	return 0;
}
