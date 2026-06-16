#include "log.hpp"

#include "config.hpp"

#include <chrono>
#include <cstdlib>
#include <memory>

using std::chrono::duration_cast;

CLog::CLog(const char* path) : path(path)
{
	// Append (not truncate) so a complete record survives even if the
	// logger is constructed more than once in a session.  This is also
	// a diagnostic safety net: truncation was hiding the init phase.
	ofstream = std::ofstream(path, std::ios::out | std::ios::app);
	if (!ofstream.is_open())
	{
		throw std::runtime_error("Unable to open logfile!");
	}

	// Resolve the popup language ONCE, from the environment Steam inherited.
	// Priority mirrors the C library: LC_ALL > LC_MESSAGES > LANG > LANGUAGE.
	// detectLang() maps "pt"/"pt_*" -> Portuguese and everything else (incl.
	// unset) -> English.
	const char* loc = getenv("LC_ALL");
	if (!loc || !loc[0]) loc = getenv("LC_MESSAGES");
	if (!loc || !loc[0]) loc = getenv("LANG");
	if (!loc || !loc[0]) loc = getenv("LANGUAGE");
	m_lang = detectLang(loc);
}

CLog::~CLog()
{
	if (ofstream.is_open())
	{
		ofstream.close();
	}
}

//Dirty workaround for not being able to access g_config from __log
LogLevel CLog::getMinLevel()
{
	return static_cast<LogLevel>(g_config.logLevel.get());
}

bool CLog::shouldNotify()
{
	return g_config.notifications.get();
}

void CLog::notifyUser(UserMsg msg, const std::string& detail)
{
	const UiMessage ui = messageFor(msg, m_lang);
	const std::string body = substituteDetail(ui.body, detail);

	// Always record that we surfaced this to the user — at info level, so it
	// lands in the file (never a popup) right next to the raw diagnostic the
	// call site logged. Helps correlate "what the user saw" with "what went
	// wrong" when reading ~/.SLSsteam.log.
	info("notifyUser: %s\n", body.c_str());

	// Gate exactly like the old popup path: respect the global Notifications
	// toggle and the per-thread suppression flag (the background pre-warm
	// worker sets it so its re-stage failures never reach the screen).
	if (!shouldNotify() || t_suppressNotify)
	{
		return;
	}

	// Severity -> notify-send timeout/urgency. Errors/warnings linger longer
	// than a success toast; urgency stays non-critical so the daemon honours
	// the expire timeout (see notify.hpp's resident-notification fix).
	int timeoutMs = 30000;
	if (ui.severity == Severity::Info)
	{
		timeoutMs = 10000;
	}

	// Throttle per category so a multi-depot/multi-DLC burst of the SAME
	// failure becomes ONE popup. Suppressed occurrences are counted and
	// noted in the log (not on screen) so the file still shows the scope.
	int suppressed = 0;
	bool emit = false;
	{
		const long long nowMs = duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		std::lock_guard<std::mutex> lk(m_notifyMu);
		emit = m_notifyThrottle.allow(static_cast<int>(msg), nowMs, suppressed);
	}

	if (!emit)
	{
		return;
	}
	if (suppressed > 0)
	{
		info("notifyUser: (%d earlier occurrence(s) of this message were "
		     "suppressed)\n", suppressed);
	}

	// Append the localized "(and N more suppressed errors)" tail so the user
	// sees the scope of a burst in the SAME popup, not a separate one.
	std::string shown = body;
	const std::string suffix = suppressedSuffix(m_lang, suppressed);
	if (!suffix.empty())
	{
		shown += " " + suffix;
	}

	const std::string cmd =
		Notify::buildCommandRaw("SLSsteam-moon", shown, timeoutMs, "normal");
	const int rc = system(cmd.c_str());
	(void)rc; // best-effort popup; nothing actionable if notify-send is absent
}

CLog* CLog::createDefaultLog()
{
	const char* home = getenv("HOME");
	if (home)
	{
		std::stringstream ss;
		ss << home << "/.SLSsteam.log";

		return new CLog(ss.str().c_str());
	}

	return nullptr;
}

std::unique_ptr<CLog> g_pLog;
