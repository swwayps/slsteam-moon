// TDD regression test for the narrowed rtld-audit object policy.

#include "audit_log.hpp"
#include "audit_policy.hpp"

#include <algorithm>
#include <climits>
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

	check(flagsForObject("/lib/x86_64-linux-gnu/libc.so.6", false) == LA_FLG_BINDTO,
	      "libc is a binding target");
	check(flagsForObject("/lib/x86_64-linux-gnu/libpthread.so.0", false) == LA_FLG_BINDTO,
	      "libpthread is a binding target");
	check(flagsForObject("/steam/steamrt64/crashhandler.so", false) == LA_FLG_BINDFROM,
	      "crashhandler is a binding source");
	check(flagsForObject("/lib/x86_64-linux-gnu/libglib-2.0.so.0", false) == LA_FLG_BINDFROM,
	      "glib is a binding source");
	check(flagsForObject("/steam/steamrt64/steamui.so", false) == LA_FLG_BINDFROM,
	      "steamui is a binding source");
	check(flagsForObject("/steam/steamrt64/steamclient.so", false) == LA_FLG_BINDFROM,
	      "steamclient remains a binding source");
	check(flagsForObject("/steam/steamrt64/steamservice.so", false) == LA_FLG_BINDFROM,
	      "steamservice is a binding source");
	check(flagsForObject("/steam/steamrt64/libSDL3.so.0", false) == LA_FLG_BINDFROM,
	      "SDL3 is a binding source");
	check(flagsForObject("/steam/steamrt64/libvideo.so", false) == LA_FLG_BINDFROM,
	      "video is a binding source");
	check(flagsForObject("/steam/steamrt64/gameoverlayrenderer.so", false) == LA_FLG_BINDFROM,
	      "gameoverlayrenderer is a binding source");
	check(flagsForObject("/steam/steamrt64/steam_monitor", false) == LA_FLG_BINDFROM,
	      "steam_monitor is a binding source");
	check(flagsForObject("/steam/steamrt64/libtier0_s.so", false) == LA_FLG_BINDFROM,
	      "tier0 is a binding source");
	check(flagsForObject("", false) == LA_FLG_BINDFROM,
	      "main executable is a binding source");
	check(flagsForObject("/lib/x86_64-linux-gnu/libm.so.6", false) == 0,
	      "unrelated object is not audited");
	check(flagsForObject(nullptr, false) == 0,
	      "unknown object is not audited");

	check(flagsForObject("/lib/x86_64-linux-gnu/libm.so.6", true) == both,
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

	// Steam's runtime pins its own libcurl ahead of the host copy; client
	// namespace lookups (CloudRedirect) are sent to the host's 32-bit copy.
	{
		using AuditBinding::systemLibraryFor;
		char root[] = "/tmp/slssteam-libdirs-XXXXXX";
		check(mkdtemp(root) != nullptr, "temporary library root can be created");
		const std::string lib64 = std::string(root) + "/lib";
		const std::string lib32 = std::string(root) + "/lib32";
		std::filesystem::create_directories(lib64);
		std::filesystem::create_directories(lib32);
		const auto writeElf = [](const std::string& path, unsigned char elfClass)
		{
			const unsigned char ident[EI_NIDENT] = {ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, elfClass};
			std::ofstream(path, std::ios::binary)
				.write(reinterpret_cast<const char*>(ident), sizeof ident);
		};
		writeElf(lib64 + "/libcurl.so.4", ELFCLASS64);
		writeElf(lib32 + "/libcurl.so.4.8.0", ELFCLASS32);
		std::filesystem::create_symlink("libcurl.so.4.8.0", lib32 + "/libcurl.so.4");

		const char* const dirs[] = {lib64.c_str(), lib32.c_str()};
		const char* const only64[] = {lib64.c_str()};
		char buffer[PATH_MAX];
		const char* name = "libcurl.so.4";
		const char* found = systemLibraryFor(name, dirs, buffer);
		check(found != nullptr && found == lib32 + "/libcurl.so.4",
		      "libcurl resolves to the first 32-bit system copy");
		check(systemLibraryFor(name, only64, buffer) == name,
		      "a 64-bit copy is never offered to the 32-bit loader");
		const char* other = "libz.so.1";
		check(systemLibraryFor(other, dirs, buffer) == other,
		      "other libraries are left to the loader");
		const char* gnutls = "libcurl-gnutls.so.4";
		check(systemLibraryFor(gnutls, dirs, buffer) == gnutls,
		      "only the libcurl.so names are redirected");
		const char* missing = "libcurl.so.3";
		check(systemLibraryFor(missing, dirs, buffer) == missing,
		      "a soname without a system copy is left to the loader");
		char tiny[8];
		check(systemLibraryFor(name, dirs, tiny) == name,
		      "a path that does not fit the buffer is not truncated");
		check(systemLibraryFor(nullptr, dirs, buffer) == nullptr,
		      "a null name is passed through");
		std::filesystem::remove_all(root);
	}

	std::ifstream mainSource("src/main.cpp");
	const std::string mainText(
		(std::istreambuf_iterator<char>(mainSource)),
		std::istreambuf_iterator<char>());
	const auto rawLogOpen = mainText.find("AuditLog::reopen(g_rawLog,");
	const auto setupLock = mainText.find("g_setupLock = std::make_unique");
	check(mainSource.is_open() && rawLogOpen != std::string::npos
	          && setupLock != std::string::npos && rawLogOpen < setupLock,
	      "raw audit log fd opens before the namespace setup lock");
	// The 64-bit client runs inside pressure-vessel, where the host's libcurl
	// is not at the redirected paths: lookups stay with the loader.
	check(mainText.find("extern \"C\" char* la_objsearch(") != std::string::npos
	          && mainText.find("AuditBinding::systemLibraryFor(") == std::string::npos,
	      "library lookups are left to the loader");

	const auto rawLogPath = mainText.find("g_rawLogPath");
	std::ifstream auditLogSource("src/audit_log.hpp");
	const std::string auditLogText(
		(std::istreambuf_iterator<char>(auditLogSource)),
		std::istreambuf_iterator<char>());
	const auto rawLogReopen = auditLogText.find("errno == EBADF");
	check(rawLogPath != std::string::npos && auditLogSource.is_open()
	          && rawLogReopen != std::string::npos,
	      "raw audit log reopens after Steam closes inherited descriptors");

	// The audit module can run on Steam-owned threads whose locale TLS does
	// not contain the audit namespace's glibc ctype table. Keep byte parsing
	// on the locale-free Ascii helpers throughout src/, not only in the parser
	// that first exposed the crash.
	const std::vector<std::string> forbiddenCtype = {
		"#include<cctype>", "#include<ctype.h>",
		"std::isalnum(", "std::isalpha(", "std::isdigit(",
		"std::isspace(", "std::isxdigit(", "std::tolower(",
		"std::toupper(", "std::isprint(", "std::isgraph(",
		"std::ispunct(", "std::iscntrl(", "std::isblank(",
		"std::isupper(", "std::islower(",
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
	const int created = mkstemp(logPath);
	check(created >= 0, "temporary raw audit log can be created");
	const auto readAll = [](const char* path)
	{
		std::ifstream file(path);
		return std::string((std::istreambuf_iterator<char>(file)),
		                   std::istreambuf_iterator<char>());
	};
	if (created >= 0)
	{
		close(created);
		AuditLog::Sink sink;
		check(AuditLog::reopen(sink, logPath), "raw audit log opens");
		AuditLog::write(sink, logPath, "before-close\\n");
		close(sink.fd); // Simulate Steam closing inherited descriptors in the child.
		AuditLog::write(sink, logPath, "after-reopen\\n");
		check(readAll(logPath) == "before-close\\nafter-reopen\\n",
		      "raw audit log reopens a closed descriptor and preserves the message");

		// The fork child may hand the number to another file before the exec
		// wrapper writes: the line must still go to the log.
		char otherPath[] = "/tmp/slssteam-audit-other-XXXXXX";
		const int other = mkstemp(otherPath);
		close(other);
		const int number = sink.fd;
		close(number);
		const int reused = open(otherPath, O_WRONLY | O_APPEND);
		const bool sameNumber = reused == number;
		AuditLog::write(sink, logPath, "after-reuse\\n");
		check(sameNumber && readAll(logPath) == "before-close\\nafter-reopen\\nafter-reuse\\n" &&
		          readAll(otherPath).empty(),
		      "raw audit log does not write through a number another file now holds");
		if (reused >= 0) close(reused);
		if (sink.fd >= 0) close(sink.fd);
		std::remove(otherPath);
		std::remove(logPath);
	}

	// Upstream abafc49: Steam's crash handler is never loaded.
	check(AuditBinding::isRefusedObject("/steam/steamrt64/crashhandler.so"),
	      "the crash handler is refused");
	check(AuditBinding::isRefusedObject("crashhandler.so"),
	      "a bare crash handler name is refused");
	check(!AuditBinding::isRefusedObject("/steam/steamrt64/steamclient.so"),
	      "steamclient is not refused");
	check(!AuditBinding::isRefusedObject(nullptr),
	      "a missing name is not refused");
	if (failures != 0)
	{
		std::fprintf(stderr, "test_audit_policy: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("audit policy tests passed");
	return 0;
}
