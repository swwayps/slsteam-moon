#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>


namespace Utils
{
	double calculateEntropy(const std::vector<uint8_t>& bytes);
	bool isNumber(const char* str);

	template<typename T>
	bool tryConvertToNumber(const char* str, T& out)
	{
		static_assert(std::is_integral_v<T>, "tryConvertToNumber parses integers");

		// Digits only (no sign, no blanks). from_chars reports a value that does
		// not fit instead of throwing: std::stoul raised out_of_range past 2^32
		// on i386, on the API watcher thread.
		if (!isNumber(str))
		{
			return false;
		}

		const char* end = str + std::strlen(str);
		T value {};
		const auto [ptr, ec] = std::from_chars(str, end, value);
		if (ec != std::errc() || ptr != end)
		{
			return false;
		}

		out = value;
		return true;
	}

	std::vector<std::string> strsplit(const char* str, const char* delimeter);
	std::string getFileSHA256(const char* filePath);

	// Reads the GNU build-id (NT_GNU_BUILD_ID note) from an ELF file and
	// returns it as a lowercase hex string, or "" if the file can't be read
	// or carries no build-id. Handles both ELF32 and ELF64 (little-endian).
	std::string getBuildId(const char* filePath);
}
