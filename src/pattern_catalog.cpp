#include "pattern_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unordered_set>


namespace
{
	using Lines = std::vector<std::string_view>;

	void setError(std::string* output, std::string_view value)
	{
		if (output != nullptr)
			output->assign(value);
	}

	bool isLowerHex(std::string_view value, std::size_t exact = 0)
	{
		if (value.empty() || (exact != 0 && value.size() != exact))
			return false;
		return std::all_of(value.begin(), value.end(), [](unsigned char byte)
		{
			return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
		});
	}

	bool isValidUtf8(std::string_view text)
	{
		const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
		for (std::size_t i = 0; i < text.size();)
		{
			const unsigned char first = bytes[i];
			if (first <= 0x7F)
			{
				++i;
				continue;
			}
			std::size_t length = 0;
			std::uint32_t value = 0;
			std::uint32_t minimum = 0;
			if (first >= 0xC2 && first <= 0xDF)
			{
				length = 2;
				value = first & 0x1F;
				minimum = 0x80;
			}
			else if (first >= 0xE0 && first <= 0xEF)
			{
				length = 3;
				value = first & 0x0F;
				minimum = 0x800;
			}
			else if (first >= 0xF0 && first <= 0xF4)
			{
				length = 4;
				value = first & 0x07;
				minimum = 0x10000;
			}
			else
			{
				return false;
			}
			if (i + length > text.size())
				return false;
			for (std::size_t offset = 1; offset < length; ++offset)
			{
				const unsigned char continuation = bytes[i + offset];
				if ((continuation & 0xC0) != 0x80)
					return false;
				value = (value << 6) | (continuation & 0x3F);
			}
			if (value < minimum || value > 0x10FFFF
			    || (value >= 0xD800 && value <= 0xDFFF))
				return false;
			i += length;
		}
		return true;
	}

	bool isSafeText(std::string_view value)
	{
		if (value.empty())
			return false;
		for (unsigned char byte : value)
		{
			if (byte < 0x20 || byte == '"' || byte == '\\')
				return false;
		}
		return true;
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

	bool isResolver(std::string_view value)
	{
		if (value.empty() || value.size() > 64)
			return false;
		const auto lowerOrDigit = [](unsigned char byte)
		{
			return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
		};
		if (!lowerOrDigit(static_cast<unsigned char>(value.front())))
			return false;
		return std::all_of(value.begin() + 1, value.end(), [&](unsigned char byte)
		{
			return lowerOrDigit(byte) || byte == '-';
		});
	}

	bool isSignature(std::string_view value)
	{
		if (value.empty() || value.front() == ' ' || value.back() == ' ')
			return false;
		bool fixed = false;
		std::size_t cursor = 0;
		while (cursor < value.size())
		{
			const std::size_t end = value.find(' ', cursor);
			const auto token = value.substr(
				cursor,
				end == std::string_view::npos ? value.size() - cursor : end - cursor
			);
			if (token.empty())
				return false;
			if (token != "?" && token != "??")
			{
				if (token.size() != 2)
					return false;
				for (unsigned char byte : token)
				{
					if (!((byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'F')))
						return false;
				}
				fixed = true;
			}
			if (end == std::string_view::npos)
				break;
			cursor = end + 1;
		}
		return fixed;
	}

	Lines splitLines(std::string_view body)
	{
		Lines lines;
		const auto withoutFinalLf = body.substr(0, body.size() - 1);
		std::size_t cursor = 0;
		while (cursor <= withoutFinalLf.size())
		{
			const std::size_t end = withoutFinalLf.find('\n', cursor);
			if (end == std::string_view::npos)
			{
				lines.push_back(withoutFinalLf.substr(cursor));
				break;
			}
			lines.push_back(withoutFinalLf.substr(cursor, end - cursor));
			cursor = end + 1;
		}
		return lines;
	}

	std::optional<std::string> stringLine(
		const Lines& lines, std::size_t index, std::string_view key)
	{
		if (index >= lines.size())
			return std::nullopt;
		const std::string prefix = std::string(key) + " = \"";
		const auto line = lines[index];
		if (!line.starts_with(prefix) || line.size() <= prefix.size()
		    || line.back() != '"')
			return std::nullopt;
		const auto value = line.substr(prefix.size(), line.size() - prefix.size() - 1);
		if (!isSafeText(value))
			return std::nullopt;
		return std::string(value);
	}

	std::optional<std::uint64_t> integerLine(
		const Lines& lines, std::size_t index, std::string_view key)
	{
		if (index >= lines.size())
			return std::nullopt;
		const std::string prefix = std::string(key) + " = ";
		const auto line = lines[index];
		if (!line.starts_with(prefix))
			return std::nullopt;
		const auto digits = line.substr(prefix.size());
		if (digits.empty() || (digits.size() > 1 && digits.front() == '0')
		    || !std::all_of(digits.begin(), digits.end(), [](unsigned char byte)
		       { return byte >= '0' && byte <= '9'; }))
			return std::nullopt;
		std::uint64_t result = 0;
		const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), result);
		if (parsed.ec != std::errc() || parsed.ptr != digits.data() + digits.size())
			return std::nullopt;
		return result;
	}
}


