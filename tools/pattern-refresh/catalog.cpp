#include "catalog.hpp"

#include "pattern_catalog.hpp"
#include "utils/process_lock.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>


namespace
{
	bool isComponent(std::string_view value)
	{
		return value == "steamclient" || value == "steamui";
	}

	bool isSha256(std::string_view value)
	{
		return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char byte)
		{
			return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
		});
	}

	bool isModule(std::string_view component, std::string_view moduleName)
	{
		return (component == "steamclient" && moduleName == "steamclient.so")
		    || (component == "steamui" && moduleName == "steamui.so");
	}

	bool isLowerHex(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum && value.size() % 2 == 0
		    && std::all_of(value.begin(), value.end(), [](unsigned char byte)
		       {
			       return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
		       });
	}

	std::optional<std::string> responseBody(
		const PatternRefresh::Response& response,
		const std::optional<PatternRefresh::CachedPair>& cache,
		bool catalog
	)
	{
		if (!response.transportOk)
			return std::nullopt;
		if (response.status == 200)
			return response.body;
		if (response.status != 304 || !cache || !cache->exactSha)
			return std::nullopt;
		return catalog ? cache->catalog : cache->signature;
	}

	PatternRefresh::Selection evaluate(
		const PatternRefresh::ResponsePair& responses,
		PatternRefresh::Source remoteSource,
		const std::optional<PatternRefresh::CachedPair>& cache,
		const PatternRefresh::Validator& validate
	)
	{
		auto catalog = responseBody(responses.catalog, cache, true);
		auto signature = responseBody(responses.signature, cache, false);
		if (!catalog || !signature || !validate(*catalog, *signature))
			return {};
		const bool onlyNotModified = responses.catalog.status == 304
		                          && responses.signature.status == 304;
		return
		{
			onlyNotModified ? PatternRefresh::Source::RevalidatedCache : remoteSource,
			std::move(*catalog),
			std::move(*signature),
		};
	}

	void setError(std::string* output, std::string_view message)
	{
		if (output != nullptr)
			output->assign(message);
	}

	bool verifyEd25519(
		std::string_view body,
		std::span<const unsigned char> signature,
		const PatternRefresh::PublicKey& publicKey
	)
	{
		std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
			EVP_PKEY_new_raw_public_key(
				EVP_PKEY_ED25519, nullptr, publicKey.data(), publicKey.size()
			),
			EVP_PKEY_free
		);
		std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
			EVP_MD_CTX_new(), EVP_MD_CTX_free
		);
		return key && context
		    && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1
		    && EVP_DigestVerify(
				context.get(), signature.data(), signature.size(),
				reinterpret_cast<const unsigned char*>(body.data()), body.size()
			) == 1;
	}

	struct FileStamp
	{
		std::uint64_t device = 0;
		std::uint64_t inode = 0;
		std::uint64_t size = 0;
		std::int64_t mtimeSeconds = 0;
		std::int64_t mtimeNanoseconds = 0;
		std::int64_t ctimeSeconds = 0;
		std::int64_t ctimeNanoseconds = 0;

		bool operator==(const FileStamp&) const = default;
	};

	std::optional<FileStamp> stampFor(int descriptor)
	{
		struct stat value {};
		if (fstat(descriptor, &value) != 0 || !S_ISREG(value.st_mode) || value.st_size <= 0)
			return std::nullopt;
		return FileStamp
		{
			static_cast<std::uint64_t>(value.st_dev),
			static_cast<std::uint64_t>(value.st_ino),
			static_cast<std::uint64_t>(value.st_size),
			static_cast<std::int64_t>(value.st_mtim.tv_sec),
			static_cast<std::int64_t>(value.st_mtim.tv_nsec),
			static_cast<std::int64_t>(value.st_ctim.tv_sec),
			static_cast<std::int64_t>(value.st_ctim.tv_nsec),
		};
	}

	bool readExactAt(int descriptor, void* output, std::size_t size, std::uint64_t offset)
	{
		auto* bytes = static_cast<unsigned char*>(output);
		std::size_t consumed = 0;
		while (consumed < size)
		{
			const ssize_t count = pread(
				descriptor, bytes + consumed, size - consumed,
				static_cast<off_t>(offset + consumed)
			);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				return false;
			consumed += static_cast<std::size_t>(count);
		}
		return true;
	}

	std::uint64_t littleEndian(const unsigned char* bytes, std::size_t size)
	{
		std::uint64_t value = 0;
		for (std::size_t index = 0; index < size; ++index)
			value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
		return value;
	}

	std::string hexadecimal(const unsigned char* bytes, std::size_t size)
	{
		static constexpr char digits[] = "0123456789abcdef";
		std::string result(size * 2, '0');
		for (std::size_t index = 0; index < size; ++index)
		{
			result[index * 2] = digits[bytes[index] >> 4];
			result[index * 2 + 1] = digits[bytes[index] & 0x0F];
		}
		return result;
	}

	std::optional<std::string> sha256For(int descriptor)
	{
		if (lseek(descriptor, 0, SEEK_SET) < 0)
			return std::nullopt;
		std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
			EVP_MD_CTX_new(), EVP_MD_CTX_free
		);
		if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
			return std::nullopt;
		std::array<unsigned char, 64 * 1024> buffer {};
		for (;;)
		{
			const ssize_t count = read(descriptor, buffer.data(), buffer.size());
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0)
				return std::nullopt;
			if (count == 0)
				break;
			if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
				return std::nullopt;
		}
		std::array<unsigned char, 32> digest {};
		unsigned int size = 0;
		if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != digest.size())
			return std::nullopt;
		return hexadecimal(digest.data(), digest.size());
	}

	struct ElfLayout
	{
		std::string buildId;
		std::vector<PatternRefresh::ExecutableRange> executableRanges;
	};

	std::optional<ElfLayout> layoutFor(int descriptor, std::uint64_t fileSize)
	{
		std::array<unsigned char, 64> header {};
		if (!readExactAt(descriptor, header.data(), header.size(), 0)
		    || std::memcmp(header.data(), "\x7f" "ELF", 4) != 0
		    || header[5] != 1)
			return std::nullopt;
		const bool elf64 = header[4] == 2;
		if (!elf64 && header[4] != 1)
			return std::nullopt;
		const std::uint64_t programOffset = littleEndian(
			header.data() + (elf64 ? 0x20 : 0x1C), elf64 ? 8 : 4
		);
		const std::uint64_t entrySize = littleEndian(
			header.data() + (elf64 ? 0x36 : 0x2A), 2
		);
		const std::uint64_t entryCount = littleEndian(
			header.data() + (elf64 ? 0x38 : 0x2C), 2
		);
		const std::size_t minimumEntry = elf64 ? 56 : 32;
		if (entrySize < minimumEntry || entrySize > 256 || entryCount == 0
		    || entryCount > 1024 || programOffset > fileSize
		    || entryCount > (fileSize - programOffset) / entrySize)
			return std::nullopt;

		ElfLayout result;
		std::vector<unsigned char> entry(static_cast<std::size_t>(entrySize));
		for (std::uint64_t index = 0; index < entryCount; ++index)
		{
			if (!readExactAt(
				descriptor, entry.data(), entry.size(), programOffset + index * entrySize
			))
				return std::nullopt;
			const std::uint64_t type = littleEndian(entry.data(), 4);
			if (type == 1)
			{
				const std::uint64_t virtualAddress = littleEndian(
					entry.data() + (elf64 ? 16 : 8), elf64 ? 8 : 4
				);
				const std::uint64_t memorySize = littleEndian(
					entry.data() + (elf64 ? 40 : 20), elf64 ? 8 : 4
				);
				const std::uint64_t flags = littleEndian(
					entry.data() + (elf64 ? 4 : 24), 4
				);
				if ((flags & 1) != 0 && memorySize > 0
				    && virtualAddress <= std::numeric_limits<std::uint64_t>::max() - memorySize)
				{
					result.executableRanges.push_back(
						{virtualAddress, virtualAddress + memorySize}
					);
				}
			}
			if (type != 4)
				continue;
			const std::uint64_t noteOffset = littleEndian(entry.data() + (elf64 ? 8 : 4), elf64 ? 8 : 4);
			const std::uint64_t noteSize = littleEndian(entry.data() + (elf64 ? 32 : 16), elf64 ? 8 : 4);
			if (noteSize < 16 || noteSize > 1024 * 1024 || noteOffset > fileSize
			    || noteSize > fileSize - noteOffset)
				continue;
			std::vector<unsigned char> notes(static_cast<std::size_t>(noteSize));
			if (!readExactAt(descriptor, notes.data(), notes.size(), noteOffset))
				return std::nullopt;
			std::size_t cursor = 0;
			while (cursor + 12 <= notes.size())
			{
				const std::uint64_t nameSize = littleEndian(notes.data() + cursor, 4);
				const std::uint64_t descriptionSize = littleEndian(notes.data() + cursor + 4, 4);
				const std::uint64_t type = littleEndian(notes.data() + cursor + 8, 4);
				cursor += 12;
				const std::uint64_t alignedName = (nameSize + 3) & ~std::uint64_t(3);
				const std::uint64_t alignedDescription = (descriptionSize + 3) & ~std::uint64_t(3);
				if (alignedName > notes.size() - cursor)
					break;
				const std::size_t descriptionOffset = cursor + static_cast<std::size_t>(alignedName);
				if (alignedDescription > notes.size() - descriptionOffset)
					break;
				if (type == 3 && result.buildId.empty() && nameSize >= 3
				    && descriptionSize > 0 && descriptionSize <= 64
				    && std::memcmp(notes.data() + cursor, "GNU", 3) == 0)
				{
					result.buildId = hexadecimal(
						notes.data() + descriptionOffset,
						static_cast<std::size_t>(descriptionSize)
					);
				}
				cursor = descriptionOffset + static_cast<std::size_t>(alignedDescription);
			}
		}
		if (result.buildId.empty() || result.executableRanges.empty())
			return std::nullopt;
		std::sort(result.executableRanges.begin(), result.executableRanges.end(),
		          [](const auto& left, const auto& right)
		          {
			          return left.begin < right.begin
			              || (left.begin == right.begin && left.end < right.end);
		          });
		return result;
	}

	std::optional<std::string> readRegularFile(
		const std::filesystem::path& path,
		std::size_t maximum
	)
	{
		const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (descriptor < 0)
			return std::nullopt;
		struct stat value {};
		if (fstat(descriptor, &value) != 0 || !S_ISREG(value.st_mode)
		    || value.st_size < 0 || static_cast<std::uint64_t>(value.st_size) > maximum)
		{
			close(descriptor);
			return std::nullopt;
		}
		std::string result(static_cast<std::size_t>(value.st_size), '\0');
		std::size_t consumed = 0;
		while (consumed < result.size())
		{
			const ssize_t count = read(descriptor, result.data() + consumed, result.size() - consumed);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
			{
				close(descriptor);
				return std::nullopt;
			}
			consumed += static_cast<std::size_t>(count);
		}
		close(descriptor);
		return result;
	}

	bool writeAtomic0600(const std::filesystem::path& path, std::string_view body)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (error || !std::filesystem::is_directory(path.parent_path(), error) || error)
			return false;
		static std::atomic<std::uint64_t> sequence {0};
		const std::filesystem::path temporary = path.parent_path()
			/ ("." + path.filename().string() + ".tmp."
			   + std::to_string(getpid()) + "."
			   + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
		const int descriptor = open(
			temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600
		);
		if (descriptor < 0)
			return false;
		bool success = true;
		std::size_t consumed = 0;
		while (consumed < body.size())
		{
			const ssize_t count = write(descriptor, body.data() + consumed, body.size() - consumed);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
			{
				success = false;
				break;
			}
			consumed += static_cast<std::size_t>(count);
		}
		if (success && (fchmod(descriptor, 0600) != 0 || fsync(descriptor) != 0))
			success = false;
		if (close(descriptor) != 0)
			success = false;
		if (success && rename(temporary.c_str(), path.c_str()) != 0)
			success = false;
		if (!success)
			unlink(temporary.c_str());
		if (success)
		{
			const int directory = open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
			if (directory >= 0)
			{
				fsync(directory);
				close(directory);
			}
		}
		return success;
	}

	template<typename Integer>
	std::optional<Integer> parseInteger(std::string_view value)
	{
		if (value.empty())
			return std::nullopt;
		Integer result {};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
		if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
			return std::nullopt;
		return result;
	}

	std::string stateBody(
		std::string_view component,
		std::string_view moduleName,
		const FileStamp& stamp,
		std::string_view sha256,
		std::string_view buildId
	)
	{
		return "schema=1\ncomponent=" + std::string(component)
		    + "\nmodule_name=" + std::string(moduleName)
		    + "\ndevice=" + std::to_string(stamp.device)
		    + "\ninode=" + std::to_string(stamp.inode)
		    + "\nsize=" + std::to_string(stamp.size)
		    + "\nmtime_seconds=" + std::to_string(stamp.mtimeSeconds)
		    + "\nmtime_nanoseconds=" + std::to_string(stamp.mtimeNanoseconds)
		    + "\nctime_seconds=" + std::to_string(stamp.ctimeSeconds)
		    + "\nctime_nanoseconds=" + std::to_string(stamp.ctimeNanoseconds)
		    + "\nsha256=" + std::string(sha256)
		    + "\nbuild_id=" + std::string(buildId) + "\n";
	}

	struct CachedIdentity
	{
		FileStamp stamp;
		std::string sha256;
		std::string buildId;
	};

	std::optional<CachedIdentity> parseState(
		std::string_view body,
		std::string_view component,
		std::string_view moduleName
	)
	{
		std::vector<std::string_view> lines;
		std::size_t cursor = 0;
		while (cursor < body.size())
		{
			const std::size_t end = body.find('\n', cursor);
			if (end == std::string_view::npos)
				return std::nullopt;
			lines.push_back(body.substr(cursor, end - cursor));
			cursor = end + 1;
		}
		if (lines.size() != 12 || lines[0] != "schema=1"
		    || lines[1] != "component=" + std::string(component)
		    || lines[2] != "module_name=" + std::string(moduleName))
			return std::nullopt;
		const auto value = [&](std::size_t index, std::string_view key) -> std::optional<std::string_view>
		{
			const std::string prefix = std::string(key) + "=";
			if (!lines[index].starts_with(prefix))
				return std::nullopt;
			return lines[index].substr(prefix.size());
		};
		const auto deviceText = value(3, "device");
		const auto inodeText = value(4, "inode");
		const auto sizeText = value(5, "size");
		const auto mtimeSecondsText = value(6, "mtime_seconds");
		const auto mtimeNanosecondsText = value(7, "mtime_nanoseconds");
		const auto ctimeSecondsText = value(8, "ctime_seconds");
		const auto ctimeNanosecondsText = value(9, "ctime_nanoseconds");
		const auto shaText = value(10, "sha256");
		const auto buildText = value(11, "build_id");
		if (!deviceText || !inodeText || !sizeText || !mtimeSecondsText
		    || !mtimeNanosecondsText || !ctimeSecondsText || !ctimeNanosecondsText
		    || !shaText || !buildText || !isSha256(*shaText)
		    || !isLowerHex(*buildText, 128))
			return std::nullopt;
		const auto device = parseInteger<std::uint64_t>(*deviceText);
		const auto inode = parseInteger<std::uint64_t>(*inodeText);
		const auto size = parseInteger<std::uint64_t>(*sizeText);
		const auto mtimeSeconds = parseInteger<std::int64_t>(*mtimeSecondsText);
		const auto mtimeNanoseconds = parseInteger<std::int64_t>(*mtimeNanosecondsText);
		const auto ctimeSeconds = parseInteger<std::int64_t>(*ctimeSecondsText);
		const auto ctimeNanoseconds = parseInteger<std::int64_t>(*ctimeNanosecondsText);
		if (!device || !inode || !size || !mtimeSeconds || !mtimeNanoseconds
		    || !ctimeSeconds || !ctimeNanoseconds)
			return std::nullopt;
		return CachedIdentity
		{
			{*device, *inode, *size, *mtimeSeconds, *mtimeNanoseconds,
			 *ctimeSeconds, *ctimeNanoseconds},
			std::string(*shaText),
			std::string(*buildText),
		};
	}

	bool safeEtag(std::string_view value)
	{
		return !value.empty() && value.size() <= 256
		    && std::all_of(value.begin(), value.end(), [](unsigned char byte)
		       {
			       return byte >= 0x20 && byte < 0x7F && byte != '\\';
		       });
	}

	bool validMirrorBase(std::string_view value, bool allowHttp)
	{
		if (value.empty() || value.size() > 512 || !value.ends_with('/'))
			return false;
		const bool scheme = value.starts_with("https://")
		                 || (allowHttp && value.starts_with("http://"));
		return scheme && std::all_of(value.begin(), value.end(), [](unsigned char byte)
		{
			return byte >= 0x21 && byte < 0x7F && byte != '\\' && byte != '"';
		});
	}

	struct FetchRequest
	{
		std::string url;
		std::size_t maximum = 0;
		std::string etag;
	};

	struct CurlTransfer
	{
		FetchRequest request;
		PatternRefresh::Response response;
		CURL* easy = nullptr;
		curl_slist* headers = nullptr;
		bool overflow = false;
		bool complete = false;
	};

	size_t curlWrite(char* data, size_t size, size_t count, void* opaque)
	{
		auto* transfer = static_cast<CurlTransfer*>(opaque);
		if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
		{
			transfer->overflow = true;
			return 0;
		}
		const std::size_t bytes = size * count;
		if (bytes > transfer->request.maximum - std::min(
			transfer->response.body.size(), transfer->request.maximum
		))
		{
			transfer->overflow = true;
			return 0;
		}
		transfer->response.body.append(data, bytes);
		return bytes;
	}

	size_t curlHeader(char* data, size_t size, size_t count, void* opaque)
	{
		auto* transfer = static_cast<CurlTransfer*>(opaque);
		if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
			return 0;
		const std::size_t bytes = size * count;
		std::string_view line(data, bytes);
		if (line.starts_with("HTTP/"))
			transfer->response.etag.clear();
		if (line.size() >= 5)
		{
			const auto lower = [](unsigned char byte)
			{
				return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
			};
			if (lower(line[0]) == 'e' && lower(line[1]) == 't'
			    && lower(line[2]) == 'a' && lower(line[3]) == 'g' && line[4] == ':')
			{
				line.remove_prefix(5);
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
					line.remove_prefix(1);
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n'
				                         || line.back() == ' ' || line.back() == '\t'))
					line.remove_suffix(1);
				if (safeEtag(line))
					transfer->response.etag.assign(line);
			}
		}
		return bytes;
	}

	std::vector<PatternRefresh::Response> fetchBatch(
		const std::vector<FetchRequest>& requests,
		bool allowHttp,
		const std::function<bool(const std::vector<PatternRefresh::Response>&)>& stopWhen = {}
	)
	{
		std::vector<PatternRefresh::Response> failures(requests.size());
		static const bool curlReady = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
		if (!curlReady || requests.empty())
			return failures;
		CURLM* multi = curl_multi_init();
		if (multi == nullptr)
			return failures;
		std::vector<std::unique_ptr<CurlTransfer>> transfers;
		transfers.reserve(requests.size());
		bool setupOk = true;
		for (const auto& request : requests)
		{
			auto transfer = std::make_unique<CurlTransfer>();
			transfer->request = request;
			transfer->easy = curl_easy_init();
			if (transfer->easy == nullptr)
			{
				setupOk = false;
				transfers.push_back(std::move(transfer));
				continue;
			}
			curl_easy_setopt(transfer->easy, CURLOPT_URL, transfer->request.url.c_str());
			curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, curlWrite);
			curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA, transfer.get());
			curl_easy_setopt(transfer->easy, CURLOPT_HEADERFUNCTION, curlHeader);
			curl_easy_setopt(transfer->easy, CURLOPT_HEADERDATA, transfer.get());
			curl_easy_setopt(transfer->easy, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(transfer->easy, CURLOPT_MAXREDIRS, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
			const char* protocols = allowHttp ? "http,https" : "https";
			curl_easy_setopt(transfer->easy, CURLOPT_PROTOCOLS_STR, protocols);
			curl_easy_setopt(transfer->easy, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#else
			const long protocols = CURLPROTO_HTTPS | (allowHttp ? CURLPROTO_HTTP : 0);
			curl_easy_setopt(transfer->easy, CURLOPT_PROTOCOLS, protocols);
			curl_easy_setopt(transfer->easy, CURLOPT_REDIR_PROTOCOLS, protocols);
#endif
			curl_easy_setopt(transfer->easy, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
			curl_easy_setopt(transfer->easy, CURLOPT_TIMEOUT_MS, 4000L);
			curl_easy_setopt(transfer->easy, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(transfer->easy, CURLOPT_ACCEPT_ENCODING, "");
			curl_easy_setopt(transfer->easy, CURLOPT_USERAGENT, "slsteam-pattern-refresh/1");
			if (safeEtag(transfer->request.etag))
			{
				const std::string header = "If-None-Match: " + transfer->request.etag;
				transfer->headers = curl_slist_append(nullptr, header.c_str());
				curl_easy_setopt(transfer->easy, CURLOPT_HTTPHEADER, transfer->headers);
			}
			if (curl_multi_add_handle(multi, transfer->easy) != CURLM_OK)
				setupOk = false;
			transfers.push_back(std::move(transfer));
		}

		const auto completedResponses = [&]
		{
			std::vector<PatternRefresh::Response> responses;
			responses.reserve(transfers.size());
			for (const auto& transfer : transfers)
				responses.push_back(transfer->response);
			return responses;
		};
		const auto drainCompleted = [&]
		{
			int remaining = 0;
			while (CURLMsg* message = curl_multi_info_read(multi, &remaining))
			{
				if (message->msg != CURLMSG_DONE)
					continue;
				const auto found = std::find_if(
					transfers.begin(), transfers.end(), [&](const auto& item)
					{
						return item->easy == message->easy_handle;
					}
				);
				if (found == transfers.end())
					continue;
				CurlTransfer& transfer = **found;
				curl_easy_getinfo(
					transfer.easy, CURLINFO_RESPONSE_CODE, &transfer.response.status
				);
				transfer.response.transportOk = message->data.result == CURLE_OK
				                             && !transfer.overflow;
				transfer.complete = true;
			}
		};

		int running = 0;
		if (!setupOk || curl_multi_perform(multi, &running) != CURLM_OK)
			running = 0;
		drainCompleted();
		while (running > 0 && !(stopWhen && stopWhen(completedResponses())))
		{
			int descriptors = 0;
			if (curl_multi_poll(multi, nullptr, 0, 100, &descriptors) != CURLM_OK
			    || curl_multi_perform(multi, &running) != CURLM_OK)
				break;
			drainCompleted();
		}
		drainCompleted();

		std::vector<PatternRefresh::Response> responses;
		responses.reserve(transfers.size());
		for (auto& transfer : transfers)
		{
			responses.push_back(std::move(transfer->response));
			if (transfer->easy != nullptr)
			{
				curl_multi_remove_handle(multi, transfer->easy);
				curl_easy_cleanup(transfer->easy);
			}
			if (transfer->headers != nullptr)
				curl_slist_free_all(transfer->headers);
		}
		curl_multi_cleanup(multi);
		return responses.size() == requests.size() ? responses : failures;
	}

	std::filesystem::path activeCatalogPath(
		const std::filesystem::path& root,
		const PatternRefresh::ExactModule& module
	)
	{
		return root / module.component / (module.sha256 + ".toml");
	}

	std::filesystem::path activeSignaturePath(
		const std::filesystem::path& root,
		const PatternRefresh::ExactModule& module
	)
	{
		return root / module.component / (module.sha256 + ".sig");
	}

	std::filesystem::path etagPath(
		const std::filesystem::path& root,
		const PatternRefresh::ExactModule& module,
		std::string_view mirror,
		std::string_view kind
	)
	{
		return root / ".state" / module.component
		    / (module.sha256 + "." + std::string(mirror) + "." + std::string(kind) + ".etag");
	}

	std::string loadEtag(const std::filesystem::path& path)
	{
		const auto body = readRegularFile(path, 256);
		return body && safeEtag(*body) ? *body : std::string();
	}

	void storeResponseEtags(
		const std::filesystem::path& root,
		const PatternRefresh::ExactModule& module,
		std::string_view mirror,
		const PatternRefresh::ResponsePair& responses
	)
	{
		if (safeEtag(responses.catalog.etag))
			writeAtomic0600(etagPath(root, module, mirror, "catalog"), responses.catalog.etag);
		if (safeEtag(responses.signature.etag))
			writeAtomic0600(etagPath(root, module, mirror, "signature"), responses.signature.etag);
	}
}


PatternRefresh::UrlPair PatternRefresh::makeUrls(
	Mirror mirror,
	std::string_view component,
	std::string_view sha256
)
{
	const std::string base = mirror == Mirror::GitHub
		? "https://raw.githubusercontent.com/swwayps/steam-monitor/main/"
		: "https://cdn.jsdelivr.net/gh/swwayps/steam-monitor@main/";
	return makeUrls(base, component, sha256);
}

PatternRefresh::UrlPair PatternRefresh::makeUrls(
	std::string_view base,
	std::string_view component,
	std::string_view sha256
)
{
	if (!isComponent(component) || !isSha256(sha256)
	    || base.empty() || base.size() > 512 || !base.ends_with('/'))
		return {};
	// The detached signature is published as a sibling of the catalog file
	// itself: `<sha256>.toml` and `<sha256>.toml.sig`.  Requesting a
	// `<sha256>.sig` peer instead 404s on both mirrors, which makes the
	// consumer fail closed on every client build and never activate signed
	// metadata at all.
	const std::string stem = std::string(base) + "linux32/" + std::string(component) + "/"
	                       + std::string(sha256);
	const std::string catalog = stem + ".toml";
	return {catalog, catalog + ".sig"};
}

PatternRefresh::Selection PatternRefresh::chooseCandidate(
	const ResponsePair& github,
	const ResponsePair& jsDelivr,
	const std::optional<CachedPair>& cache,
	const Validator& validate
)
{
	if (auto selected = evaluate(github, Source::GitHub, cache, validate))
		return selected;
	if (auto selected = evaluate(jsDelivr, Source::JsDelivr, cache, validate))
		return selected;
	if (cache && cache->exactSha && validate(cache->catalog, cache->signature))
		return {Source::OfflineCache, cache->catalog, cache->signature};
	return {};
}

std::optional<PatternRefresh::ValidatedCatalog> PatternRefresh::validateSignedCatalog(
	std::string_view body,
	std::span<const unsigned char> signature,
	const ExactModule& module,
	const PublicKey& publicKey,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	if (!isModule(module.component, module.moduleName) || !isSha256(module.sha256)
	    || module.size == 0 || !isLowerHex(module.gnuBuildId, 128))
	{
		setError(error, "exact module identity is invalid");
		return std::nullopt;
	}
	if (body.empty() || body.size() > kMaximumCatalogSize)
	{
		setError(error, "catalog size is invalid");
		return std::nullopt;
	}
	if (signature.size() != kSignatureSize)
	{
		setError(error, "catalog signature size is invalid");
		return std::nullopt;
	}
	if (!verifyEd25519(body, signature, publicKey))
	{
		setError(error, "catalog signature is invalid");
		return std::nullopt;
	}

	std::string parseError;
	auto catalog = PatternCatalog::parseCanonical(
		body,
		{
			module.component,
			module.moduleName,
			module.sha256,
			module.size,
			module.gnuBuildId,
		},
		&parseError
	);
	if (!catalog)
	{
		setError(error, parseError.empty() ? "catalog metadata is invalid" : parseError);
		return std::nullopt;
	}
	for (const auto& locator : catalog->locators())
	{
		const bool executable = std::any_of(
			module.executableRanges.begin(), module.executableRanges.end(),
			[&](const ExecutableRange& range)
			{
				return range.begin < range.end
				    && locator.targetRva >= range.begin
				    && locator.targetRva < range.end;
			}
		);
		if (!executable)
		{
			setError(error, "catalog target is not in an executable ELF segment");
			return std::nullopt;
		}
	}
	return ValidatedCatalog{catalog->revision()};
}

bool PatternRefresh::acceptsRevision(
	const ValidatedCatalog& candidate,
	std::string_view candidateBody,
	const std::optional<ValidatedCatalog>& active,
	std::string_view activeBody
) noexcept
{
	if (!active)
		return true;
	if (candidate.revision > active->revision)
		return true;
	return candidate.revision == active->revision && candidateBody == activeBody;
}

std::optional<PatternRefresh::InspectedModule> PatternRefresh::inspectModule(
	std::string component,
	std::string moduleName,
	const std::filesystem::path& modulePath,
	const std::filesystem::path& statePath,
	const std::function<void()>& beforeRestat,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	if (!isModule(component, moduleName) || !modulePath.is_absolute() || !statePath.is_absolute())
	{
		setError(error, "module inspection arguments are invalid");
		return std::nullopt;
	}
	const int descriptor = open(modulePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
	{
		setError(error, "module could not be opened");
		return std::nullopt;
	}
	const auto before = stampFor(descriptor);
	if (!before)
	{
		close(descriptor);
		setError(error, "module is not a stable regular file");
		return std::nullopt;
	}
	const auto layout = layoutFor(descriptor, before->size);
	if (!layout || !isLowerHex(layout->buildId, 128))
	{
		close(descriptor);
		setError(error, "module ELF layout could not be derived");
		return std::nullopt;
	}

	if (const auto stateText = readRegularFile(statePath, 4096))
	{
		if (const auto cached = parseState(*stateText, component, moduleName);
		    cached && cached->stamp == *before && cached->buildId == layout->buildId)
		{
			close(descriptor);
			return InspectedModule
			{
				{std::move(component), std::move(moduleName), cached->sha256,
				 before->size, layout->buildId, layout->executableRanges},
				true,
			};
		}
	}

	const auto sha256 = sha256For(descriptor);
	if (!sha256 || !isSha256(*sha256))
	{
		close(descriptor);
		setError(error, "module identity could not be derived");
		return std::nullopt;
	}
	try
	{
		if (beforeRestat)
			beforeRestat();
	}
	catch (...)
	{
		close(descriptor);
		setError(error, "module inspection hook failed");
		return std::nullopt;
	}
	const auto after = stampFor(descriptor);
	close(descriptor);
	if (!after || *before != *after)
	{
		setError(error, "module changed during inspection");
		return std::nullopt;
	}
	if (!writeAtomic0600(
		statePath, stateBody(component, moduleName, *before, *sha256, layout->buildId)
	))
	{
		setError(error, "module identity cache could not be written");
		return std::nullopt;
	}
	return InspectedModule
	{
		{std::move(component), std::move(moduleName), *sha256, before->size,
		 layout->buildId, layout->executableRanges},
		false,
	};
}

PatternRefresh::ActivationResult PatternRefresh::activateSignedCatalog(
	const std::filesystem::path& patternRoot,
	const ExactModule& module,
	std::string_view body,
	std::span<const unsigned char> signature,
	const PublicKey& publicKey,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	if (!patternRoot.is_absolute() || !isModule(module.component, module.moduleName)
	    || !isSha256(module.sha256))
	{
		setError(error, "catalog activation arguments are invalid");
		return {};
	}
	const auto candidate = validateSignedCatalog(body, signature, module, publicKey, error);
	if (!candidate)
		return {};

	const std::filesystem::path directory = patternRoot / module.component;
	const std::filesystem::path catalogPath = directory / (module.sha256 + ".toml");
	const std::filesystem::path signaturePath = directory / (module.sha256 + ".sig");
	std::optional<ValidatedCatalog> active;
	std::string activeBody;
	if (const auto catalogBytes = readRegularFile(catalogPath, kMaximumCatalogSize);
	    catalogBytes && !catalogBytes->empty())
	{
		if (const auto signatureBytes = readRegularFile(signaturePath, kSignatureSize);
		    signatureBytes && signatureBytes->size() == kSignatureSize)
		{
			activeBody = *catalogBytes;
			const auto* signatureData = reinterpret_cast<const unsigned char*>(signatureBytes->data());
			active = validateSignedCatalog(
				activeBody,
				std::span<const unsigned char>(signatureData, signatureBytes->size()),
				module,
				publicKey
			);
		}
	}
	if (!acceptsRevision(*candidate, body, active, activeBody))
	{
		setError(error, "catalog revision would roll back active metadata");
		return {};
	}
	if (active && candidate->revision == active->revision && body == activeBody)
		return {true, false, candidate->revision};

	const auto signatureText = std::string_view(
		reinterpret_cast<const char*>(signature.data()), signature.size()
	);
	if (!writeAtomic0600(signaturePath, signatureText)
	    || !writeAtomic0600(catalogPath, body))
	{
		setError(error, "catalog could not be activated atomically");
		return {};
	}
	return {true, true, candidate->revision};
}

PatternRefresh::RefreshResult PatternRefresh::refreshComponent(
	const std::filesystem::path& patternRoot,
	const ExactModule& module,
	const PublicKey& publicKey,
	RefreshMode mode,
	const MirrorBases& mirrors,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	if (!patternRoot.is_absolute() || !isModule(module.component, module.moduleName)
	    || !isSha256(module.sha256) || module.size == 0
	    || !isLowerHex(module.gnuBuildId, 128)
	    || (mode != RefreshMode::Remote && mode != RefreshMode::CacheOnly)
	    || (mode == RefreshMode::Remote
	        && (!validMirrorBase(mirrors.github, mirrors.allowHttpForTests)
	            || !validMirrorBase(mirrors.jsDelivr, mirrors.allowHttpForTests))))
	{
		setError(error, "pattern refresh arguments are invalid");
		return {};
	}

	std::optional<CachedPair> cache;
	std::optional<ValidatedCatalog> activeMetadata;
	std::string activeBody;
	const auto cachedCatalog = readRegularFile(
		activeCatalogPath(patternRoot, module), kMaximumCatalogSize
	);
	const auto cachedSignature = readRegularFile(
		activeSignaturePath(patternRoot, module), kSignatureSize
	);
	if (cachedCatalog && cachedSignature && !cachedCatalog->empty()
	    && cachedSignature->size() == kSignatureSize)
	{
		const auto* bytes = reinterpret_cast<const unsigned char*>(cachedSignature->data());
		activeMetadata = validateSignedCatalog(
			*cachedCatalog,
			std::span<const unsigned char>(bytes, cachedSignature->size()),
			module,
			publicKey
		);
		if (activeMetadata)
		{
			activeBody = *cachedCatalog;
			cache = CachedPair{*cachedCatalog, *cachedSignature, true};
		}
	}
	if (mode == RefreshMode::CacheOnly)
	{
		if (activeMetadata)
			return {true, false, Source::OfflineCache, activeMetadata->revision};
		setError(error, "no valid signed exact-module cache is available");
		return {};
	}

	const Validator validator = [&](std::string_view body, std::string_view signature)
	{
		const auto* bytes = reinterpret_cast<const unsigned char*>(signature.data());
		const auto candidate = validateSignedCatalog(
			body,
			std::span<const unsigned char>(bytes, signature.size()),
			module,
			publicKey
		);
		return candidate && acceptsRevision(
			*candidate, body, activeMetadata, activeBody
		);
	};

	const UrlPair githubUrls = makeUrls(mirrors.github, module.component, module.sha256);
	const UrlPair cdnUrls = makeUrls(mirrors.jsDelivr, module.component, module.sha256);
	const std::vector<FetchRequest> requests
	{
		{githubUrls.catalog, kMaximumCatalogSize,
		 loadEtag(etagPath(patternRoot, module, "github", "catalog"))},
		{githubUrls.signature, kSignatureSize,
		 loadEtag(etagPath(patternRoot, module, "github", "signature"))},
		{cdnUrls.catalog, kMaximumCatalogSize,
		 loadEtag(etagPath(patternRoot, module, "jsdelivr", "catalog"))},
		{cdnUrls.signature, kSignatureSize,
		 loadEtag(etagPath(patternRoot, module, "jsdelivr", "signature"))},
	};
	const auto responses = fetchBatch(
		requests,
		mirrors.allowHttpForTests,
		[&](const std::vector<Response>& partial)
		{
			if (partial.size() != 4)
				return false;
			const Selection selected = chooseCandidate(
				{partial[0], partial[1]},
				{partial[2], partial[3]},
				cache,
				validator
			);
			return selected.source == Source::GitHub
			    || selected.source == Source::JsDelivr
			    || selected.source == Source::RevalidatedCache;
		}
	);
	if (responses.size() != 4)
	{
		setError(error, "mirror request batch failed");
		return activeMetadata
			? RefreshResult{true, false, Source::OfflineCache, activeMetadata->revision}
			: RefreshResult{};
	}
	const ResponsePair github {responses[0], responses[1]};
	const ResponsePair jsDelivr {responses[2], responses[3]};
	const Selection githubOnly = chooseCandidate(
		github, ResponsePair{}, cache, validator
	);
	const Selection selected = chooseCandidate(github, jsDelivr, cache, validator);
	if (!selected)
	{
		setError(error, "no valid signed exact-module catalog is available");
		return {};
	}

	if (selected.source == Source::GitHub || selected.source == Source::JsDelivr)
	{
		const auto* bytes = reinterpret_cast<const unsigned char*>(selected.signature.data());
		const auto activated = activateSignedCatalog(
			patternRoot,
			module,
			selected.catalog,
			std::span<const unsigned char>(bytes, selected.signature.size()),
			publicKey,
			error
		);
		if (!activated.active)
			return {};
		if (selected.source == Source::GitHub)
			storeResponseEtags(patternRoot, module, "github", github);
		else
			storeResponseEtags(patternRoot, module, "jsdelivr", jsDelivr);
		return {true, activated.changed, selected.source, activated.revision};
	}

	if (!activeMetadata)
	{
		setError(error, "remote response requires an unavailable exact cache");
		return {};
	}
	if (selected.source == Source::RevalidatedCache)
	{
		if (githubOnly.source == Source::RevalidatedCache)
			storeResponseEtags(patternRoot, module, "github", github);
		else
			storeResponseEtags(patternRoot, module, "jsdelivr", jsDelivr);
	}
	return {true, false, selected.source, activeMetadata->revision};
}

std::optional<PatternRefresh::PublicKey> PatternRefresh::parsePublicKeyHex(
	std::string_view value
) noexcept
{
	if (value.size() != 64 || !std::all_of(value.begin(), value.end(), [](unsigned char byte)
	    {
		    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
	    }))
		return std::nullopt;
	const auto nibble = [](unsigned char byte) -> unsigned char
	{
		return byte <= '9' ? byte - '0' : byte - 'a' + 10;
	};
	PublicKey result {};
	for (std::size_t index = 0; index < result.size(); ++index)
		result[index] = static_cast<unsigned char>(
			(nibble(value[index * 2]) << 4) | nibble(value[index * 2 + 1])
		);
	return result;
}

std::optional<PatternRefresh::Invocation> PatternRefresh::parseInvocation(
	const std::vector<std::string_view>& arguments
)
{
	if (arguments.empty() || arguments.front().empty())
		return std::nullopt;
	Invocation result;
	bool steamSeen = false;
	bool configSeen = false;
	bool cacheOnlySeen = false;
	for (std::size_t index = 1; index < arguments.size();)
	{
		if (arguments[index] == "--cache-only")
		{
			if (cacheOnlySeen)
				return std::nullopt;
			cacheOnlySeen = true;
			result.mode = RefreshMode::CacheOnly;
			++index;
			continue;
		}
		if (index + 1 >= arguments.size() || arguments[index + 1].empty())
			return std::nullopt;
		const std::filesystem::path value(arguments[index + 1]);
		if (!value.is_absolute())
			return std::nullopt;
		if (arguments[index] == "--steam-root" && !steamSeen)
		{
			result.steamRoot = value;
			steamSeen = true;
		}
		else if (arguments[index] == "--config-root" && !configSeen)
		{
			result.configRoot = value;
			configSeen = true;
		}
		else
		{
			return std::nullopt;
		}
		index += 2;
	}
	return steamSeen && configSeen ? std::optional<Invocation>(std::move(result))
	                               : std::nullopt;
}

int PatternRefresh::refreshInstallation(
	const std::filesystem::path& steamRoot,
	const std::filesystem::path& configRoot,
	const PublicKey& publicKey,
	RefreshMode mode,
	const MirrorBases& mirrors,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	std::error_code filesystemError;
	if (!steamRoot.is_absolute() || !configRoot.is_absolute()
	    || (mode != RefreshMode::Remote && mode != RefreshMode::CacheOnly)
	    || !std::filesystem::is_directory(steamRoot, filesystemError) || filesystemError)
	{
		setError(error, "Steam or configuration root is invalid");
		return 2;
	}
	const std::filesystem::path patternRoot = configRoot / "SLSsteam" / "patterns";
	std::optional<ProcessLock::FileLock> refreshLock;
	if (mode == RefreshMode::Remote)
	{
		std::filesystem::create_directories(patternRoot / ".state", filesystemError);
		if (filesystemError)
		{
			setError(error, "embedded fallback refresh state directory is unavailable");
			return 3;
		}
		refreshLock.emplace((patternRoot / ".state/.refresh.lock").string(), true);
		if (!refreshLock->acquired())
		{
			setError(error, "embedded fallback remote refresh is already active");
			return 3;
		}
	}
	const struct
	{
		const char* component;
		const char* moduleName;
	} specifications[] =
	{
		{"steamclient", "steamclient.so"},
		{"steamui", "steamui.so"},
	};

	std::array<std::optional<InspectedModule>, 2> modules;
	std::array<std::string, 2> diagnostics;
	for (std::size_t index = 0; index < modules.size(); ++index)
	{
		modules[index] = inspectModule(
			specifications[index].component,
			specifications[index].moduleName,
			steamRoot / "ubuntu12_32" / specifications[index].moduleName,
			patternRoot / ".state"
			    / (std::string(specifications[index].component) + ".module.state"),
			{},
			&diagnostics[index]
		);
	}

	std::array<std::future<RefreshResult>, 2> futures;
	std::array<bool, 2> started {false, false};
	for (std::size_t index = 0; index < modules.size(); ++index)
	{
		if (!modules[index])
			continue;
		started[index] = true;
		futures[index] = std::async(
			std::launch::async,
			[&, index]
			{
				return refreshComponent(
					patternRoot,
					modules[index]->identity,
					publicKey,
					mode,
					mirrors,
					&diagnostics[index]
				);
			}
		);
	}

	bool allActive = true;
	for (std::size_t index = 0; index < modules.size(); ++index)
	{
		if (!started[index])
		{
			allActive = false;
			continue;
		}
		const RefreshResult result = futures[index].get();
		if (!result.active)
			allActive = false;
	}
	if (!allActive)
	{
		std::string diagnostic = "embedded fallback";
		for (std::size_t index = 0; index < diagnostics.size(); ++index)
		{
			if (!diagnostics[index].empty())
				diagnostic += " " + std::string(specifications[index].component)
				           + "=" + diagnostics[index];
		}
		setError(error, diagnostic);
		return 3;
	}
	return 0;
}
