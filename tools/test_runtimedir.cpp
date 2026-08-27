// Standalone test for RuntimeDir: where SLSsteam keeps its runtime contracts.
//
// The API command file lived at /tmp/SLSsteam.API — a fixed name in a
// world-writable directory, with no access control, driving app installs. The
// per-process load lock lived at /tmp/.slssteam.load.<pid>, opened without
// O_EXCL or O_NOFOLLOW; a local process that pre-created it and held an flock
// made our own lock attempt fail, and the hooking pass was then skipped, which
// turned a lock file into an injection kill switch.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_runtimedir.cpp -o /tmp/test_runtimedir \
//     && /tmp/test_runtimedir
#include "../src/runtimedir.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            std::printf("ok   %s\n", (msg));                                   \
        } else {                                                              \
            std::printf("FAIL %s\n", (msg));                                   \
            g_failures++;                                                      \
        }                                                                     \
    } while (0)

int main()
{
	const uid_t me = geteuid();

	// ── base selection ──────────────────────────────────────────────────────
	CHECK(RuntimeDir::resolveBase("/run/user/1000", "/home/u")
	          == "/run/user/1000/SLSsteam",
	      "XDG_RUNTIME_DIR is preferred");
	CHECK(RuntimeDir::resolveBase(nullptr, "/home/u")
	          == "/home/u/.cache/SLSsteam",
	      "HOME is the fallback when there is no runtime dir");
	CHECK(RuntimeDir::resolveBase("", "/home/u") == "/home/u/.cache/SLSsteam",
	      "an empty runtime dir falls back to HOME");
	// A relative value cannot be trusted to be the user's own directory.
	CHECK(RuntimeDir::resolveBase("run/user/1000", "/home/u")
	          == "/home/u/.cache/SLSsteam",
	      "a relative runtime dir is ignored");
	CHECK(RuntimeDir::resolveBase(nullptr, "u") == "",
	      "a relative HOME yields no base");
	CHECK(RuntimeDir::resolveBase(nullptr, nullptr) == "",
	      "no inputs yield no base");
	// The point of the change: /tmp is never chosen on its own.
	CHECK(RuntimeDir::resolveBase(nullptr, nullptr).find("/tmp")
	          == std::string::npos,
	      "the shared temp directory is never a base");

	// ── mode predicates ─────────────────────────────────────────────────────
	CHECK(RuntimeDir::isPrivateDir(S_IFDIR | 0700, me, me),
	      "a 0700 directory we own is private");
	CHECK(!RuntimeDir::isPrivateDir(S_IFDIR | 0750, me, me),
	      "a group-readable directory is not private");
	CHECK(!RuntimeDir::isPrivateDir(S_IFDIR | 0777, me, me),
	      "a world-writable directory is not private");
	CHECK(!RuntimeDir::isPrivateDir(S_IFDIR | 0700, me + 1, me),
	      "a directory owned by someone else is not private");
	CHECK(!RuntimeDir::isPrivateDir(S_IFREG | 0600, me, me),
	      "a regular file is not a private directory");
	CHECK(!RuntimeDir::isPrivateDir(S_IFLNK | 0777, me, me),
	      "a symlink is not a private directory");

	CHECK(RuntimeDir::isPrivateFile(S_IFREG | 0600, me, me),
	      "a 0600 regular file we own is private");
	CHECK(!RuntimeDir::isPrivateFile(S_IFREG | 0644, me, me),
	      "a world-readable file is not private");
	CHECK(!RuntimeDir::isPrivateFile(S_IFREG | 0660, me, me),
	      "a group-writable file is not private");
	CHECK(!RuntimeDir::isPrivateFile(S_IFREG | 0600, me + 1, me),
	      "a file owned by someone else is not private");
	CHECK(!RuntimeDir::isPrivateFile(S_IFDIR | 0700, me, me),
	      "a directory is not a private file");
	CHECK(!RuntimeDir::isPrivateFile(S_IFIFO | 0600, me, me),
	      "a fifo is not a private file");

	// ── ensureDir against a real filesystem ─────────────────────────────────
	char tmpl[] = "/tmp/slssteam-rtd-XXXXXX";
	const char* sandbox = mkdtemp(tmpl);
	if (sandbox == nullptr)
	{
		std::printf("FAIL could not create a sandbox\n");
		return 1;
	}

	{
		const std::string dir = std::string(sandbox) + "/fresh";
		CHECK(RuntimeDir::ensureDir(dir), "creates a missing directory");
		struct stat st{};
		CHECK(lstat(dir.c_str(), &st) == 0
		          && (st.st_mode & 07777) == 0700,
		      "the created directory is 0700");
		CHECK(RuntimeDir::ensureDir(dir), "an existing private directory is accepted");
	}

	{
		// A directory left open to everyone must be refused, not reused: files
		// already inside it may have been planted.
		const std::string dir = std::string(sandbox) + "/open";
		mkdir(dir.c_str(), 0777);
		chmod(dir.c_str(), 0777);
		CHECK(!RuntimeDir::ensureDir(dir), "a world-writable directory is refused");
	}

	{
		// A symlink standing in for the directory is refused (lstat, not stat).
		const std::string target = std::string(sandbox) + "/link-target";
		const std::string link = std::string(sandbox) + "/link";
		mkdir(target.c_str(), 0700);
		CHECK(symlink(target.c_str(), link.c_str()) == 0, "sandbox symlink created");
		CHECK(!RuntimeDir::ensureDir(link), "a symlinked directory is refused");
	}

	{
		CHECK(!RuntimeDir::ensureDir(""), "an empty path is refused");
	}

	// ── descriptorIsPrivate ─────────────────────────────────────────────────
	{
		const std::string path = std::string(sandbox) + "/priv";
		const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
		CHECK(fd >= 0, "sandbox private file created");
		CHECK(RuntimeDir::descriptorIsPrivate(fd), "a 0600 file we own passes");
		CHECK(fchmod(fd, 0644) == 0, "sandbox file widened");
		CHECK(!RuntimeDir::descriptorIsPrivate(fd),
		      "the same file fails once it is world-readable");
		close(fd);
	}
	{
		CHECK(!RuntimeDir::descriptorIsPrivate(-1), "a closed descriptor fails");
	}
	{
		// A directory descriptor is not a usable contract file.
		const int fd = open(sandbox, O_RDONLY | O_DIRECTORY);
		CHECK(fd >= 0, "sandbox directory opened");
		CHECK(!RuntimeDir::descriptorIsPrivate(fd), "a directory descriptor fails");
		close(fd);
	}

	// Clean up the sandbox.
	std::string rm = "rm -rf '";
	rm += sandbox;
	rm += "'";
	if (system(rm.c_str()) != 0)
	{
		std::printf("note: sandbox cleanup returned non-zero\n");
	}

	if (g_failures == 0)
	{
		std::printf("test_runtimedir: ALL PASS\n");
		return 0;
	}
	std::printf("test_runtimedir: %d FAILURE(S)\n", g_failures);
	return 1;
}
