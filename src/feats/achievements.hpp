#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CMsgProtoBufHeader;
class CNetPacket;

namespace Achievements
{
inline constexpr uint32_t maxReplyBytes = 262144;
inline constexpr uint32_t maxHeaderBytes = 1024;
// A reviewer lookup is not repeated for the same game within this window.
inline constexpr uint64_t kOwnerLookupCooldownSecs = 600;

enum class OwnerSource : uint8_t { None, Override, Preferred, Fallback, Candidate };
enum class OwnerVerdict : uint8_t { Unknown, Good, Bad };
struct OwnerChoice
{
	uint64_t owner = 0;
	OwnerSource source = OwnerSource::None;
	bool lookup = false;
};

inline bool isIndividualSteamId(uint64_t id)
{
	return (id >> 32) == 0x01100001u && static_cast<uint32_t>(id) != 0;
}

// Recent reviewers own the game. filter=1 lists them newest first and
// purchase_type=1 includes key activations; the brackets stay encoded because
// the curl CLI would read them as a glob.
inline std::string reviewUrl(uint32_t app, uint32_t count)
{
	return "https://api.steampowered.com/IUserReviewsService/GetAppReviews/v1/?appid=" +
		std::to_string(app) + "&filter=1&languages%5B0%5D=all&purchase_type=1&num_per_page=" +
		std::to_string(count);
}

// Reviewer SteamID64s in page order, deduplicated, at most `max`.
inline std::vector<uint64_t> parseReviewers(std::string_view page, std::size_t max)
{
	constexpr std::string_view key = "\"steamid\":\"";
	const char* const end = page.data() + page.size();
	std::vector<uint64_t> out;
	for (auto pos = page.find(key);
	     pos != std::string_view::npos && out.size() < max;
	     pos = page.find(key, pos))
	{
		pos += key.size();
		uint64_t id = 0;
		const auto [next, error] = std::from_chars(page.data() + pos, end, id);
		if (error == std::errc() && next != end && *next == '"' &&
		    isIndividualSteamId(id) && std::find(out.begin(), out.end(), id) == out.end())
			out.push_back(id);
	}
	return out;
}

// Per-game schema owner. Never blocks: when no owner is left, choose() asks
// the caller for one background lookup, whose result arrives through offer().
// Order: AchievementOwners override, the owner that answered last, looked-up
// reviewers, then the AchievementOwnerId fallback; a lookup starts when there
// is no usable fallback. serialize()/restore() carry what was learned
// across restarts, so the first request of a session can use it.
class OwnerDirectory
{
public:
	OwnerChoice choose(uint32_t app, uint64_t override, uint64_t fallback,
	                   uint64_t self, bool lookupEnabled, uint64_t nowSecs)
	{
		if (override) return {override, OwnerSource::Override, false};
		std::lock_guard lock(m_mutex);
		Entry& entry = m_apps[app];
		if (entry.preferred) return {entry.preferred, OwnerSource::Preferred, false};
		for (const uint64_t candidate : entry.candidates)
		{
			if (candidate != self)
				return {candidate, OwnerSource::Candidate, false};
		}
		if (fallback && fallback != self && !entry.bad.contains(fallback))
			return {fallback, OwnerSource::Fallback, false};
		if (!lookupEnabled || entry.looking ||
		    (entry.looked && nowSecs < entry.lookedAt + kOwnerLookupCooldownSecs))
			return {};
		entry.looking = true;
		entry.looked = true;
		entry.lookedAt = nowSecs;
		return {0, OwnerSource::None, true};
	}

	// An explicit override is the user's choice and is never replaced.
	// Returns true when what was learned changed.
	bool report(uint32_t app, uint64_t owner, OwnerSource source, OwnerVerdict verdict)
	{
		if (!owner || source == OwnerSource::None || source == OwnerSource::Override ||
		    verdict == OwnerVerdict::Unknown)
			return false;
		std::lock_guard lock(m_mutex);
		Entry& entry = m_apps[app];
		if (verdict == OwnerVerdict::Good)
		{
			const bool changed = entry.preferred != owner;
			entry.preferred = owner;
			return changed;
		}
		const bool changed = entry.bad.insert(owner).second;
		if (entry.preferred == owner) entry.preferred = 0;
		std::erase(entry.candidates, owner);
		return changed;
	}

