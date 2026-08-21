#pragma once

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
		if constexpr (std::is_same_v<T, int32_t>)
		{
			if (!isNumber(str))
			{
				return false;
			}

			out = std::stoi(str);
			return true;
		}

		else if constexpr (std::is_same_v<T, uint32_t>)
		{
			if (!isNumber(str))
			{
				return false;
			}

			out = std::stoul(str);
			return true;
		}

		//TODO: Add GCC error when compiling this path
		return false;
	}

	std::vector<std::string> strsplit(const char* str, const char* delimeter);
	std::string getFileSHA256(const char* filePath);

	// Reads the GNU build-id (NT_GNU_BUILD_ID note) from an ELF file and
	// returns it as a lowercase hex string, or "" if the file can't be read
	// or carries no build-id. Handles both ELF32 and ELF64 (little-endian).
	std::string getBuildId(const char* filePath);
}
