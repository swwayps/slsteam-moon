#include "pattern_cache.hpp"

#include "ascii.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>


namespace
{
	using Lines = std::vector<std::string_view>;

	void setError(std::string* output, std::string_view value)
	{
		if (output != nullptr)
			output->assign(value);
	}

	bool isLowerHex(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum
		    && value.size() % 2 == 0
		    && std::all_of(value.begin(), value.end(), [](unsigned char byte)
		       {
			       return (byte >= '0' && byte <= '9')
			           || (byte >= 'a' && byte <= 'f');
		       });
	}

	bool isModule(std::string_view component, std::string_view moduleName)
	{
		return (component == "steamclient" && moduleName == "steamclient.so")
		    || (component == "steamui" && moduleName == "steamui.so");
	}

	bool isSymbol(std::string_view value)
	{
		if (!value.starts_with("Patterns::") || value.size() <= 10)
			return false;
		return std::all_of(value.begin() + 10, value.end(), [](unsigned char byte)
		{
			return (byte >= 'A' && byte <= 'Z')
			    || (byte >= 'a' && byte <= 'z')
			    || (byte >= '0' && byte <= '9')
			    || byte == '_' || byte == ':';
		});
	}

	bool isFollowMode(std::string_view value)
	{
		return value == "None" || value == "Relative" || value == "PrologueUpwards";
	}

	Lines splitLines(std::string_view body)
	{
		Lines lines;
		std::size_t cursor = 0;
		while (cursor < body.size())
		{
			const std::size_t end = body.find('\n', cursor);
			if (end == std::string_view::npos)
				return {};
			lines.push_back(body.substr(cursor, end - cursor));
			cursor = end + 1;
		}
		return lines;
	}

	std::optional<std::string_view> valueLine(
		const Lines& lines,
		std::size_t index,
		std::string_view key
	)
	{
		if (index >= lines.size())
			return std::nullopt;
		const std::string prefix = std::string(key) + "=";
		if (!lines[index].starts_with(prefix))
			return std::nullopt;
		const std::string_view value = lines[index].substr(prefix.size());
		return value.empty() ? std::nullopt : std::optional<std::string_view>(value);
	}

	template<typename Integer>
	std::optional<Integer> integerLine(
		const Lines& lines,
		std::size_t index,
		std::string_view key
	)
	{
		const auto value = valueLine(lines, index, key);
		if (!value)
			return std::nullopt;
		Integer result {};
		const auto parsed = std::from_chars(
			value->data(), value->data() + value->size(), result
		);
		if (parsed.ec != std::errc() || parsed.ptr != value->data() + value->size())
			return std::nullopt;
		return result;
	}

	std::optional<std::vector<std::int16_t>> parseSignature(
		std::string_view signature
	)
	{
		if (signature.empty() || signature.front() == ' ' || signature.back() == ' ')
			return std::nullopt;

		std::vector<std::int16_t> result;
		result.reserve(64);
		bool fixed = false;
		std::size_t cursor = 0;
		while (cursor < signature.size())
		{
			if (result.size() >= 1024)
				return std::nullopt;
			const std::size_t end = signature.find(' ', cursor);
			const std::string_view token = signature.substr(
				cursor,
				end == std::string_view::npos ? signature.size() - cursor : end - cursor
			);
			if (token.empty())
				return std::nullopt;
			if (token == "?" || token == "??")
			{
				result.push_back(-1);
			}
			else
			{
				if (token.size() != 2
				    || !std::all_of(token.begin(), token.end(), [](unsigned char byte)
				       {
					       return (byte >= '0' && byte <= '9')
					           || (byte >= 'A' && byte <= 'F');
				       }))
					return std::nullopt;
				const auto nibble = [](unsigned char byte) -> std::uint8_t
				{
					return byte <= '9' ? byte - '0' : byte - 'A' + 10;
				};
				result.push_back(static_cast<std::int16_t>(
					(nibble(token[0]) << 4) | nibble(token[1])
				));
				fixed = true;
			}
			if (end == std::string_view::npos)
				break;
			cursor = end + 1;
		}
		return fixed ? std::optional<std::vector<std::int16_t>>(std::move(result))
		            : std::nullopt;
	}

