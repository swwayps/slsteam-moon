// SHA-1 against the FIPS 180-4 example vectors, at every length around the
// padding boundary.
// Build: g++ -std=c++20 -Wall -Wextra -Wpedantic -I src tools/test_sha1.cpp -o /tmp/test_sha1 && /tmp/test_sha1
#include "sha1.hpp"

#include <cstdio>
#include <string>

namespace
{
int failures = 0;

std::string hex(const std::string& input)
{
	std::uint8_t digest[20];
	Sha1::digest(input.data(), input.size(), digest);
	static const char* digits = "0123456789abcdef";
	std::string out;
	for (const std::uint8_t byte : digest)
	{
		out += digits[byte >> 4];
		out += digits[byte & 15];
	}
	return out;
}

void check(const std::string& input, const char* expected, const char* label)
{
	const std::string got = hex(input);
	const bool ok = got == expected;
	std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
	if (!ok)
	{
		std::printf("  got %s\n", got.c_str());
		++failures;
	}
}
} // namespace

int main()
{
	check("", "da39a3ee5e6b4b0d3255bfef95601890afd80709", "empty input");
	check("abc", "a9993e364706816aba3e25717850c26c9cd0d89d", "abc");
	check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	      "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "56 bytes: the length moves to a second block");
	check("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
	      "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
	      "a49b2446a02c645bf419f995b67091253a04a259", "112 bytes");
	check(std::string(1000000, 'a'), "34aa973cd4c4daa4f61eeb2bdbad27316534016f", "one million a");
	check(std::string(64, 'a'), "0098ba824b5c16427bd7a1122a5a442a25ec644d", "one whole block");
	check(std::string(55, 'a'), "c1c8bbdc22796e28c0e15163d20899b65621d65a", "55 bytes: one block");
	if (failures == 0) std::printf("\nall sha1 checks passed\n");
	return failures == 0 ? 0 : 1;
}
