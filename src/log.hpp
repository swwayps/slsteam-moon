#pragma once

#include "notify.hpp" // LogLevel + the notify-send command builder
#include "usermsg.hpp" // user-facing message catalog (UserMsg/Lang/messageFor)

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <openssl/sha.h>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

// Per-thread popup suppression.  A worker thread can set this so its own
// log output still reaches the file but never raises a desktop popup.  Used
// by the background manifest pre-warm worker (feats/prewarm.cpp), whose
// every-30s re-stage of purged DLC manifests would otherwise fire a notify-
// send popup per failing depot per pass.  Thread-local: the synchronous
// install path runs on a different thread and keeps its popups.
inline thread_local bool t_suppressNotify = false;

// RAII helper to suppress popups for the current scope (restores the prior
// value on exit), for callers that want scoped rather than whole-thread
// suppression.
struct ScopedNotifySuppression
{
	bool m_prev;
	ScopedNotifySuppression() : m_prev(t_suppressNotify) { t_suppressNotify = true; }
	~ScopedNotifySuppression() { t_suppressNotify = m_prev; }
};

class CLog
{
	std::ofstream ofstream;
	std::unordered_set<std::string> msgHist {};
	std::shared_mutex mutex;

	// User-facing popup state. m_lang is resolved once from the environment
	// at construction; m_notifyThrottle collapses per-category bursts into a
	// single popup (see Notify::NotifyThrottle). Guarded by m_notifyMu, which
	// is independent of `mutex` so a popup never contends with file writes.
	Lang m_lang = Lang::English;
	Notify::NotifyThrottle m_notifyThrottle {};
	std::mutex m_notifyMu;

	constexpr const char* logLvlToStr(LogLevel& lvl)
	{
		switch(lvl)
		{
			case LogLevel::Once:
				return "Once";
			case LogLevel::Debug:
				return "Debug";
			case LogLevel::Info:
				return "Info";
			case LogLevel::NotifyShort:
			case LogLevel::NotifyLong:
				return "Notify";
			case LogLevel::Warn:
				return "Warn";

			//Shut gcc warning up
			default:
				return "Unknown";
		}
	}

	template<typename ...Args>
	__attribute__((hot))
	void __log(LogLevel lvl, const char* msg, Args... args)
	{
		if (lvl < getMinLevel())
		{
			return;
		}

		size_t size = snprintf(nullptr, 0, msg, args...) + 1; //Allocate one more byte for zero termination
		std::string formatted;
		formatted.resize(size);
		snprintf(formatted.data(), size, msg, args...);

		// NOTE: __log no longer raises desktop popups. Every log level here
		// (warn/notify/info/...) writes ONLY to the file, so the raw
		// developer diagnostics stay available for debugging without ever
		// reaching the user's screen. The single popup path is notifyUser(),
		// which renders a friendly, localized, throttled message from the
		// usermsg.hpp catalog instead.

		const auto lock = std::unique_lock(mutex);

		if (lvl == LogLevel::Once)
		{
			for(const auto& oldMsg : msgHist)
			{
				if (oldMsg == formatted)
				{
					return;
				}
			}

			msgHist.emplace(formatted);
		}

		ofstream << "[" << logLvlToStr(lvl) << "] " << formatted.c_str();
		if (lvl == LogLevel::NotifyShort || lvl == LogLevel::NotifyLong)
		{
			ofstream << "\n";
		}

		ofstream.flush();
	}

	// Per-(level,message) deduplication state used by the *Once
	// helpers below.  We can't piggy-back on msgHist because that
	// set holds raw strings and is also touched by LogLevel::Once
	// — we want suppression scoped to the helper, not to literal
	// equality across levels.
	std::unordered_set<std::string> dedupHist {};

	bool seenBefore(LogLevel lvl, const std::string& formatted)
	{
		std::unique_lock lock(mutex);
		const std::string key =
			std::string(logLvlToStr(lvl)) + "|" + formatted;
		auto [_, inserted] = dedupHist.emplace(key);
		return !inserted;
	}

public:
	std::string path;

	CLog(const char* path);
	~CLog();

	template<typename ...Args>
	constexpr void once(const char* msg, Args... args)
	{
		__log(LogLevel::Once, msg, args...);
	}

	template<typename ...Args>
	constexpr void debug(const char* msg, Args... args)
	{
		__log(LogLevel::Debug, msg, args...);
	}

	template<typename ...Args>
	constexpr void info(const char* msg, Args... args)
	{
		__log(LogLevel::Info, msg, args...);
	}

	// Variants of debug/info that suppress repeated messages with
	// identical formatted text.  Use these for diagnostics that fire
	// per-app/per-depot/etc. — the first occurrence is logged, the
	// rest are squelched, so a hot loop doesn't drown the file.
	template<typename ...Args>
	void infoOnce(const char* msg, Args... args)
	{
		if (LogLevel::Info < getMinLevel()) return;
		size_t size = snprintf(nullptr, 0, msg, args...) + 1;
		std::string formatted;
		formatted.resize(size);
		snprintf(formatted.data(), size, msg, args...);
		if (seenBefore(LogLevel::Info, formatted)) return;
		__log(LogLevel::Info, "%s", formatted.c_str());
	}

	template<typename ...Args>
	void debugOnce(const char* msg, Args... args)
	{
		if (LogLevel::Debug < getMinLevel()) return;
		size_t size = snprintf(nullptr, 0, msg, args...) + 1;
		std::string formatted;
		formatted.resize(size);
		snprintf(formatted.data(), size, msg, args...);
		if (seenBefore(LogLevel::Debug, formatted)) return;
		__log(LogLevel::Debug, "%s", formatted.c_str());
	}

	template<typename ...Args>
	constexpr void notify(const char* msg, Args... args)
	{
		__log(LogLevel::NotifyShort, msg, args...);
	}

	template<typename ...Args>
	constexpr void notifyLong(const char* msg, Args... args)
	{
		__log(LogLevel::NotifyLong, msg, args...);
	}

	template<typename ...Args>
	constexpr void warn(const char* msg, Args... args)
	{
		__log(LogLevel::Warn, msg, args...);
	}

	// The ONLY desktop-popup path. Renders a friendly, localized message
	// from the usermsg.hpp catalog, throttled per-category so a multi-depot
	// burst collapses into a single popup. `detail` fills the catalog's
	// "{detail}" slot when present (e.g. "HTTP 503") and is omitted
	// otherwise. The raw developer diagnostic should still be logged
	// separately (warn/info) at the call site for debugging.
	void notifyUser(UserMsg msg, const std::string& detail = "");

	//Do not include config.hpp in this header, otherwise things will break :) (proly due to recursive inclusion)
	static LogLevel getMinLevel();
	static bool shouldNotify();
	static CLog* createDefaultLog();
};

extern std::unique_ptr<CLog> g_pLog;
