// Validate the derived ReconcilePin call site against a 32-bit steamclient:
// the production locator must find EvaluateConfigChanges' call to the
// builder that CUserAppManager::BuildDepotDependency resolves to.
// Build with: g++ -std=c++20 -I src tools/test_reconcilepin_pattern.cpp -o /tmp/test_reconcilepin_pattern

#include "feats/reconcilepin.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

struct Byte
{
	uint8_t value;
	bool wildcard;
};

static std::vector<Byte> parse(const char* pattern)
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

static std::vector<size_t> findMatches(const std::string& image,
	                                    const std::vector<Byte>& pattern)
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

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: test_reconcilepin_pattern /path/to/steamclient.so\n";
		return 2;
	}
	std::ifstream input(argv[1], std::ios::binary);
	if (!input)
	{
		std::cerr << "could not open " << argv[1] << '\n';
		return 2;
	}
	const std::string image{std::istreambuf_iterator<char>(input),
	                        std::istreambuf_iterator<char>()};
	const auto evaluates = findMatches(image, parse(
	    "55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF "
	    "8B 45 10 89 85 40 FF FF FF 8B 45 08 8B 40 04"));
	if (evaluates.size() != 1)
	{
		std::cerr << "expected one EvaluateConfigChanges; found "
		          << evaluates.size() << '\n';
		return 1;
	}
	// Patterns::CUserAppManager::BuildDepotDependency (SigFollowMode::Relative).
	const auto builderSites = findMatches(image, parse(
	    "E8 ? ? ? ? 88 45 A3 83 C4 ? 84 C0 74 ?"));
	if (builderSites.size() != 1)
	{
		std::cerr << "expected one BuildDepotDependency site; found "
		          << builderSites.size() << '\n';
		return 1;
	}
	int32_t relative = 0;
	std::memcpy(&relative, image.data() + builderSites.front() + 1, sizeof(relative));
	const auto builder = static_cast<std::uintptr_t>(
	    static_cast<int64_t>(builderSites.front() + 5) + relative);

	// File offsets stand in for addresses: both sites lie in .text, so their
	// rel32 targets compare in the same space.
	const size_t entry = evaluates.front();
	const size_t len = std::min(image.size() - entry,
	    ReconcilePin::detail::kBuilderCallOff + ReconcilePin::detail::kBuilderCallWindow + 8);
	const auto inImage = [&](std::uintptr_t target) { return target < image.size(); };
	const long call = ReconcilePin::detail::findBuilderCall(
	    reinterpret_cast<const uint8_t*>(image.data()) + entry, len, entry,
	    builder, inImage);
	if (call < 0)
	{
		std::cerr << "no call to the builder near EvaluateConfigChanges+0x"
		          << std::hex << ReconcilePin::detail::kBuilderCallOff << '\n';
		return 1;
	}
	std::cout << "ReconcilePin target-vector call at EvaluateConfigChanges+0x"
	          << std::hex << call << " -> builder 0x" << builder << '\n';
	return 0;
}