PatternCatalog::Catalog::Catalog(
	std::string component,
	std::uint64_t revision,
	std::vector<Locator> locators
)
	: component_(std::move(component)),
	  revision_(revision),
	  locators_(std::move(locators))
{
}

const PatternCatalog::Locator* PatternCatalog::Catalog::entry(
	std::string_view symbol
) const noexcept
{
	const auto found = std::find_if(locators_.begin(), locators_.end(), [&](const Locator& item)
	{
		return item.symbol == symbol;
	});
	return found == locators_.end() ? nullptr : &*found;
}

bool PatternCatalog::Catalog::validatePolicy(
	std::span<const CompiledLocator> compiled
) const noexcept
{
	if (locators_.size() > compiled.size())
		return false;
	std::unordered_set<std::string_view> compiledSymbols;
	for (const auto& item : compiled)
	{
		if (!compiledSymbols.insert(item.symbol).second)
			return false;
	}
	for (const auto& locator : locators_)
	{
		const auto found = std::find_if(compiled.begin(), compiled.end(), [&](const CompiledLocator& item)
		{
			return item.symbol == locator.symbol;
		});
		if (found == compiled.end() || found->required != locator.required ||
			found->signature != locator.signature ||
			found->followMode != locator.followMode)
			return false;
	}
	for (const auto& item : compiled)
	{
		if (item.required && entry(item.symbol) == nullptr)
			return false;
	}
	return true;
}

