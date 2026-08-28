#pragma once

#include <string>
#include <string_view>
#include <vector>


namespace ManifestZip
{
	// Extract the single payload carried by a Steam manifest ZIP. The parser is
	// intentionally narrow: encrypted, multi-entry, ZIP64, and data-descriptor
	// archives are rejected instead of being interpreted ambiguously.
	bool extractSingleFile(std::string_view archive,
	                       std::vector<unsigned char>& output,
	                       std::string* diagnostic = nullptr);
}
