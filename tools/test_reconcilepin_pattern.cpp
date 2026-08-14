// Validate the derived ReconcilePin call site against a 32-bit steamclient.
// Build with: g++ -std=c++20 tools/test_reconcilepin_pattern.cpp -o /tmp/test_reconcilepin_pattern

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
	const size_t call = evaluates.front() + 0x183;
	if (call + 5 > image.size()
	    || static_cast<uint8_t>(image[call]) != 0xe8)
	{
		std::cerr << "target-vector builder call assumption drifted\n";
		return 1;
	}
	int32_t relative = 0;
	std::memcpy(&relative, image.data() + call + 1, sizeof(relative));
	const int64_t destination = static_cast<int64_t>(call + 5) + relative;
	if (destination < 0 || static_cast<uint64_t>(destination) >= image.size())
	{
		std::cerr << "target-vector builder call resolves outside steamclient\n";
		return 1;
	}
	std::cout << "ReconcilePin target-vector call site is valid\n";
	return 0;
}
