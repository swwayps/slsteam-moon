// SPDX-License-Identifier: AGPL-3.0-only
#include "librarydates.hpp"

#include "../utils/atomic_file.hpp"
#include "../utils/process_lock.hpp"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <sstream>

namespace LibraryDates
{
namespace
{
using Dates = std::map<uint32_t, uint32_t>;
constexpr const char* header = "library-added-times-v1";
constexpr std::size_t maxFileSize = 8 * 1024 * 1024;
enum class ReadResult { Missing, Ready, Failed };

bool parseNumber(const std::string& text, uint32_t& value)
{
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	return result.ec == std::errc{} && result.ptr == text.data() + text.size()
	    && value != 0;
}

ReadResult readDates(const std::string& path, Dates& dates, std::string& error)
{
	// Refuse symlinks and special files: a date cache must never block startup.
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
	{
		if (errno == ENOENT) return ReadResult::Missing;
		error = "cannot open date file";
		return ReadResult::Failed;
	}
	struct stat st{};
	if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
	    static_cast<uint64_t>(st.st_size) > maxFileSize)
	{
		::close(fd);
		error = "invalid date file type or size";
		return ReadResult::Failed;
	}
	std::string text(static_cast<std::size_t>(st.st_size), '\0');
	std::size_t offset = 0;
	while (offset < text.size())
	{
		const auto count = ::read(fd, text.data() + offset, text.size() - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) break;
		offset += static_cast<std::size_t>(count);
	}
	::close(fd);
	if (offset != text.size())
	{
		error = "incomplete date file read";
		return ReadResult::Failed;
	}
	std::istringstream input(text);
	std::string line;
	if (!std::getline(input, line) || line != header)
	{
		error = "unrecognized date file format";
		return ReadResult::Failed;
	}
	while (std::getline(input, line))
	{
		if (line.empty()) continue;
		std::istringstream row(line);
		std::string idText, dateText, extra;
		uint32_t id = 0, date = 0;
		if (!(row >> idText >> dateText) || (row >> extra) ||
		    !parseNumber(idText, id) || !parseNumber(dateText, date) ||
		    !dates.emplace(id, date).second)
		{
			dates.clear();
			error = "invalid date file record";
			return ReadResult::Failed;
		}
	}
	return ReadResult::Ready;
}

uint32_t initialDate(const std::string& scriptDir, uint32_t appId, uint32_t now)
{
	if (!scriptDir.empty())
	{
		const auto path = scriptDir + "/" + std::to_string(appId) + ".lua";
		struct stat st{};
		if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_mtime > 0)
			return static_cast<uint32_t>(std::min<uint64_t>(st.st_mtime, now));
	}
	return now;
}
}

bool Store::refresh(const std::string& configDir, const std::string& scriptDir,
                    const std::unordered_set<uint32_t>& appIds, uint32_t now,
                    std::string& error)
{
	std::unique_lock guard(mutex_);
	error.clear();
	const std::string path = configDir + "/library-added-times.txt";
	if (path_ != path)
	{
		path_ = path;
		times_.clear();
		initialized_ = false;
	}
	ProcessLock::FileLock fileLock(path + ".lock");
	Dates persisted;
	ReadResult result = ReadResult::Failed;
	if (fileLock.acquired())
		result = readDates(path, persisted, error);
	else
		error = "date store lock unavailable";

	Dates next;
	for (uint32_t id : appIds)
	{
		if (!id) continue;
		const auto local = times_.find(id);
		const auto saved = persisted.find(id);
		// Once initialized, memory is authoritative: a failed removal write
		// must not resurrect an old disk date when the app is added again.
		const uint32_t date = local != times_.end() ? local->second
		    : !initialized_ && saved != persisted.end() ? saved->second
		    : !initialized_ ? initialDate(scriptDir, id, now) : now;
		if (date) next.emplace(id, date);
	}
	// A removed app is forgotten, so a later re-add gets a new inclusion date.
	times_.swap(next);
	initialized_ = true;
	if (result == ReadResult::Failed) return false;
	if (result == ReadResult::Ready && times_ == persisted) return true;

	std::ostringstream output;
	output << header << '\n';
	for (const auto& [id, date] : times_) output << id << ' ' << date << '\n';
	const auto text = output.str();
	if (text.size() > maxFileSize)
	{
		error = "date snapshot exceeds file size limit";
		return false;
	}
	return AtomicFile::write(path, text, error);
}

uint32_t Store::get(uint32_t appId) const
{
	std::shared_lock guard(mutex_);
	const auto found = times_.find(appId);
	return found == times_.end() ? 0 : found->second;
}
}
