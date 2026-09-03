// SPDX-License-Identifier: AGPL-3.0-only
//
// synthmark — persistent "synthetic appinfo" markers + outgoing-PICS strip.
//
// Why this exists
// ---------------
// Token-locked titles (their PICS product-info access token is DENIED to
// the client — e.g. Risk of Rain 2, app 632360) come back from Steam's
// runtime RequestAppInfoUpdate with an EMPTY product-info buffer.  When
// that empty refresh lands, Steam overwrites the depots + installdir that
// manifestsynth rebuilt into appinfo.vdf at startup, so the install dialog
// drops to "0 B" and fails with "Invalid install path".
//
// The cure is to keep Steam from ever re-fetching those apps: provisioning
// MARKS an app synthetic when it had to rebuild depots from local
// manifests, and the outgoing-PICS hook (apps.cpp::sendPICSInfoRequest)
// STRIPS marked apps from Steam's product-info request.  Steam then never
// receives the empty refresh and keeps the appinfo we spliced at startup.
//
// The mark must be PERSISTED, not just in-memory: Steam re-execs setup()
// several times per boot, and the surviving process can hit the
// provisioning cache (TTL) and skip synthesis entirely — an in-memory set
// would be empty in exactly the process that issues the PICS requests.  A
// marker file (`<cacheDir>/synthetic_<appid>`) survives that.
//
// Kept dependency-light (only std + std::filesystem, dir injected) so it is
// host-unit-testable (tools/test_synthmark.cpp); the real cache dir is
// supplied by appinfo_provision.cpp.

#pragma once

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <system_error>
#include <sys/syscall.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <unistd.h>
#include <vector>

#include <filesystem>

namespace SynthMark
{
	inline std::string markerPath(const std::string& dir, uint32_t appId)
	{
		return dir + "/synthetic_" + std::to_string(appId);
	}

	// Record that appId's appinfo depots were synthesized (token-locked).
	// Idempotent; returns true if the marker exists afterwards.
	inline bool mark(const std::string& dir, uint32_t appId)
	{
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const auto path = markerPath(dir, appId);
		if (std::filesystem::exists(path, ec)) return true;
		std::FILE* f = std::fopen(path.c_str(), "w");
		if (!f) return false;
		std::fclose(f);
		return true;
	}

	// Remove a marker (remove-game cleanup).  Returns true if it is gone
	// afterwards (including when it never existed).
	inline bool unmark(const std::string& dir, uint32_t appId)
	{
		std::error_code ec;
		std::filesystem::remove(markerPath(dir, appId), ec);
		return !std::filesystem::exists(markerPath(dir, appId), ec);
	}

	// True iff appId is marked synthetic.  A single stat; cheap enough to
	// call per outgoing PICS request.  Missing dir / file -> false.
	inline bool isMarked(const std::string& dir, uint32_t appId)
	{
		std::error_code ec;
		return std::filesystem::exists(markerPath(dir, appId), ec);
	}

