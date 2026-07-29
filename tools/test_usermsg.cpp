// Standalone test for the user-facing message catalog (usermsg.hpp).
//
// Goal: every popup the end user can ever see is a friendly, localized
// sentence drawn from ONE catalog — never a raw developer diagnostic like
// "ManifestFetch: blob depot=445701 gid=... all CDN hosts failed (HTTP=503)".
// The catalog is pure (no openssl / config / logger) so it is unit-testable
// in isolation, exactly like notify.hpp.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_usermsg.cpp -o /tmp/test_usermsg && /tmp/test_usermsg

#include "../src/usermsg.hpp"

#include <cstdio>
#include <cstring>
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

// Every catalog entry we expect to surface to the user.
static const UserMsg kAll[] = {
	UserMsg::LoadSuccess,
	UserMsg::BirthdayGreeting,
	UserMsg::SteamVersionUnsupported,
	UserMsg::SteamVersionMismatch,
	UserMsg::InitializationFailed,
	UserMsg::ContentServersUnavailable,
	UserMsg::DownloadAuthUnavailable,
	UserMsg::DownloadTimedOut,
	UserMsg::GamePreparationFailed,
	UserMsg::DrmRemovalFailed,
	UserMsg::LocalStorageError,
	UserMsg::ConfigUnreadable,
	UserMsg::ConfigParseFailed,
	UserMsg::ConfigRepaired,
	UserMsg::ConfigWriteFailed,
};