	void offer(uint32_t app, const std::vector<uint64_t>& owners)
	{
		std::lock_guard lock(m_mutex);
		Entry& entry = m_apps[app];
		entry.looking = false;
		entry.candidates.clear();
		for (const uint64_t owner : owners)
		{
			if (!entry.bad.contains(owner)) entry.candidates.push_back(owner);
		}
	}

	// One line per game: "app preferred candidate...", preferred 0 when none.
	std::string serialize() const
	{
		std::lock_guard lock(m_mutex);
		std::string out;
		for (const auto& [app, entry] : m_apps)
		{
			if (!entry.preferred && entry.candidates.empty()) continue;
			out += std::to_string(app) + ' ' + std::to_string(entry.preferred);
			for (const uint64_t candidate : entry.candidates)
				out += ' ' + std::to_string(candidate);
			out += '\n';
		}
		return out;
	}

	// Lines that do not parse are skipped; ids that are not individual
	// accounts are dropped.
	void restore(std::string_view text)
	{
		std::lock_guard lock(m_mutex);
		while (!text.empty())
		{
			const auto end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
			std::vector<uint64_t> values;
			bool valid = true;
			while (valid && !line.empty())
			{
				const auto space = line.find(' ');
				const std::string_view token = line.substr(0, space);
				line = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
				uint64_t value = 0;
				const auto [next, error] =
					std::from_chars(token.data(), token.data() + token.size(), value);
				valid = error == std::errc() && next == token.data() + token.size();
				values.push_back(value);
			}
			if (!valid || values.size() < 2 || !values[0] || values[0] > UINT32_MAX)
				continue;
			Entry& entry = m_apps[static_cast<uint32_t>(values[0])];
			if (isIndividualSteamId(values[1])) entry.preferred = values[1];
			for (std::size_t i = 2; i < values.size() && entry.candidates.size() < kMaxRestored; ++i)
			{
				if (isIndividualSteamId(values[i]) &&
				    std::find(entry.candidates.begin(), entry.candidates.end(), values[i]) ==
				        entry.candidates.end())
					entry.candidates.push_back(values[i]);
			}
		}
	}

private:
	static constexpr std::size_t kMaxRestored = 64;
	struct Entry
	{
		uint64_t preferred = 0;
		std::vector<uint64_t> candidates;
		std::unordered_set<uint64_t> bad;
		uint64_t lookedAt = 0;
		bool looked = false;
		bool looking = false;
	};
	mutable std::mutex m_mutex;
	std::unordered_map<uint32_t, Entry> m_apps;
};

// Empty request output / nullopt response means byte-for-byte passthrough.
// A present empty response is a correlated failure with header.eresult=Fail.
// Correlation uses final CM job IDs, never app ID alone.
std::vector<uint8_t> rewriteRequest(bool modern, const uint8_t* data,
	uint32_t size, const CMsgProtoBufHeader& header);
std::optional<std::vector<uint8_t>> rewriteResponse(bool modern, const uint8_t* data,
	uint32_t size, CMsgProtoBufHeader& header);
// Apply rewriteResponse to an inbound CM packet (ServiceMethodResponse or
// ClientGetUserStatsResponse). True when the packet now carries the rewrite.
bool recvReply(CNetPacket* pkt);
// Called by the CM-receive dispatch with the login/logoff/license eMsg and
// the parsed header's steamid.
void recvMessage(uint32_t type, bool hasSteamId, uint64_t steamId);
// Result of a background reviewer lookup; empty when it failed.
void offerOwners(uint32_t app, const std::vector<uint64_t>& owners);
// Learned owners in <config>/cache/achievement_owners.txt. loadOwners runs at
// setup; saveOwners runs on the lookup worker only.
void loadOwners();
void saveOwners();
// Queue one reviewer lookup of `count` owners, or one save of the learned
// owners, on the background worker (achievement_lookup.cpp).
void requestOwnerLookup(uint32_t app, uint32_t count);
void requestOwnerSave();
}
