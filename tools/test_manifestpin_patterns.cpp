// Validate the manifest-pin builder locator against a 64-bit steamclient: the
// entry signature compiled in src/patterns.cpp matches once, and Steam calls
// that function in four flag pairs (esi = 0, then esi = 1).
// Run: make test-manifestpin-patterns [STEAMCLIENT=...]
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
static size_t callDestination(const std::string& image, size_t call)
{
	int32_t relative = 0;
	std::memcpy(&relative, image.data() + call + 1, sizeof(relative));
	return static_cast<size_t>(static_cast<int64_t>(call + 5) + relative);
}
// The flag the nearest esi write before `call` loads: `xor esi,esi` (31 f6)
// or `mov esi,1` (be 01 00 00 00); -1 when neither is within 64 bytes.
static int flagBefore(const std::string& image, size_t call)
{
	const size_t begin = call > 64 ? call - 64 : 0;
	for (size_t i = call; i-- > begin;)
	{
		const auto* at = reinterpret_cast<const uint8_t*>(image.data()) + i;
		if (i + 5 <= call && at[0] == 0xbe && at[1] == 0x01 && at[2] == 0 &&
		    at[3] == 0 && at[4] == 0)
			return 1;
		if (i + 2 <= call && at[0] == 0x31 && at[1] == 0xf6)
			return 0;
	}
	return -1;
}
int main(int argc, char** argv)
{
	if (argc != 3)
	{
		std::cerr << "usage: test_manifestpin_patterns <steamclient.so> <src/patterns.cpp>\n";
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
	std::ifstream sourceInput(argv[2], std::ios::binary);
	const std::string source{std::istreambuf_iterator<char>(sourceInput),
	                         std::istreambuf_iterator<char>()};
	// CDepotDownloadMgr::BuildDepotDependency entry.
	constexpr const char* kBuilder =
	    "41 57 41 56 41 55 41 54 41 89 F4 55 48 89 FD 53 48 81 EC ? ? 00 00 "
	    "64 4C 8B 2C 25 28 00 00 00 4C 89 AC 24 ? ? 00 00 49 89 CD C6 47 ? 01 "
	    "44 8B 72 10";
	if (source.find(kBuilder) == std::string::npos)
	{
		std::cerr << "the builder signature is not the one compiled in src/patterns.cpp\n";
		return 1;
	}
	const auto builders = findMatches(image, parse(kBuilder));
	if (builders.size() != 1)
	{
		std::cerr << "expected one BuildDepotDependency match; found "
		          << builders.size() << '\n';
		return 1;
	}
	size_t flag0 = 0;
	size_t flag1 = 0;
	for (size_t call = 0; call + 5 <= image.size(); ++call)
	{
		if (static_cast<uint8_t>(image[call]) != 0xe8
		    || callDestination(image, call) != builders.front())
			continue;
		const int flag = flagBefore(image, call);
		flag0 += flag == 0;
		flag1 += flag == 1;
	}
	if (flag0 != 4 || flag1 != 4)
	{
		std::cerr << "expected four flag=0 and four flag=1 builder calls; found "
		          << flag0 << " and " << flag1 << '\n';
		return 1;
	}
	std::cout << "manifest-pin builder calls match the expected flag pairs\n";
	return 0;
}