	bool uniqueLocatorSymbols(const PatternCache::Catalog& catalog)
	{
		for (std::size_t left = 0; left < catalog.locators.size(); ++left)
			for (std::size_t right = left + 1; right < catalog.locators.size(); ++right)
				if (catalog.locators[left].symbol == catalog.locators[right].symbol)
					return false;
		return true;
	}

	const PatternCache::CompiledLocator* compiledEntry(
		const PatternCache::Locator& locator,
		std::span<const PatternCache::CompiledLocator> compiled
	)
	{
		const auto found = std::find_if(compiled.begin(), compiled.end(), [&](const auto& item)
		{
			return item.symbol == locator.symbol;
		});
		return found == compiled.end() ? nullptr : &*found;
	}

	bool validCatalogShape(const PatternCache::Catalog& catalog)
	{
		if (!isModule(catalog.identity.component, catalog.identity.moduleName)
		    || !isLowerHex(catalog.identity.gnuBuildId, 128)
		    || catalog.identity.size == 0
		    || catalog.locators.empty()
		    || catalog.locators.size() > PatternCache::kMaximumLocators
		    || !uniqueLocatorSymbols(catalog))
			return false;

		for (const auto& locator : catalog.locators)
		{
			if (!isSymbol(locator.symbol)
			    || locator.targetRva >= catalog.identity.size
			    || locator.matchRva >= catalog.identity.size
			    || !isFollowMode(locator.followMode))
				return false;
			const auto size = parseSignature(locator.signature);
			if (!size || size->size() > catalog.identity.size - locator.matchRva)
				return false;
		}
		return true;
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
		    || value.st_size <= 0 || static_cast<std::uint64_t>(value.st_size) > maximum)
		{
			close(descriptor);
			return std::nullopt;
		}
		std::string result(static_cast<std::size_t>(value.st_size), '\0');
		std::size_t consumed = 0;
		while (consumed < result.size())
		{
			const ssize_t count = read(
				descriptor, result.data() + consumed, result.size() - consumed
			);
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

	bool isToken(std::string_view value, std::string_view token)
	{
		if (value.size() != token.size())
			return false;
		for (std::size_t index = 0; index < value.size(); ++index)
		{
			const unsigned char left = static_cast<unsigned char>(value[index]);
			const unsigned char right = static_cast<unsigned char>(token[index]);
			if (Ascii::toLower(left) != Ascii::toLower(right))
				return false;
		}
		return true;
	}
}


const PatternCache::Locator* PatternCache::Catalog::entry(std::string_view symbol) const noexcept
{
	const auto found = std::find_if(locators.begin(), locators.end(), [&](const Locator& item)
	{
		return item.symbol == symbol;
	});
	return found == locators.end() ? nullptr : &*found;
}

bool PatternCache::identityMatches(
	const ModuleIdentity& cached,
	const ModuleIdentity& current
) noexcept
{
	return cached.component == current.component
	    && cached.moduleName == current.moduleName
	    && cached.gnuBuildId == current.gnuBuildId
	    && cached.size == current.size
	    && cached.mtimeSeconds == current.mtimeSeconds
	    && cached.mtimeNanoseconds == current.mtimeNanoseconds;
}

std::optional<std::size_t> PatternCache::signatureSize(std::string_view signature) noexcept
{
	const auto parsed = parseSignature(signature);
	return parsed ? std::optional<std::size_t>(parsed->size()) : std::nullopt;
}

bool PatternCache::signatureMatches(
	std::string_view signature,
	std::span<const std::uint8_t> bytes
) noexcept
{
	const auto parsed = parseSignature(signature);
	if (!parsed || parsed->size() != bytes.size())
		return false;
	for (std::size_t index = 0; index < parsed->size(); ++index)
	{
		if ((*parsed)[index] >= 0
		    && bytes[index] != static_cast<std::uint8_t>((*parsed)[index]))
			return false;
	}
	return true;
}

bool PatternCache::targetRvaMatches(
	std::string_view followMode,
	std::uint64_t matchRva,
	std::uint64_t cachedTargetRva,
	std::uint64_t derivedTargetRva
) noexcept
{
	if (followMode != "None" && followMode != "Relative"
	    && followMode != "PrologueUpwards")
		return false;
	if (followMode == "None" && derivedTargetRva != matchRva)
		return false;
	return cachedTargetRva == derivedTargetRva;
}

bool PatternCache::policyMatches(
	const Catalog& catalog,
	std::span<const CompiledLocator> compiled
) noexcept
{
	if (!validCatalogShape(catalog) || catalog.locators.size() > compiled.size())
		return false;
	for (const auto& locator : catalog.locators)
	{
		const auto* compiledLocator = compiledEntry(locator, compiled);
		if (compiledLocator == nullptr ||
			compiledLocator->required != locator.required ||
			compiledLocator->signature != locator.signature ||
			compiledLocator->followMode != locator.followMode)
			return false;
	}
	for (const auto& compiledLocator : compiled)
	{
		if (compiledLocator.required && catalog.entry(compiledLocator.symbol) == nullptr)
			return false;
	}
	return true;
}

bool PatternCache::validate(
	const Catalog& catalog,
	const ModuleIdentity& current,
	std::span<const CompiledLocator> compiled,
	std::span<const std::uint8_t> moduleBytes
) noexcept
{
	if (!identityMatches(catalog.identity, current)
	    || moduleBytes.size() != current.size
	    || !policyMatches(catalog, compiled))
		return false;

	for (const auto& locator : catalog.locators)
	{
		const auto signatureBytes = signatureSize(locator.signature);
		if (!signatureBytes || locator.matchRva > moduleBytes.size()
		    || *signatureBytes > moduleBytes.size() - locator.matchRva
		    || !signatureMatches(
				locator.signature,
				moduleBytes.subspan(
					static_cast<std::size_t>(locator.matchRva), *signatureBytes
				)
			))
			return false;
	}
	return true;
}

std::string PatternCache::serialize(const Catalog& catalog)
{
	if (!validCatalogShape(catalog))
		return {};

	std::string result;
	result.reserve(512 + catalog.locators.size() * 192);
	result += "schema=" + std::to_string(kSchema)
	       + "\ncomponent=" + catalog.identity.component
	       + "\nmodule_name=" + catalog.identity.moduleName
	       + "\ngnu_build_id=" + catalog.identity.gnuBuildId
	       + "\nmodule_size=" + std::to_string(catalog.identity.size)
	       + "\nmtime_seconds=" + std::to_string(catalog.identity.mtimeSeconds)
	       + "\nmtime_nanoseconds=" + std::to_string(catalog.identity.mtimeNanoseconds)
	       + "\nlocator_count=" + std::to_string(catalog.locators.size()) + "\n";
	for (const auto& locator : catalog.locators)
	{
		result += "[[locator]]\n";
		result += "symbol=" + locator.symbol
		       + "\ntarget_rva=" + std::to_string(locator.targetRva)
		       + "\nmatch_rva=" + std::to_string(locator.matchRva)
		       + "\nsignature=" + locator.signature
		       + "\nfollow_mode=" + locator.followMode
		       + "\nrequired=" + (locator.required ? "true" : "false") + "\n";
	}
	return result.size() <= kMaximumBodySize ? result : std::string();
}

std::optional<PatternCache::Catalog> PatternCache::parse(
	std::string_view body,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	const auto reject = [&](std::string_view message) -> std::optional<Catalog>
	{
		setError(error, message);
		return std::nullopt;
	};
	if (body.empty() || body.size() > kMaximumBodySize || body.back() != '\n')
		return reject("catalog size or final LF is invalid");
	const Lines lines = splitLines(body);
	if (lines.size() < 9 || lines[0] != "schema=1" || lines[8].empty())
		return reject("catalog header is invalid");

	const auto component = valueLine(lines, 1, "component");
	const auto moduleName = valueLine(lines, 2, "module_name");
	const auto buildId = valueLine(lines, 3, "gnu_build_id");
	const auto size = integerLine<std::uint64_t>(lines, 4, "module_size");
	const auto mtimeSeconds = integerLine<std::int64_t>(lines, 5, "mtime_seconds");
	const auto mtimeNanoseconds = integerLine<std::int64_t>(lines, 6, "mtime_nanoseconds");
	const auto locatorCount = integerLine<std::size_t>(lines, 7, "locator_count");
	if (!component || !moduleName || !buildId || !size || !mtimeSeconds
	    || !mtimeNanoseconds || !locatorCount || *locatorCount == 0
	    || *locatorCount > kMaximumLocators)
		return reject("catalog identity is invalid");
	if (!isModule(*component, *moduleName) || !isLowerHex(*buildId, 128)
	    || *size == 0 || *mtimeNanoseconds < 0 || *mtimeNanoseconds >= 1000000000)
		return reject("catalog module identity is invalid");

	Catalog result
	{
		{std::string(*component), std::string(*moduleName), std::string(*buildId),
		 *size, *mtimeSeconds, *mtimeNanoseconds},
		{},
	};
	result.locators.reserve(*locatorCount);
	std::size_t cursor = 8;
	for (std::size_t index = 0; index < *locatorCount; ++index)
	{
		if (cursor + 6 >= lines.size() || lines[cursor] != "[[locator]]")
			return reject("locator boundary is invalid");
		const auto symbol = valueLine(lines, cursor + 1, "symbol");
		const auto target = integerLine<std::uint64_t>(lines, cursor + 2, "target_rva");
		const auto match = integerLine<std::uint64_t>(lines, cursor + 3, "match_rva");
		const auto signature = valueLine(lines, cursor + 4, "signature");
		const auto followMode = valueLine(lines, cursor + 5, "follow_mode");
		const auto required = valueLine(lines, cursor + 6, "required");
		if (!symbol || !target || !match || !signature || !followMode || !required
		    || (*required != "true" && *required != "false"))
			return reject("locator fields are invalid");
		result.locators.push_back(
			{
				std::string(*symbol), *target, *match, std::string(*signature),
				std::string(*followMode), *required == "true",
			}
		);
		cursor += 7;
	}
	if (cursor != lines.size() || !validCatalogShape(result))
		return reject("locator identity or signature is invalid");
	return result;
}

std::optional<PatternCache::Catalog> PatternCache::load(
	const std::filesystem::path& path,
	const ModuleIdentity& current,
	std::string* error
)
{
	if (error != nullptr)
		error->clear();
	const auto body = readRegularFile(path, kMaximumBodySize);
	if (!body)
	{
		setError(error, "local catalog could not be read");
		return std::nullopt;
	}
	const auto parsed = parse(*body, error);
	if (!parsed)
		return std::nullopt;
	if (!identityMatches(parsed->identity, current))
	{
		setError(error, "local catalog identity is stale");
		return std::nullopt;
	}
	return parsed;
}

bool PatternCache::writeAtomic(
	const std::filesystem::path& path,
	std::string_view body
)
{
	if (!path.is_absolute() || body.empty() || body.size() > kMaximumBodySize)
		return false;
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error || !std::filesystem::is_directory(path.parent_path(), error) || error)
		return false;

