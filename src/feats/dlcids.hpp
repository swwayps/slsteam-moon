// SPDX-License-Identifier: AGPL-3.0-only
//
// DLC appid extraction from a provisioned appinfo wire-text buffer.
//
// Root cause (proven on the Zorin VM 2026-06-05 with Binding of Isaac
// 250900): Steam's depot-install planner only schedules a depot tagged
// with `dlcappid` when that DLC's appid is present in package 0's
// AppIdVec.  The existing PackagePatch injected only AdditionalApps
// (base ids), so DLC appids were never injected and their depots were
// filtered out — the base game installed but its DLC content never did.
//
// A base app advertises its DLC appids in two places inside its appinfo:
//   - extended.listofdlc      -> comma-separated DLC appid list
//   - depots.<id>.dlcappid    -> the DLC appid a depot belongs to
// (Mirrors how GBE/GSE fork tools and SFF resolve DLC ids.)
//
// This header is pure (no Steam/SDK deps) so it can be unit-tested with
// a stock g++, same pattern as feats/retry.hpp.  It parses the wire-text
// VDF dialect that AppInfoProvision emits / persists to
// `picsbuffer_<appid>.bin` (quoted keys and values, tab-separated).

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace AppInfoProvision
{

namespace detail
{

// Append the integer parsed from `tok` to `out`/`seen` if it is a
// non-zero appid different from `baseAppId`.
inline void addDlcId(std::vector<uint32_t>& out,
                     std::unordered_set<uint32_t>& seen,
                     const std::string& tok, uint32_t baseAppId)
{
	if (tok.empty()) return;
	for (char c : tok) if (c < '0' || c > '9') return;
	uint32_t id = 0;
	try { id = static_cast<uint32_t>(std::stoul(tok)); }
	catch (...) { return; }
	if (id == 0 || id == baseAppId) return;
	if (seen.insert(id).second) out.push_back(id);
}

// Return the quoted value that follows the FIRST occurrence of the
// quoted key `"<key>"` at or after `from`.  Sets `next` to one past the
// consumed value so callers can continue scanning.  Returns false when
// no further occurrence exists.
inline bool nextQuotedValueFor(const std::string& s, const std::string& key,
                               std::size_t from, std::string& value,
                               std::size_t& next)
{
	const std::string needle = "\"" + key + "\"";
	const std::size_t k = s.find(needle, from);
	if (k == std::string::npos) return false;

	// Find the opening quote of the value after the key.
	std::size_t i = k + needle.size();
	const std::size_t open = s.find('"', i);
	if (open == std::string::npos) { next = s.size(); return false; }
	const std::size_t close = s.find('"', open + 1);
	if (close == std::string::npos) { next = s.size(); return false; }

	value = s.substr(open + 1, close - (open + 1));
	next = close + 1;
	return true;
}

} // namespace detail

// Parse `wire` (a provisioned appinfo wire-text VDF) and return every
// DLC appid it advertises via `extended.listofdlc` and any
// `depots.<id>.dlcappid`, deduplicated and excluding `baseAppId`.
inline std::vector<uint32_t> extractDlcAppIds(const std::string& wire,
                                              uint32_t baseAppId)
{
	std::vector<uint32_t> out;
	std::unordered_set<uint32_t> seen;
	if (wire.empty()) return out;

	// Source A: extended.listofdlc (one comma-separated value; appears
	// at most once, but scan all occurrences defensively).
	{
		std::size_t pos = 0;
		std::string value;
		while (detail::nextQuotedValueFor(wire, "listofdlc", pos, value, pos))
		{
			std::size_t i = 0;
			while (i < value.size())
			{
				std::size_t j = value.find(',', i);
				if (j == std::string::npos) j = value.size();
				detail::addDlcId(out, seen, value.substr(i, j - i), baseAppId);
				i = j + 1;
			}
		}
	}

	// Source B: every depots.<id>.dlcappid value.
	{
		std::size_t pos = 0;
		std::string value;
		while (detail::nextQuotedValueFor(wire, "dlcappid", pos, value, pos))
		{
			detail::addDlcId(out, seen, value, baseAppId);
		}
	}

	return out;
}

// Classify ONE depot from the base app's appinfo, without touching the
// network or Steam's runtime structures.
//
// Needed because DepotEntry::DlcAppId is only observable inside the install
// planner, and the planner is not consulted on every re-plan: a DLC-only
// re-plan of an already-installed app never reported it, so bookkeeping that
// depended on it never ran.  The base app's own appinfo carries the same
// information: a content DLC's depot is either tagged `dlcappid` in the base
// app's depots block, or — when the DLC ships its own depots
// (`hasdepotsindlc`) — advertised by id in `extended.listofdlc`.  The base
// depot appears in neither, so a base/shared depot can never be misclassified.
//
// Returns the owning DLC appid, or 0 when `depotId` is not DLC content.
inline uint32_t dlcAppIdForDepot(const std::string& wire, uint32_t baseAppId,
                                 uint32_t depotId)
{
	if (wire.empty() || depotId == 0 || depotId == baseAppId) return 0;

	// Source A: an explicit `depots.<depotId>.dlcappid` tag. Scan the depot
	// keys so the tag is attributed to the right depot.
	{
		const std::string key = "\"" + std::to_string(depotId) + "\"";
		std::size_t pos = 0;
		while ((pos = wire.find(key, pos)) != std::string::npos)
		{
			pos += key.size();
			// The tag belongs to this depot only if it appears before the next
			// depot block opens a sibling key at the same nesting level; a
			// bounded window keeps this simple and allocation-free.
			const std::size_t window = wire.find("\"dlcappid\"", pos);
			if (window == std::string::npos) break;

			std::string value;
			std::size_t next = pos;
			if (detail::nextQuotedValueFor(wire, "dlcappid", pos, value, next))
			{
				std::vector<uint32_t> parsed;
				std::unordered_set<uint32_t> seen;
				detail::addDlcId(parsed, seen, value, baseAppId);
				if (!parsed.empty()) return parsed.front();
			}
			break;
		}
	}

	// Source B: the depot id is itself an advertised DLC appid, which is how
	// Steam models a DLC that ships its own depots.
	for (uint32_t advertised : extractDlcAppIds(wire, baseAppId))
	{
		if (advertised == depotId) return depotId;
	}

	return 0;
}

// Remove appids in `unsupported` from an extended.listofdlc value while
// preserving the original order and every unrelated token.  This keeps the
// ownership metadata consistent with pruneUnsupportedDepots(): advertising a
// DLC after all of its content depots were rejected makes Steam fetch the
// DLC's own appinfo and schedule those rejected depots anyway.
//
// `removedOut` counts removed occurrences (normally one per appid).
inline std::string filterUnsupportedDlcAppIds(
	const std::string& value,
	const std::unordered_set<uint32_t>& unsupported,
	std::size_t* removedOut = nullptr)
{
	std::string out;
	std::size_t removed = 0;
	bool firstKept = true;
	std::size_t i = 0;

	while (i <= value.size())
	{
		std::size_t j = value.find(',', i);
		if (j == std::string::npos) j = value.size();
		const std::string token = value.substr(i, j - i);

		bool remove = false;
		if (!token.empty())
		{
			bool numeric = true;
			for (char c : token)
			{
				if (c < '0' || c > '9') { numeric = false; break; }
			}
			if (numeric)
			{
				try
				{
					const unsigned long parsed = std::stoul(token);
					if (parsed <= UINT32_MAX &&
					    unsupported.count(static_cast<uint32_t>(parsed)) != 0)
					{
						remove = true;
					}
				}
				catch (...) {}
			}
		}

		if (remove)
		{
			++removed;
		}
		else
		{
			if (!firstKept) out.push_back(',');
			out += token;
			firstKept = false;
		}

		if (j == value.size()) break;
		i = j + 1;
	}

	if (removedOut) *removedOut = removed;
	return out;
}

} // namespace AppInfoProvision
