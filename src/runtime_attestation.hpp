#pragma once

#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iomanip>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <sys/stat.h>
#include <unistd.h>

namespace RuntimeAttestation
{
	struct Metadata
	{
		std::string session;
		std::string candidateBuildId;
		std::uint64_t pid;
		std::uint64_t monotonicMs;
	};

	struct Field
	{
		enum class Type
		{
			Text,
			Number,
			Boolean,
		};

		std::string name;
		Type type;
		std::string textValue;
		std::uint64_t numberValue{};
		bool booleanValue{};

		static Field text(std::string name, std::string value)
		{
			return {std::move(name), Type::Text, std::move(value), 0, false};
		}

		static Field number(std::string name, std::uint64_t value)
		{
			return {std::move(name), Type::Number, {}, value, false};
		}

		static Field boolean(std::string name, bool value)
		{
			return {std::move(name), Type::Boolean, {}, 0, value};
		}
	};

	inline std::string escapeJson(std::string_view value)
	{
		std::ostringstream escaped;
		for (const unsigned char character : value)
		{
			switch (character)
			{
			case '"': escaped << "\\\""; break;
			case '\\': escaped << "\\\\"; break;
			case '\b': escaped << "\\b"; break;
			case '\f': escaped << "\\f"; break;
			case '\n': escaped << "\\n"; break;
			case '\r': escaped << "\\r"; break;
			case '\t': escaped << "\\t"; break;
			default:
				if (character < 0x20)
				{
					escaped << "\\u" << std::hex << std::setw(4)
					        << std::setfill('0') << static_cast<unsigned>(character)
					        << std::dec;
				}
				else
					escaped << character;
			}
		}
		return escaped.str();
	}

	inline std::string encodeEvent
	(
		const Metadata& metadata,
		std::string_view event,
		std::initializer_list<Field> fields = {}
	)
	{
		std::ostringstream line;
		line << "{\"schema\":1,\"event\":\"" << escapeJson(event)
		     << "\",\"session\":\"" << escapeJson(metadata.session)
		     << "\",\"pid\":" << metadata.pid
		     << ",\"monotonic_ms\":" << metadata.monotonicMs
		     << ",\"candidate_build_id\":\""
		     << escapeJson(metadata.candidateBuildId) << '"';

		for (const Field& field : fields)
		{
			line << ",\"" << escapeJson(field.name) << "\":";
			switch (field.type)
			{
			case Field::Type::Text:
				line << '"' << escapeJson(field.textValue) << '"';
				break;
			case Field::Type::Number:
				line << field.numberValue;
				break;
			case Field::Type::Boolean:
				line << (field.booleanValue ? "true" : "false");
				break;
			}
		}

		line << '}';
		return line.str();
	}

	class EventWriter
	{
	public:
		explicit EventWriter(const std::string& path)
		{
			fd_ = open
			(
				path.c_str(),
				O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
				0600
			);
			if (fd_ < 0)
				return;

			struct stat fileStat {};
			if (fstat(fd_, &fileStat) != 0 || !S_ISREG(fileStat.st_mode)
			    || fchmod(fd_, 0600) != 0)
			{
				close(fd_);
				fd_ = -1;
			}
		}

		~EventWriter()
		{
			if (fd_ >= 0)
				close(fd_);
		}

		EventWriter(const EventWriter&) = delete;
		EventWriter& operator=(const EventWriter&) = delete;

		bool good() const
		{
			return fd_ >= 0;
		}

		bool append
		(
			const Metadata& metadata,
			std::string_view event,
			std::initializer_list<Field> fields = {}
		)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			return appendLocked(encodeEvent(metadata, event, fields) + '\n');
		}

		bool appendOnce
		(
			std::string key,
			const Metadata& metadata,
			std::string_view event,
			std::initializer_list<Field> fields = {}
		)
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (onceKeys_.contains(key))
				return true;
			if (!appendLocked(encodeEvent(metadata, event, fields) + '\n'))
				return false;
			onceKeys_.insert(std::move(key));
			return true;
		}

	private:
		bool appendLocked(const std::string& line)
		{
			if (fd_ < 0)
				return false;

			std::size_t offset = 0;
			while (offset < line.size())
			{
				const ssize_t written = write(fd_, line.data() + offset,
				                              line.size() - offset);
				if (written > 0)
				{
					offset += static_cast<std::size_t>(written);
					continue;
				}
				if (written < 0 && errno == EINTR)
					continue;
				return false;
			}
			return true;
		}

		int fd_ = -1;
		std::mutex mutex_;
		std::unordered_set<std::string> onceKeys_;
	};

	inline std::unique_ptr<EventWriter> activeWriter;
	inline std::string activeSession;
	inline std::string activeBuildId;

	inline Metadata currentMetadata()
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>
		(
			std::chrono::steady_clock::now().time_since_epoch()
		);
		return
		{
			activeSession,
			activeBuildId,
			static_cast<std::uint64_t>(getpid()),
			static_cast<std::uint64_t>(elapsed.count()),
		};
	}

	inline bool initialize(std::string candidateBuildId)
	{
		if (activeWriter)
			return true;

		const char* path = getenv("SLSSTEAM_ATTESTATION_FILE");
		const char* session = getenv("SLSSTEAM_ATTESTATION_SESSION");
		if (path == nullptr || path[0] == '\0'
		    || session == nullptr || session[0] == '\0')
			return false;

		auto writer = std::make_unique<EventWriter>(path);
		if (!writer->good())
			return false;

		activeSession = session;
		activeBuildId = std::move(candidateBuildId);
		activeWriter = std::move(writer);
		return activeWriter->append(currentMetadata(), "attestation-started");
	}

	inline bool enabled()
	{
		return activeWriter != nullptr;
	}

	inline bool emit
	(
		std::string_view event,
		std::initializer_list<Field> fields = {}
	)
	{
		if (!activeWriter)
			return false;
		return activeWriter->append(currentMetadata(), event, fields);
	}

	inline bool emitOnce
	(
		std::string key,
		std::string_view event,
		std::initializer_list<Field> fields = {}
	)
	{
		if (!activeWriter)
			return false;
		return activeWriter->appendOnce
		(
			std::move(key), currentMetadata(), event, fields
		);
	}
}
