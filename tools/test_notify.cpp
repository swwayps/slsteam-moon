// Standalone test for the desktop-notification command builder.
//
// Bug (HANDOFF 2026-06-05): every SLSsteam notify-send popup EXCEPT the
// "Loaded successfully" (NotifyShort) one stays on screen forever.  Root
// cause: the Warn branch emitted `notify-send -u "critical"` with NO `-t`.
// Per the freedesktop spec, urgency=critical notifications are "resident"
// and most daemons (GNOME/Zorin) IGNORE the expire timeout for them, so
// warn-level popups never auto-dismiss.
//
// Fix: a pure mapping (Notify::specForLevel) gives every notifying level an
// explicit timeout and a non-critical urgency, and Notify::buildCommand
// renders the notify-send command (escaping the body so a stray `"` / `$`
// / backtick in a log message can't break or inject into the shell).
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_notify.cpp -o /tmp/test_notify && /tmp/test_notify

#include "../src/notify.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool contains(const std::string& hay, const std::string& needle)
{
	return hay.find(needle) != std::string::npos;
}

int main()
{
	// 1) THE FIX: Warn must auto-dismiss -> enabled, a positive timeout, and
	//    a NON-critical urgency (critical is what makes daemons keep it
	//    resident regardless of -t).
	{
		const Notify::Spec s = Notify::specForLevel(LogLevel::Warn);
		CHECK(s.enabled, "Warn produces a desktop notification");
		CHECK(s.timeoutMs > 0, "Warn has an explicit expire timeout");
		CHECK(std::string(s.urgency) != "critical",
		      "Warn urgency is not critical (so it auto-dismisses)");
	}

	// 2) NotifyShort/NotifyLong keep their existing, working behaviour.
	{
		const Notify::Spec sShort = Notify::specForLevel(LogLevel::NotifyShort);
		CHECK(sShort.enabled && sShort.timeoutMs == 10000 &&
		      std::string(sShort.urgency) == "normal",
		      "NotifyShort = {enabled, 10000ms, normal}");

		const Notify::Spec sLong = Notify::specForLevel(LogLevel::NotifyLong);
		CHECK(sLong.enabled && sLong.timeoutMs == 30000 &&
		      std::string(sLong.urgency) == "normal",
		      "NotifyLong = {enabled, 30000ms, normal}");
	}

	// 3) Non-notifying levels never raise a popup.
	{
		CHECK(!Notify::specForLevel(LogLevel::Once).enabled,  "Once does not notify");
		CHECK(!Notify::specForLevel(LogLevel::Debug).enabled, "Debug does not notify");
		CHECK(!Notify::specForLevel(LogLevel::Info).enabled,  "Info does not notify");
		CHECK(!Notify::specForLevel(LogLevel::None).enabled,  "None does not notify");
	}

	// 4) buildCommand: notifying levels render a notify-send with -t and -u.
	{
		const std::string warnCmd = Notify::buildCommand(LogLevel::Warn, "boom");
		CHECK(contains(warnCmd, "notify-send"), "Warn cmd invokes notify-send");
		CHECK(contains(warnCmd, "-t "), "Warn cmd carries an explicit -t timeout");
		CHECK(!contains(warnCmd, "critical"), "Warn cmd is not critical urgency");
		CHECK(contains(warnCmd, "boom"), "Warn cmd carries the message body");

		const std::string shortCmd = Notify::buildCommand(LogLevel::NotifyShort, "hi");
		CHECK(contains(shortCmd, "-t 10000"), "NotifyShort cmd uses 10000ms");
	}

	// 5) Non-notifying levels build an empty command (caller skips system()).
	{
		CHECK(Notify::buildCommand(LogLevel::Info, "x").empty(),
		      "Info builds no command");
		CHECK(Notify::buildCommand(LogLevel::Debug, "x").empty(),
		      "Debug builds no command");
	}

	// 6) The body is escaped so a stray quote / shell metachar can't break
	//    or inject into the system() command (fixes the old TODO).
	{
		const std::string cmd = Notify::buildCommand(LogLevel::Warn, "a\"b");
		CHECK(contains(cmd, "a\\\"b"), "double-quote in body is backslash-escaped");

		const std::string inj = Notify::buildCommand(LogLevel::Warn, "x`id`$(id)");
		CHECK(contains(inj, "\\`id\\`"), "backticks in body are escaped");
		CHECK(contains(inj, "\\$(id)"), "dollar in body is escaped");
	}

	// 7) REGRESSION: __log builds `formatted` with snprintf(size)+1 then
	//    resize(size), leaving an embedded trailing '\0' in the std::string.
	//    The command must NOT carry that null into the middle of the shell
	//    word, or system()'s c_str() truncates it -> the closing quote is
	//    lost -> shell syntax error -> notification never appears. (This is
	//    exactly the real-Steam failure the unit literals didn't reproduce.)
	{
		std::string withNull = "Loaded successfully";
		withNull.push_back('\0'); // mimic the resize(strlen+1) artifact
		const std::string cmd = Notify::buildCommand(LogLevel::NotifyShort, withNull);
		// What system() actually executes is cmd.c_str() (stops at \0).
		const std::string asExecuted(cmd.c_str());
		CHECK(asExecuted == cmd,
		      "command carries no embedded null (would truncate in system())");
		CHECK(!asExecuted.empty() && asExecuted.back() == '"',
		      "command ends with the closing quote (not truncated mid-word)");
	}

	// 8) shouldRaiseNotification gates the popup on THREE facts: the level
	//    notifies, the user enabled notifications, and the current thread is
	//    not suppressing popups.  The background pre-warm worker re-stages
	//    purged DLC manifests every pass; a genuinely-gone depot warns each
	//    time, which used to spam a popup per depot per pass.  Suppressing
	//    popups on that ONE worker thread silences the spam while keeping the
	//    warning visible in the log AND keeping popups on the install path
	//    (where a failure is user-actionable).
	{
		using Notify::shouldRaiseNotification;

		// Warn notifies when enabled and not suppressed.
		CHECK(shouldRaiseNotification(LogLevel::Warn, /*enabled=*/true, /*suppressed=*/false),
		      "Warn pops up when enabled and not suppressed");

		// Suppressed thread: no popup even for a notifying level.
		CHECK(!shouldRaiseNotification(LogLevel::Warn, true, /*suppressed=*/true),
		      "Warn does NOT pop up on a suppressed thread");
		CHECK(!shouldRaiseNotification(LogLevel::NotifyShort, true, true),
		      "NotifyShort does NOT pop up on a suppressed thread");

		// Notifications globally disabled: never pop up.
		CHECK(!shouldRaiseNotification(LogLevel::Warn, /*enabled=*/false, false),
		      "Warn does NOT pop up when notifications are disabled");

		// Non-notifying levels never pop up regardless of the flags.
		CHECK(!shouldRaiseNotification(LogLevel::Info, true, false),
		      "Info never pops up");
		CHECK(!shouldRaiseNotification(LogLevel::Debug, true, false),
		      "Debug never pops up");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
