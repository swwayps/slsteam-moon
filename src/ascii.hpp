#pragma once

// Locale-free byte classification for code that can run from the rtld-audit
// namespace.  glibc's ctype functions consult per-thread locale TLS, which is
// not guaranteed to exist on Steam-owned threads entering the audit module.
namespace Ascii
{
constexpr bool isSpace(unsigned char value) noexcept
{
	return value == ' ' || value == '\t' || value == '\n' || value == '\v' ||
		value == '\f' || value == '\r';
}

constexpr bool isDigit(unsigned char value) noexcept
{
	return value >= '0' && value <= '9';
}

constexpr bool isAlnum(unsigned char value) noexcept
{
	return isDigit(value) || (value >= 'A' && value <= 'Z') ||
		(value >= 'a' && value <= 'z');
}

constexpr bool isHexDigit(unsigned char value) noexcept
{
	return isDigit(value) || (value >= 'A' && value <= 'F') ||
		(value >= 'a' && value <= 'f');
}

constexpr unsigned char toLower(unsigned char value) noexcept
{
	return value >= 'A' && value <= 'Z'
		? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}
}
