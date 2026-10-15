// Validate the manifest-pin builder locator against a 32-bit steamclient: the
// caller-site signature compiled in src/patterns.cpp matches once, and the call
// it follows reaches the builder Steam calls in flag pairs.
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

static bool hasPushLiteralBefore(const std::string& image, size_t call,
	                              uint8_t literal)
{
	const size_t begin = call > 16 ? call - 16 : 0;
	for (size_t i = begin; i + 1 < call; ++i)
		if (static_cast<uint8_t>(image[i]) == 0x6a
		    && static_cast<uint8_t>(image[i + 1]) == literal)
			return true;
	return false;
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

	// CDepotDownloadMgr::BuildDepotDependency, reached from its first caller.
	constexpr const char* kBuilderCaller =
	    "E8 ? ? ? ? 8B 46 08 8D 9F ? ? ? ? 83 C4 10 89 5D AC 8B 10 8B 52 4C 39 DA 0F 85";
	if (source.find(kBuilderCaller) == std::string::npos)
	{
		std::cerr << "the builder signature is not the one compiled in src/patterns.cpp\n";
		return 1;
	}
	const auto callers = findMatches(image, parse(kBuilderCaller));
	if (callers.size() != 1)
	{
		std::cerr << "expected one BuildDepotDependency caller; found "
		          << callers.size() << '\n';
		return 1;
	}
	const std::vector<size_t> builders{callDestination(image, callers.front())};

	size_t flag0 = 0;
	size_t flag1 = 0;
	for (size_t call = 0; call + 5 <= image.size(); ++call)
	{
		if (static_cast<uint8_t>(image[call]) != 0xe8
		    || callDestination(image, call) != builders.front())
			continue;
		flag0 += hasPushLiteralBefore(image, call, 0);
		flag1 += hasPushLiteralBefore(image, call, 1);
	}
	if (flag0 != 4 || flag1 != 4)
	{
		std::cerr << "expected four flag=0 and four flag=1 builder calls; found "
		          << flag0 << " and " << flag1 << '\n';
		return 1;
	}

	std::cout << "manifest-pin call sites match the expected flag pairs\n";
	return 0;
}