	static std::atomic<std::uint64_t> sequence {0};
	const auto temporary = path.parent_path()
		/ ("." + path.filename().string() + ".tmp."
		   + std::to_string(static_cast<unsigned long long>(getpid())) + "."
		   + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
	const int descriptor = open(
		temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600
	);
	if (descriptor < 0)
		return false;

	bool success = true;
	std::size_t written = 0;
	while (written < body.size())
	{
		const ssize_t count = write(descriptor, body.data() + written, body.size() - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
		{
			success = false;
			break;
		}
		written += static_cast<std::size_t>(count);
	}
	if (success && (fchmod(descriptor, 0600) != 0 || fsync(descriptor) != 0))
		success = false;
	if (close(descriptor) != 0)
		success = false;
	if (success && rename(temporary.c_str(), path.c_str()) != 0)
		success = false;
	if (!success)
		unlink(temporary.c_str());
	if (!success)
		return false;

	const int directory = open(
		path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY
	);
	if (directory >= 0)
	{
		fsync(directory);
		close(directory);
	}
	return true;
}

bool PatternCache::enabled(bool configEnabled, const char* environmentOverride) noexcept
{
	if (environmentOverride == nullptr || environmentOverride[0] == '\0')
		return configEnabled;
	const std::string_view value(environmentOverride);
	if (isToken(value, "0") || isToken(value, "no")
	    || isToken(value, "false") || isToken(value, "off"))
		return false;
	if (isToken(value, "1") || isToken(value, "yes")
	    || isToken(value, "true") || isToken(value, "on"))
		return true;
	return configEnabled;
}