int main()
{
	// 1) Language detection from an environment string (LANG / LC_MESSAGES
	//    / LANGUAGE shape).  pt* -> Portuguese; anything else / empty / null
	//    -> English fallback.
	{
		CHECK(detectLang("pt_BR.UTF-8") == Lang::Portuguese, "pt_BR -> Portuguese");
		CHECK(detectLang("pt_PT")       == Lang::Portuguese, "pt_PT -> Portuguese");
		CHECK(detectLang("pt")          == Lang::Portuguese, "pt -> Portuguese");
		CHECK(detectLang("en_US.UTF-8") == Lang::English,    "en_US -> English");
		CHECK(detectLang("de_DE")       == Lang::English,    "de_DE -> English (fallback)");
		CHECK(detectLang("")            == Lang::English,    "empty -> English");
		CHECK(detectLang(nullptr)       == Lang::English,    "null -> English");
		// LANGUAGE can be a colon list ("pt_BR:en"); first wins.
		CHECK(detectLang("pt_BR:en")    == Lang::Portuguese, "colon list, pt first -> Portuguese");
	}

	// 2) Every catalog entry has a non-empty body in BOTH languages, and
	//    NONE of them leaks a raw developer token (no "depot=", "gid=",
	//    "ManifestFetch", "%", "\\n").
	{
		for (UserMsg m : kAll)
		{
			const UiMessage en = messageFor(m, Lang::English);
			const UiMessage pt = messageFor(m, Lang::Portuguese);
			CHECK(en.body && en.body[0] != '\0', "English body present");
			CHECK(pt.body && pt.body[0] != '\0', "Portuguese body present");
			CHECK(!contains(en.body, "depot=") && !contains(en.body, "gid=") &&
			      !contains(en.body, "ManifestFetch"),
			      "English body carries no raw developer token");
			CHECK(!contains(pt.body, "depot=") && !contains(pt.body, "gid=") &&
			      !contains(pt.body, "ManifestFetch"),
			      "Portuguese body carries no raw developer token");
		}
	}

	// 3) The two languages actually differ for a representative error (so a
	//    Portuguese user doesn't silently get English).
	{
		const UiMessage en = messageFor(UserMsg::ContentServersUnavailable, Lang::English);
		const UiMessage pt = messageFor(UserMsg::ContentServersUnavailable, Lang::Portuguese);
		CHECK(std::strcmp(en.body, pt.body) != 0,
		      "ContentServersUnavailable differs EN vs PT");
	}

	// 4) Detail substitution: a body template carrying the {detail} sentinel
	//    gets the runtime detail spliced in; an empty detail removes the
	//    sentinel cleanly (no dangling "{detail}" or doubled spaces).
	{
		CHECK(substituteDetail("server said {detail}.", "HTTP 503") ==
		      "server said HTTP 503.",
		      "{detail} replaced with the runtime value");
		CHECK(substituteDetail("no placeholder here", "HTTP 503") ==
		      "no placeholder here",
		      "template without sentinel is unchanged");
		CHECK(!contains(substituteDetail("code {detail}", ""), "{detail}"),
		      "empty detail leaves no dangling sentinel");
	}

	// 5) The HTTP-carrying message actually uses the sentinel, so the code
	//    can be shown ("HTTP 503") without baking it into the catalog.
	{
		const UiMessage en = messageFor(UserMsg::ContentServersUnavailable, Lang::English);
		CHECK(contains(en.body, "{detail}"),
		      "ContentServersUnavailable body has a {detail} slot for the HTTP code");
	}

	// A preparation failure caused by incomplete install data is not repaired
	// by restarting Steam. Point to the corrective action and identify the app.
	{
		const UiMessage en = messageFor(UserMsg::GamePreparationFailed, Lang::English);
		const UiMessage pt = messageFor(UserMsg::GamePreparationFailed, Lang::Portuguese);
		CHECK(contains(en.body, "{detail}") && contains(pt.body, "{detail}"),
		      "GamePreparationFailed identifies the AppID");
		CHECK(contains(en.body, "LuaTools") && contains(pt.body, "LuaTools"),
		      "GamePreparationFailed points to re-adding through LuaTools");
		CHECK(!contains(en.body, "restart") && !contains(pt.body, "Reinicie") &&
		      !contains(pt.body, "reinicie"),
		      "GamePreparationFailed does not recommend restarting Steam");
	}

	// Network loss is not incomplete local metadata and re-adding the title
	// cannot repair it. Point to connectivity/restart and identify the app.
	{
		const UiMessage en = messageFor(UserMsg::GameMetadataUnavailable, Lang::English);
		const UiMessage pt = messageFor(UserMsg::GameMetadataUnavailable, Lang::Portuguese);
		CHECK(contains(en.body, "{detail}") && contains(pt.body, "{detail}"),
		      "GameMetadataUnavailable identifies the AppID");
		CHECK(contains(en.body, "connection") && contains(pt.body, "conex"),
		      "GameMetadataUnavailable explains the connectivity problem");
		CHECK(contains(en.body, "restart Steam") && contains(pt.body, "reinicie a Steam"),
		      "GameMetadataUnavailable explains how to refresh after reconnecting");
		CHECK(!contains(en.body, "Re-add") && !contains(pt.body, "Adicione"),
		      "GameMetadataUnavailable never recommends re-adding the title");
	}

	// 6) Severity is exposed so the popup layer can pick a timeout/urgency:
	//    a success is Info, a hard failure is Error.
	{
		CHECK(messageFor(UserMsg::LoadSuccess, Lang::English).severity == Severity::Info,
		      "LoadSuccess is Info severity");
		CHECK(messageFor(UserMsg::ContentServersUnavailable, Lang::English).severity == Severity::Error,
		      "ContentServersUnavailable is Error severity");
	}

	// 7) Suppressed-count suffix: when the throttle hid N repeats of the same
	//    error during the cooldown, the next shown popup carries a short,
	//    localized "(and N more suppressed errors)" tail. N==0 -> empty (no
	//    tail). Singular vs plural is handled per language.
	{
		CHECK(suppressedSuffix(Lang::English, 0).empty(),
		      "EN: zero suppressed -> no suffix");
		CHECK(suppressedSuffix(Lang::Portuguese, 0).empty(),
		      "PT: zero suppressed -> no suffix");

		CHECK(suppressedSuffix(Lang::English, 1) == "(and 1 more suppressed error)",
		      "EN: 1 suppressed -> singular");
		CHECK(suppressedSuffix(Lang::English, 3) == "(and 3 more suppressed errors)",
		      "EN: 3 suppressed -> plural");

		CHECK(suppressedSuffix(Lang::Portuguese, 1) == "(e mais 1 erro suprimido)",
		      "PT: 1 suppressed -> singular");
		CHECK(suppressedSuffix(Lang::Portuguese, 3) == "(e mais 3 erros suprimidos)",
		      "PT: 3 suppressed -> plural");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
