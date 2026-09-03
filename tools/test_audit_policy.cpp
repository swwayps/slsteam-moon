// TDD regression test for the narrowed rtld-audit object policy.

#include "audit_log.hpp"
#include "audit_policy.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <link.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}
}

int main()
{
	using AuditBinding::flagsForObject;
	const unsigned int both = LA_FLG_BINDFROM | LA_FLG_BINDTO;
	check(flagsForObject("/plugins/cloud_redirect.so", false) == both,
	      "stats bridge is visible in narrowed audit mode");

	check(flagsForObject("/lib/i386-linux-gnu/libc.so.6", false) == LA_FLG_BINDTO,
	      "libc is a binding target");
	check(flagsForObject("/lib/i386-linux-gnu/libpthread.so.0", false) == LA_FLG_BINDTO,
	      "libpthread is a binding target");
	check(flagsForObject("/steam/ubuntu12_32/crashhandler.so", false) == LA_FLG_BINDFROM,
	      "crashhandler is a binding source");
	check(flagsForObject("/lib/i386-linux-gnu/libglib-2.0.so.0", false) == LA_FLG_BINDFROM,
	      "glib is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/steamui.so", false) == LA_FLG_BINDFROM,
	      "steamui is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/steamclient.so", false) == LA_FLG_BINDFROM,
	      "steamclient remains a binding source");
	check(flagsForObject("/steam/ubuntu12_32/steamservice.so", false) == LA_FLG_BINDFROM,
	      "steamservice is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/libSDL3.so.0", false) == LA_FLG_BINDFROM,
	      "SDL3 is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/libvideo.so", false) == LA_FLG_BINDFROM,
	      "video is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/gameoverlayrenderer.so", false) == LA_FLG_BINDFROM,
	      "gameoverlayrenderer is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/steam_monitor", false) == LA_FLG_BINDFROM,
	      "steam_monitor is a binding source");
	check(flagsForObject("/steam/ubuntu12_32/libtier0_s.so", false) == LA_FLG_BINDFROM,
	      "tier0 is a binding source");
	check(flagsForObject("", false) == LA_FLG_BINDFROM,
	      "main executable is a binding source");
	check(flagsForObject("/lib/i386-linux-gnu/libm.so.6", false) == 0,
	      "unrelated object is not audited");
	check(flagsForObject(nullptr, false) == 0,
	      "unknown object is not audited");

	check(flagsForObject("/lib/i386-linux-gnu/libm.so.6", true) == both,
	      "bind-all override restores both flags");
	check(AuditBinding::bindAllValueEnabled("1"), "bind-all accepts 1");
	check(!AuditBinding::bindAllValueEnabled("0"), "bind-all rejects 0");
	check(!AuditBinding::bindAllValueEnabled(nullptr), "bind-all rejects unset");
	check(!AuditBinding::narrowValueEnabled(nullptr),
	      "audit narrowing is disabled by default");
	check(AuditBinding::narrowValueEnabled("1"),
	      "audit narrowing requires an explicit opt-in");
	check(!AuditBinding::narrowValueEnabled("0"),
	      "audit narrowing rejects disabled values");

	std::ifstream mainSource("src/main.cpp");
	const std::string mainText(
		(std::istreambuf_iterator<char>(mainSource)),
		std::istreambuf_iterator<char>());
	const auto rawLogOpen = mainText.find("g_rawLogFd = open");
	const auto setupLock = mainText.find("g_setupLock = std::make_unique");
	check(mainSource.is_open() && rawLogOpen != std::string::npos
	          && setupLock != std::string::npos && rawLogOpen < setupLock,
	      "raw audit log fd opens before the namespace setup lock");

	const auto rawLogPath = mainText.find("g_rawLogPath");
	std::ifstream auditLogSource("src/audit_log.hpp");
	const std::string auditLogText(
		(std::istreambuf_iterator<char>(auditLogSource)),
		std::istreambuf_iterator<char>());
	const auto rawLogReopen = auditLogText.find("errno == EBADF");
	check(rawLogPath != std::string::npos && auditLogSource.is_open()
	          && rawLogReopen != std::string::npos,
	      "raw audit log reopens after Steam closes inherited descriptors");

	const auto policyReady = mainText.find("g_auditPolicyReady.store(true");
	check(policyReady != std::string::npos && setupLock != std::string::npos
	          && policyReady < setupLock,
	      "narrow audit policy is published before a secondary namespace can skip setup");

	// The audit module can run on Steam-owned threads whose locale TLS does
	// not contain the audit namespace's glibc ctype table. Keep byte parsing
	// on the locale-free Ascii helpers throughout src/, not only in the parser
	// that first exposed the crash.
	const std::vector<std::string> forbiddenCtype = {
		"#include<cctype>", "#include<ctype.h>",
		"std::isalnum(", "std::isalpha(", "std::isdigit(",
		"std::isspace(", "std::isxdigit(", "std::tolower(",
		"std::toupper(",
	};
	bool auditSourcesAreLocaleFree = true;
	for (const auto& entry : std::filesystem::recursive_directory_iterator("src"))
	{
		if (!entry.is_regular_file()) continue;
		const auto extension = entry.path().extension();
		if (extension != ".cpp" && extension != ".hpp" && extension != ".h")
			continue;
		std::ifstream source(entry.path());
		std::string text(
			(std::istreambuf_iterator<char>(source)),
			std::istreambuf_iterator<char>());
		text.erase(std::remove_if(text.begin(), text.end(), [](char value)
		{
			return value == ' ' || value == '\t' || value == '\n' ||
				value == '\v' || value == '\f' || value == '\r';
		}), text.end());
		for (const auto& token : forbiddenCtype)
		{
			if (text.find(token) == std::string::npos) continue;
			std::fprintf(stderr, "unsafe audit ctype dependency: %s (%s)\n",
				entry.path().c_str(), token.c_str());
			auditSourcesAreLocaleFree = false;
		}
	}
	check(auditSourcesAreLocaleFree,
	      "audit sources avoid locale-dependent libc ctype calls");

	char logPath[] = "/tmp/slssteam-audit-log-XXXXXX";
	int logFd = mkstemp(logPath);
	check(logFd >= 0, "temporary raw audit log can be created");
	if (logFd >= 0)
	{
		AuditLog::write(logFd, logPath, "before-close\\n");
		close(logFd); // Simulate Steam closing inherited descriptors in the child.
		AuditLog::write(logFd, logPath, "after-reopen\\n");

		std::ifstream logFile(logPath);
		const std::string contents(
			(std::istreambuf_iterator<char>(logFile)),
			std::istreambuf_iterator<char>());
		check(contents == "before-close\\nafter-reopen\\n",
		      "raw audit log reopens an EBADF descriptor and preserves the message");
		if (logFd >= 0) close(logFd);
		std::remove(logPath);
	}

	if (failures != 0)
	{
		std::fprintf(stderr, "test_audit_policy: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("audit policy tests passed");
	return 0;
}
