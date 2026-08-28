#include "../src/feats/librarydates.hpp"
#include "../src/utils/process_lock.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace
{
int failures = 0;
constexpr uint32_t first = 1700000000;
constexpr uint32_t second = 1701000000;
constexpr uint32_t now = 1780000000;

void check(bool condition, const char* message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAIL", message);
	if (!condition) ++failures;
}

void script(const fs::path& dir, uint32_t appId, uint32_t time)
{
	const auto path = dir / (std::to_string(appId) + ".lua");
	std::ofstream(path) << "-- test fixture\n";
	const timespec times[] = {{time, 0}, {time, 0}};
	if (::utimensat(AT_FDCWD, path.c_str(), times, 0) != 0) std::abort();
}

std::string read(const fs::path& path)
{
	std::ifstream file(path);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}

int main()
{
	char pattern[] = "/tmp/slsteam_library_dates_XXXXXX";
	const char* temp = ::mkdtemp(pattern);
	if (!temp) return 2;
	const fs::path root(temp);
	const auto config = root / "config";
	const auto scripts = root / "scripts";
	fs::create_directories(config);
	fs::create_directories(scripts);
	const auto path = config / "library-added-times.txt";
	script(scripts, 101, first);
	script(scripts, 202, second);
	std::string error;
	LibraryDates::Store dates;
	check(dates.refresh(config.string(), scripts.string(), {101, 202, 303}, now, error),
	      "first discovery saves a complete date snapshot");
	check(dates.get(101) == first && dates.get(202) == second,
	      "migration uses each existing script's date independently");
	check(dates.get(303) == now,
	      "a source without a script uses its first observation time");
	check(dates.get(0) == 0 && dates.get(999) == 0,
	      "unknown app ids have no date");

	struct stat before{}, after{};
	::stat(path.c_str(), &before);
	check(dates.refresh(config.string(), scripts.string(), {303, 202, 101}, now + 100, error),
	      "an unchanged config reload succeeds");
	::stat(path.c_str(), &after);
	check(before.st_ino == after.st_ino,
	      "an unchanged reload does not rewrite the date file");
	script(scripts, 101, now + 100);
	LibraryDates::Store restarted;
	check(restarted.refresh(config.string(), scripts.string(), {101, 202, 303}, now + 200, error),
	      "a fresh process loads the persisted dates");
	check(restarted.get(101) == first && restarted.get(303) == now,
	      "script updates and restarts never advance existing dates");

	script(scripts, 404, first);
	check(restarted.refresh(config.string(), scripts.string(), {101, 202, 303, 404}, now + 300, error),
	      "a runtime addition persists its own date");
	check(restarted.get(404) == now + 300,
	      "a newly copied old script uses runtime inclusion time");
	check(restarted.get(101) == first,
	      "adding another game does not move existing games");
	check(restarted.refresh(config.string(), scripts.string(), {202, 303, 404}, now + 400, error),
	      "removal persists the remaining snapshot");
	check(restarted.get(101) == 0,
	      "a removed app loses its inclusion date");
	LibraryDates::Store afterRemoval;
	check(afterRemoval.refresh(config.string(), scripts.string(), {202, 303, 404}, now + 500, error),
	      "removal survives a restart");
	check(afterRemoval.get(101) == 0,
	      "a removed date does not reappear from disk");
	check(afterRemoval.refresh(config.string(), scripts.string(), {101, 202, 303, 404}, now + 600, error),
	      "re-adding an app records a new inclusion");
	check(afterRemoval.get(101) == now + 600,
	      "re-added apps sort with the new additions");
	{
		ProcessLock::FileLock otherWriter(path.string() + ".lock");
		check(otherWriter.acquired(), "the date-store lock fixture is held");
		check(!afterRemoval.refresh(config.string(), scripts.string(), {202, 303, 404}, now + 650, error),
		      "a busy store defers persistence without blocking source discovery");
	}
	check(afterRemoval.refresh(config.string(), scripts.string(), {101, 202, 303, 404}, now + 675, error)
	      && afterRemoval.get(101) == now + 675,
	      "re-add never resurrects a stale date after a failed removal write");

	const auto blocked = root / "blocked";
	std::ofstream(blocked) << "not a directory";
	LibraryDates::Store unavailable;
	check(!unavailable.refresh(blocked.string(), scripts.string(), {505}, now, error),
	      "an unavailable date store reports failure without throwing");
	check(!error.empty() && unavailable.get(505) == now,
	      "a disk failure retains a usable session date");
	check(!unavailable.refresh(blocked.string(), scripts.string(), {505}, now + 700, error)
	      && unavailable.get(505) == now,
	      "repeated write failures do not advance the session date");
	fs::remove(blocked);
	fs::create_directory(blocked);
	check(unavailable.refresh(blocked.string(), scripts.string(), {505}, now + 800, error),
	      "persistence recovers when the directory becomes available");
	LibraryDates::Store recovered;
	check(recovered.refresh(blocked.string(), scripts.string(), {505}, now + 900, error)
	      && recovered.get(505) == now,
	      "recovery saves the original session date, not retry time");

	const auto corruptDir = root / "corrupt";
	fs::create_directory(corruptDir);
	const auto corruptPath = corruptDir / "library-added-times.txt";
	const std::string corrupt = "library-added-times-v1\n101 42949672960\n";
	std::ofstream(corruptPath) << corrupt;
	LibraryDates::Store malformed;
	check(!malformed.refresh(corruptDir.string(), scripts.string(), {101}, now + 1000, error),
	      "an overflowing persisted timestamp is rejected without exceptions");
	check(read(corruptPath) == corrupt,
	      "invalid state is preserved for inspection, never overwritten");
	check(malformed.get(101) == now + 100,
	      "invalid state falls back to script metadata, not a package date");

	const auto futureDir = root / "future";
	fs::create_directory(futureDir);
	script(scripts, 606, now + 10000);
	LibraryDates::Store future;
	check(future.refresh(futureDir.string(), scripts.string(), {606}, now, error)
	      && future.get(606) == now,
	      "future script timestamps are clamped to observation time");
	fs::remove_all(root);
	std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
