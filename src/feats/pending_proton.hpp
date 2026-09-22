// SPDX-License-Identifier: AGPL-3.0-only
//
// Strict parsing for the deferred Proton mapping cache record.

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <system_error>

namespace AppInfoProvision
{

enum class PendingProtonParseStatus
{
	Valid,
	Invalid,
};

struct PendingProtonParseResult
{
	PendingProtonParseStatus status = PendingProtonParseStatus::Invalid;
	std::set<uint32_t> ids;
};

inline std::set<std::string> parsePlatformOsList(std::string_view text)
{
	std::set<std::string> out;
	std::size_t pos = 0;
	while (pos < text.size())
	{
		std::size_t end = text.find(',', pos);
		if (end == std::string_view::npos) end = text.size();
		while (pos < end && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
		while (end > pos && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
		if (pos < end) out.emplace(text.substr(pos, end - pos));
		pos = end + 1;
	}
	return out;
}

// Decide whether an app with surviving content must be forced onto Proton.
//
// A per-depot oslist is unreliable for proving native Linux support:
// publishers frequently mis-tag a Windows-only title's content depot with
// "windows,linux" even though no native build exists (e.g. app 2497920 depot
// 2497921, while common.oslist is "windows,macos").  common.oslist is the
// app-level platform list Steam itself uses to decide native vs. Proton, so
// it is authoritative for whether native Linux exists at all; a surviving
// depot's linux tag is trusted only to confirm that the Linux content is
// actually present, and stands alone only when common.oslist is absent.
//
// Concretely, a genuine native Linux build requires BOTH a surviving depot
// that targets Linux AND app-level Linux support.  If either is missing the
// app can only run through Proton.
inline bool requiresProtonMapping(
	std::size_t keptContentDepots,
	const std::set<std::string>& depotOs,
	std::string_view commonOsList)
{
	if (keptContentDepots == 0) return false;

	const auto commonOs = parsePlatformOsList(commonOsList);

	// No per-depot platform info: fall back to the app-level list (and when
	// that is also absent, nothing forces Proton).
	if (depotOs.empty())
		return !commonOs.empty() && commonOs.count("linux") == 0;

	const bool depotTargetsLinux = depotOs.count("linux") != 0;
	const bool nativeLinuxAvailable = commonOs.empty()
		? depotTargetsLinux
		: (depotTargetsLinux && commonOs.count("linux") != 0);
	return !nativeLinuxAvailable;
}

// Parse one complete proton-mappings.pending record. The file format is one
// non-zero uint32 appid per whitespace-delimited token. A malformed token
// invalidates the entire record so callers never replace a valid file with a
// partial interpretation of corrupt or truncated input.
inline PendingProtonParseResult parsePendingProtonText(std::string_view text)
{
	PendingProtonParseResult result;
	std::size_t pos = 0;
	while (pos < text.size())
	{
		while (pos < text.size() &&
		       (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
	        text[pos] == '\r' || text[pos] == '\f' || text[pos] == '\v'))
		{
			++pos;
		}
		if (pos == text.size()) break;

		const std::size_t tokenStart = pos;
		while (pos < text.size() &&
		       text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n' &&
		       text[pos] != '\r' && text[pos] != '\f' && text[pos] != '\v')
		{
			++pos;
		}

		uint32_t id = 0;
		const auto first = text.data() + tokenStart;
		const auto last = text.data() + pos;
		const auto parsed = std::from_chars(first, last, id, 10);
		if (parsed.ec != std::errc{} || parsed.ptr != last || id == 0)
		{
			result.ids.clear();
			return result;
		}
		result.ids.insert(id);
	}

	result.status = PendingProtonParseStatus::Valid;
	return result;
}

} // namespace AppInfoProvision
