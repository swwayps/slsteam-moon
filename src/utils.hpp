#pragma once

#include <string>
#include <vector>


namespace Utils
{
	std::vector<std::string> strsplit(const char* str, const char* delimeter);
	std::string getFileSHA256(const char* filePath);

	// Reads the GNU build-id (NT_GNU_BUILD_ID note) from an ELF file and
	// returns it as a lowercase hex string, or "" if the file can't be read
	// or carries no build-id. Handles both ELF32 and ELF64 (little-endian).
	std::string getBuildId(const char* filePath);
}