	// Every marked appid in `dir` (empty if the dir is missing).
	inline std::unordered_set<uint32_t> loadAll(const std::string& dir)
	{
		std::unordered_set<uint32_t> out;
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec)) return out;
		for (const auto& e : std::filesystem::directory_iterator(dir, ec))
		{
			if (ec) break;
			const auto name = e.path().filename().string();
			constexpr const char* kPrefix = "synthetic_";
			if (name.rfind(kPrefix, 0) != 0) continue;
			const std::string idStr = name.substr(std::char_traits<char>::length(kPrefix));
			if (idStr.empty()) continue;
			try
			{
				size_t consumed = 0;
				const unsigned long v = std::stoul(idStr, &consumed);
				if (consumed == idStr.size() && v != 0)
					out.insert(static_cast<uint32_t>(v));
			}
			catch (...) { continue; }
		}
		return out;
	}

	struct StripLimits
	{
		std::uint64_t maxStrips = 8;
		std::uint64_t maxSeconds = 60;
	};

	struct StripState
	{
		std::uint64_t count = 0;
		std::int64_t firstStripAt = 0;
	};

	enum class StripDecision
	{
		Allow,
		Disabled,
		CountLimit,
		TimeLimit,
	};

	struct StripEvaluation
	{
		StripDecision decision = StripDecision::Disabled;
		StripState nextState{};
	};

	namespace detail
	{
		inline bool parseUnsigned(const char* text, std::uint64_t& out)
		{
			if (text == nullptr || *text == '\0') return false;
			const std::string value(text);
			const auto first = value.data();
			const auto last = first + value.size();
			const auto parsed = std::from_chars(first, last, out, 10);
			return parsed.ec == std::errc{} && parsed.ptr == last;
		}

		inline bool parseCacheAppId(const std::string& filename,
		                           const char* prefix,
		                           const char* suffix,
		                           std::uint32_t& appId)
		{
			const std::size_t prefixSize = std::char_traits<char>::length(prefix);
			const std::size_t suffixSize = std::char_traits<char>::length(suffix);
			if (filename.size() < prefixSize + suffixSize ||
			    filename.compare(0, prefixSize, prefix) != 0)
			{
				return false;
			}
			const std::size_t idSize = filename.size() - prefixSize - suffixSize;
			if (idSize == 0 ||
			    filename.compare(prefixSize + idSize, suffixSize, suffix) != 0)
			{
				return false;
			}

			const char* first = filename.data() + prefixSize;
			const char* last = first + idSize;
			std::uint64_t value = 0;
			const auto parsed = std::from_chars(first, last, value, 10);
			if (parsed.ec != std::errc{} || parsed.ptr != last || value == 0 ||
			    value > std::numeric_limits<std::uint32_t>::max())
			{
				return false;
			}
			appId = static_cast<std::uint32_t>(value);
			return true;
		}

		inline bool cacheAppId(const std::string& filename, std::uint32_t& appId)
		{
			// Only these names encode an appid.  manifestid_<n>.yaml is
			// depot-scoped, so it cannot be classified from managed appids
			// without an explicit app-to-depot relation.
			return parseCacheAppId(filename, "synthetic_", "", appId) ||
			       parseCacheAppId(filename, "picsbuffer_", ".bin", appId) ||
			       parseCacheAppId(filename, "picsbuffer_", ".yaml", appId) ||
			       parseCacheAppId(filename, "dlcmetadata_", ".yaml", appId) ||
			       parseCacheAppId(filename, "terminal_", ".state", appId);
		}

		inline bool renameNoReplace(const std::filesystem::path& original,
		                           const std::filesystem::path& target,
		                           std::error_code& ec)
		{
#if defined(__linux__) && defined(SYS_renameat2)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
			if (::syscall(SYS_renameat2, AT_FDCWD, original.c_str(), AT_FDCWD,
			              target.c_str(), RENAME_NOREPLACE) == 0)
			{
				ec.clear();
				return true;
			}
			const int savedErrno = errno;
			if (savedErrno != ENOSYS && savedErrno != EINVAL)
			{
				ec = std::error_code(savedErrno, std::generic_category());
				return false;
			}
#endif
			// The fallback is for platforms without renameat2. A hard-link
			// followed by unlink is still no-replace: link(2) fails atomically
			// when the destination exists, and a failed unlink removes the new
			// link so the source remains recoverable.
			std::error_code existsEc;
			if (std::filesystem::exists(target, existsEc))
			{
				ec = std::make_error_code(std::errc::file_exists);
				return false;
			}
			if (existsEc)
			{
				ec = existsEc;
				return false;
			}
			std::filesystem::create_hard_link(original, target, ec);
			if (ec) return false;
			std::error_code removeEc;
			std::filesystem::remove(original, removeEc);
			if (!removeEc) return true;
			std::error_code cleanupEc;
			std::filesystem::remove(target, cleanupEc);
			ec = removeEc;
			return false;
		}

		inline bool quarantineFile(const std::filesystem::path& original,
		                          const std::string& suffix,
		                          std::filesystem::path& quarantined)
		{
			if (suffix.empty()) return false;
			constexpr unsigned int kMaxCollisionRetries = 1024;
			for (unsigned int attempt = 0; attempt < kMaxCollisionRetries; ++attempt)
			{
				std::string name = original.string() + suffix;
				if (attempt != 0) name += "." + std::to_string(attempt);
				const auto candidate = std::filesystem::path(name);
				std::error_code ec;
				if (renameNoReplace(original, candidate, ec))
				{
					quarantined = candidate;
					return true;
				}
				if (ec != std::make_error_code(std::errc::file_exists))
					return false;
			}
			return false;
		}

	inline std::vector<std::string> appArtifactNames(
	    std::uint32_t appId,
	    const std::vector<std::uint32_t>& relatedDepotIds,
	    bool includeTicketArtifacts = true,
	    bool includeSyntheticMarker = true)
	{
		std::vector<std::string> names;
		if (appId == 0) return names;
		std::unordered_set<std::string> seen;
		const auto add = [&](std::string name) {
			if (seen.insert(name).second) names.push_back(std::move(name));
		};
		const std::string id = std::to_string(appId);
		add("picsbuffer_" + id + ".bin");
		add("picsbuffer_" + id + ".yaml");
		add("terminal_" + id + ".state");
		add("dlcmetadata_" + id + ".yaml");
		if (includeSyntheticMarker)
			add("synthetic_" + id);
		if (includeTicketArtifacts)
		{
			add("ticket_" + id + ".yaml");
			add("encryptedTicket_" + id + ".yaml");
		}
		for (const std::uint32_t depotId : relatedDepotIds)
		{
			if (depotId != 0)
				add("manifestid_" + std::to_string(depotId) + ".yaml");
		}
		return names;
	}
}

	// Parse the optional environment overrides without reading the process
	// environment here.  The hook can pass getenv() results, while tests and
	// callers that need deterministic policy can pass explicit strings.
	inline StripLimits parseStripLimits(const char* maxStripsText,
	                                    const char* maxSecondsText)
	{
		StripLimits out;
		std::uint64_t parsed = 0;
		if (detail::parseUnsigned(maxStripsText, parsed))
			out.maxStrips = parsed;
		if (detail::parseUnsigned(maxSecondsText, parsed))
			out.maxSeconds = parsed;
		return out;
	}

	// Pure state transition for one candidate strip.  The caller owns the
	// per-app state (and may protect it with a mutex); this helper only decides
	// whether the attempt is allowed and returns the state after that attempt.
	inline StripEvaluation evaluateStrip(const StripState& state,
	                                     const StripLimits& limits,
	                                     std::int64_t now)
	{
		if (limits.maxStrips == 0)
			return {StripDecision::Disabled, state};
		if (state.count >= limits.maxStrips)
			return {StripDecision::CountLimit, state};
		if (state.count != 0 && now >= state.firstStripAt &&
		    static_cast<std::uint64_t>(now - state.firstStripAt) >=
		        limits.maxSeconds)
		{
			return {StripDecision::TimeLimit, state};
		}

		StripState next = state;
		if (next.count == 0) next.firstStripAt = now;
		++next.count;
		return {StripDecision::Allow, next};
	}

	// Thread-safe process-local strip budget. The caller decides whether an
	// app is eligible; this component only reserves one strip and retains the
	// count/timestamp independently for each appid.
	class StripBudget
	{
	public:
		StripEvaluation reserve(std::uint32_t appId,
		                        const StripLimits& limits,
		                        std::int64_t now)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto it = states_.find(appId);
			const StripState state =
			    it == states_.end() ? StripState{} : it->second;
			const StripEvaluation evaluation = evaluateStrip(state, limits, now);
			if (evaluation.decision == StripDecision::Allow)
				states_[appId] = evaluation.nextState;
			return evaluation;
		}

	private:
		std::mutex mutex_;
		std::unordered_map<std::uint32_t, StripState> states_;
	};

	inline bool isManagedSynthetic(
	    std::uint32_t appId,
	    const std::function<bool(std::uint32_t)>& isSynthetic,
	    const std::function<bool(std::uint32_t)>& isManaged)
	{
		return isManaged && isManaged(appId) && isSynthetic && isSynthetic(appId);
	}

	// Install-state gate for the outgoing-PICS protection. Until the app
	// manager interface is resolved, preserve the early-boot behavior and
	// strip every locally authoritative app. Once resolved, a fully installed
	// app no longer needs protection from an empty refresh.
	inline bool installStateAllowsStrip(bool locallyAuthoritative,
	                                    bool isManaged,
	                                    bool appManagerResolved,
	                                    bool fullyInstalled)
	{
		if (!locallyAuthoritative || !isManaged) return false;
		return !appManagerResolved || !fullyInstalled;
	}

	struct QuarantineRecord
	{
		std::filesystem::path original;
		std::filesystem::path quarantined;
	};

	// Rename known per-app cache artifacts for apps no longer active. The
	// caller supplies active compatibility ids as well as managed sources so a
	// live app's retained synthetic-PICS marker is not quarantined early. A
	// unique suffix lets the original path be restored without deleting data.
	inline std::vector<QuarantineRecord> quarantineOrphans(
	    const std::string& dir,
	    const std::unordered_set<std::uint32_t>& managedAppIds,
	    const std::string& suffix)
	{
		std::vector<QuarantineRecord> out;
		if (suffix.empty()) return out;

		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec) || ec) return out;
		for (std::filesystem::directory_iterator it(dir, ec), end;
		     it != end && !ec; it.increment(ec))
		{
			std::error_code fileEc;
			if (!it->is_regular_file(fileEc) || fileEc) continue;

			std::uint32_t appId = 0;
			const auto original = it->path();
			if (!detail::cacheAppId(original.filename().string(), appId) ||
			    managedAppIds.count(appId) != 0)
			{
				continue;
			}

			std::filesystem::path quarantined;
			if (!detail::quarantineFile(original, suffix, quarantined))
				continue;
			out.push_back({original, quarantined});
		}
		return out;
	}

	// Quarantine all artifacts whose names are explicitly tied to one app.
	// ManifestId catalogs are depot-scoped; only the caller's relation index
	// may supply the depot ids that belong to this app. This prevents an app
	// id that happens to be a different depot from being quarantined blindly.
	inline std::vector<QuarantineRecord> quarantineAppArtifacts(
		const std::string& dir,
		std::uint32_t appId,
		const std::vector<std::uint32_t>& relatedDepotIds,
		const std::string& suffix,
		bool includeTicketArtifacts = true,
		bool includeSyntheticMarker = true)
	{
		std::vector<QuarantineRecord> out;
		if (appId == 0 || suffix.empty()) return out;
		for (const auto& name : detail::appArtifactNames(
				appId, relatedDepotIds, includeTicketArtifacts,
				includeSyntheticMarker))
		{
			const auto original = std::filesystem::path(dir) / name;
			std::error_code ec;
			if (!std::filesystem::is_regular_file(original, ec) || ec)
				continue;

			std::filesystem::path quarantined;
			if (!detail::quarantineFile(original, suffix, quarantined))
				continue;
			out.push_back({original, quarantined});
		}
		return out;
	}

	// Compatibility overload for callers that only need app-scoped state.
	inline std::vector<QuarantineRecord> quarantineAppArtifacts(
		const std::string& dir,
		std::uint32_t appId,
		const std::string& suffix)
	{
		return quarantineAppArtifacts(dir, appId,
		                             std::vector<std::uint32_t>{}, suffix);
	}

	// True when an original app-scoped or relation-scoped artifact still exists
	// after a quarantine attempt. Callers use this to turn a partial move into
	// an observable failure instead of silently claiming that cleanup completed.
	inline bool hasAppArtifacts(
		const std::string& dir,
		std::uint32_t appId,
		const std::vector<std::uint32_t>& relatedDepotIds,
		bool includeTicketArtifacts = true,
		bool includeSyntheticMarker = true)
	{
		if (appId == 0) return false;
		for (const auto& name : detail::appArtifactNames(
				appId, relatedDepotIds, includeTicketArtifacts,
				includeSyntheticMarker))
		{
			std::error_code ec;
			const bool regular = std::filesystem::is_regular_file(
				std::filesystem::path(dir) / name, ec);
			if (ec || regular) return true;
		}
		return false;
	}

	inline bool hasAppArtifacts(const std::string& dir, std::uint32_t appId)
	{
		return hasAppArtifacts(dir, appId, std::vector<std::uint32_t>{});
	}

	inline bool restoreQuarantined(const QuarantineRecord& record)
	{
		if (record.original.empty() || record.quarantined.empty()) return false;
		std::error_code ec;
		if (!std::filesystem::exists(record.quarantined, ec) || ec) return false;
		ec.clear();
		if (std::filesystem::exists(record.original, ec) || ec) return false;
		ec.clear();
		std::filesystem::rename(record.quarantined, record.original, ec);
		if (ec) return false;
		std::error_code verifyEc;
		return std::filesystem::exists(record.original, verifyEc) && !verifyEc;
	}

	// Indices (DESCENDING) of `requestedAppIds` that are synthetic, so the
	// caller can delete them in place from a protobuf repeated field without
	// invalidating the not-yet-processed indices.  Pure.
	inline std::vector<int> stripIndices(
	    const std::vector<uint32_t>& requestedAppIds,
	    const std::function<bool(uint32_t)>& isSynthetic)
	{
		std::vector<int> out;
		for (int i = static_cast<int>(requestedAppIds.size()) - 1; i >= 0; --i)
			if (isSynthetic(requestedAppIds[static_cast<size_t>(i)]))
				out.push_back(i);
		return out;
	}

	inline std::vector<int> stripIndices(
	    const std::vector<uint32_t>& requestedAppIds,
	    const std::function<bool(uint32_t)>& isSynthetic,
	    const std::function<bool(uint32_t)>& isManaged)
	{
		return stripIndices(requestedAppIds,
		    [&](uint32_t appId) {
				return isManagedSynthetic(appId, isSynthetic, isManaged);
		    });
	}
}
