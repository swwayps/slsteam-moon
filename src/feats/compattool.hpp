#pragma once

// Pure, side-effect-free helper for reading the user's default Steam Play
// compatibility tool out of a `config/config.vdf` text body.
//
// Steam stores the global "Default compatibility tool" (the dropdown under
// Settings -> Compatibility) as a CompatToolMapping entry keyed by the
// special app-id "0":
//
//     "CompatToolMapping"
//     {
//         "0"
//         {
//             "name"      "proton-cachyos"
//             "config"    ""
//             "priority"  "75"
//         }
//         ...
//     }
//
// When we register a windows-only AddedApp for Proton we want to honour that
// user choice instead of hard-coding Proton Experimental.  This header holds
// the text parsing so it can be unit-tested in isolation (see
// tools/test_compattool.cpp); the config.vdf read/write lives in
// appinfo_provision.cpp.

#include <cstddef>
#include <cstdint>
#include <string>

namespace CompatTool
{
	// Find the matching close brace for the block whose opening '{' is at
	// `open`.  Returns the index of the closing '}', or std::string::npos if
	// unbalanced.
	inline std::size_t matchBrace(const std::string& s, std::size_t open)
	{
		if (open >= s.size() || s[open] != '{') return std::string::npos;
		int depth = 0;
		for (std::size_t i = open; i < s.size(); ++i)
		{
			if (s[i] == '{') ++depth;
			else if (s[i] == '}')
			{
				if (--depth == 0) return i;
			}
		}
		return std::string::npos;
	}

	// Read the quoted value that follows the `"name"` key inside the object
	// bracketed by [open, close].  Empty if absent.
	inline std::string readName(const std::string& s, std::size_t open, std::size_t close)
	{
		std::size_t namePos = s.find("\"name\"", open);
		if (namePos == std::string::npos || namePos >= close) return {};
		std::size_t q1 = s.find('"', namePos + 6); // opening quote of the value
		if (q1 == std::string::npos || q1 >= close) return {};
		std::size_t q2 = s.find('"', q1 + 1);
		if (q2 == std::string::npos || q2 >= close) return {};
		return s.substr(q1 + 1, q2 - q1 - 1);
	}

	// Position of the '{' opening the entry keyed `key` (quoted, e.g. "\"0\"")
	// inside the CompatToolMapping block, or npos. A key is followed by its
	// object brace, so the same digits used as a value do not match.
	inline std::size_t findMappingEntry(const std::string& content, const std::string& key)
	{
		const std::size_t mapPos = content.find("\"CompatToolMapping\"");
		if (mapPos == std::string::npos) return std::string::npos;
		const std::size_t mapBrace = content.find('{', mapPos);
		if (mapBrace == std::string::npos) return std::string::npos;
		const std::size_t mapEnd = matchBrace(content, mapBrace);
		if (mapEnd == std::string::npos) return std::string::npos;

		std::size_t k = mapBrace;
		while ((k = content.find(key, k)) != std::string::npos && k < mapEnd)
		{
			const std::size_t b = content.find_first_not_of(" \t\r\n", k + key.size());
			if (b != std::string::npos && b < mapEnd && content[b] == '{')
				return b;
			k += key.size();
		}
		return std::string::npos;
	}

	inline bool hasAppMapping(const std::string& content, std::uint32_t appId)
	{
		return findMappingEntry(content, "\"" + std::to_string(appId) + "\"")
			!= std::string::npos;
	}

	// Return the user's default compatibility-tool name from a config.vdf
	// body, or "" if Steam Play has no default tool set (or the file shape is
	// unexpected).  Robust against partial/malformed input.
	inline std::string parseDefaultTool(const std::string& content)
	{
		// The global default is the standalone "0" key.
		const std::size_t b = findMappingEntry(content, "\"0\"");
		if (b == std::string::npos) return {};
		return readName(content, b, matchBrace(content, b));
	}
}
