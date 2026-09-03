#include "ascii.hpp"

#include <cstdio>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (condition) return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++failures;
}
}

int main()
{
	for (unsigned int value = 0; value <= 0xff; ++value)
	{
		const auto ch = static_cast<unsigned char>(value);
		const bool whitespace = ch == ' ' || ch == '\t' || ch == '\n' ||
			ch == '\v' || ch == '\f' || ch == '\r';
		const bool digit = ch >= '0' && ch <= '9';
		const bool alpha = (ch >= 'A' && ch <= 'Z') ||
			(ch >= 'a' && ch <= 'z');
		const bool hexDigit = digit || (ch >= 'A' && ch <= 'F') ||
			(ch >= 'a' && ch <= 'f');
		const unsigned char lowered = ch >= 'A' && ch <= 'Z'
			? static_cast<unsigned char>(ch + ('a' - 'A')) : ch;

		check(Ascii::isSpace(ch) == whitespace,
			"ASCII whitespace classification is exact");
		check(Ascii::isDigit(ch) == digit,
			"ASCII digit classification is exact");
		check(Ascii::isAlnum(ch) == (alpha || digit),
			"ASCII alphanumeric classification is exact");
		check(Ascii::isHexDigit(ch) == hexDigit,
			"ASCII hexadecimal classification is exact");
		check(Ascii::toLower(ch) == lowered,
			"ASCII lowercase conversion preserves non-ASCII bytes");
	}

	return failures == 0 ? 0 : 1;
}