std::optional<PatternCatalog::Catalog> PatternCatalog::parseCanonical(
	std::string_view body,
	const ModuleIdentity& expected,
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
		return reject("metadata size or final LF is invalid");
	if (!isValidUtf8(body) || body.find('\r') != std::string_view::npos
	    || body.find('\0') != std::string_view::npos)
		return reject("metadata text encoding is invalid");
	const Lines lines = splitLines(body);
	if (lines.size() < 20)
		return reject("metadata is missing required fields");

	const auto schema = integerLine(lines, 0, "schema");
	const auto platform = stringLine(lines, 1, "platform");
	const auto component = stringLine(lines, 2, "component");
	const auto moduleName = stringLine(lines, 3, "module_name");
	const auto moduleSha = stringLine(lines, 4, "module_sha256");
	const auto moduleSize = integerLine(lines, 5, "module_size");
	const auto buildId = stringLine(lines, 6, "gnu_build_id");
	const auto steamVersion = integerLine(lines, 7, "steam_version");
	const auto consumerCommit = stringLine(lines, 8, "consumer_commit");
	const auto minimumSchema = integerLine(lines, 9, "minimum_consumer_schema");
	const auto revision = integerLine(lines, 10, "revision");
	const auto source = stringLine(lines, 11, "source");
	if (!schema || *schema != 1 || !platform || *platform != "linux32"
	    || !component || *component != expected.component
	    || !moduleName || *moduleName != expected.moduleName
	    || !moduleSha || *moduleSha != expected.sha256 || !isLowerHex(*moduleSha, 64)
	    || !moduleSize || *moduleSize != expected.size || *moduleSize == 0
	    || !buildId || *buildId != expected.gnuBuildId || !isLowerHex(*buildId)
	    || !steamVersion || *steamVersion == 0
	    || !consumerCommit || !isLowerHex(*consumerCommit, 40)
	    || !minimumSchema || *minimumSchema != kConsumerSchema
	    || !revision || *revision == 0
	    || !source || (*source != "deterministic" && *source != "model-validated"))
		return reject("metadata header or module identity is invalid");

	std::vector<Locator> locators;
	std::unordered_set<std::string> symbols;
	std::unordered_set<std::string> names;
	std::size_t cursor = 12;
	while (cursor < lines.size())
	{
		if (cursor + 10 >= lines.size() || !lines[cursor].empty()
		    || lines[cursor + 1] != "[[locators]]")
			return reject("locator boundary is invalid");
		const auto symbol = stringLine(lines, cursor + 2, "symbol");
		const auto name = stringLine(lines, cursor + 3, "name");
		const auto target = integerLine(lines, cursor + 4, "target_rva");
		if (!symbol || !isSymbol(*symbol) || !name || !target || *target == 0
		    || *target > std::numeric_limits<std::uint32_t>::max()
		    || *target >= expected.size)
			return reject("locator identity or target RVA is invalid");
		cursor += 5;

		std::optional<std::uint32_t> matchRva;
		if (cursor < lines.size() && lines[cursor].starts_with("match_rva = "))
		{
			const auto match = integerLine(lines, cursor, "match_rva");
			if (!match || *match == 0 || *match > std::numeric_limits<std::uint32_t>::max())
				return reject("locator match RVA is invalid");
			matchRva = static_cast<std::uint32_t>(*match);
			++cursor;
		}
		const auto signature = stringLine(lines, cursor, "signature");
		const auto followMode = stringLine(lines, cursor + 1, "follow_mode");
		const auto resolver = stringLine(lines, cursor + 2, "resolver");
		if (cursor + 5 >= lines.size() || !signature || !isSignature(*signature)
		    || !followMode
		    || (*followMode != "None" && *followMode != "Relative"
		        && *followMode != "PrologueUpwards")
		    || !resolver || !isResolver(*resolver))
			return reject("locator resolver fields are invalid");
		bool required = false;
		if (lines[cursor + 3] == "required = true")
			required = true;
		else if (lines[cursor + 3] != "required = false")
			return reject("locator required policy is invalid");
		const auto matchCount = integerLine(lines, cursor + 4, "match_count");
		const auto targetCount = integerLine(lines, cursor + 5, "target_count");
		if (!matchCount || *matchCount == 0
		    || *matchCount > std::numeric_limits<std::uint32_t>::max()
		    || !targetCount || *targetCount != 1)
			return reject("locator resolution counts are invalid");
		if (!symbols.insert(*symbol).second || !names.insert(*name).second)
			return reject("locator identity is duplicated");
		locators.push_back(
			{
				*symbol,
				*name,
				static_cast<std::uint32_t>(*target),
				matchRva,
				*signature,
				*followMode,
				*resolver,
				required,
				static_cast<std::uint32_t>(*matchCount),
				static_cast<std::uint32_t>(*targetCount),
			}
		);
		cursor += 6;
	}
	if (locators.empty())
		return reject("metadata has no locators");
	return Catalog(*component, *revision, std::move(locators));
}
