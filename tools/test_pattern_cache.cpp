#include "pattern_cache.hpp"
#include "pattern_scan.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	int failures = 0;

	void expect(bool condition, std::string_view message)
	{
		if (condition)
			return;
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}

	PatternCache::ModuleIdentity identity()
	{
		return
		{
			"steamclient",
			"steamclient.so",
			"abcdef0123456789abcdef0123456789abcdef01",
			256,
			123456,
			789,
		};
	}

	std::vector<std::uint8_t> fixtureBytes()
	{
		std::vector<std::uint8_t> bytes(256, 0x90);
		bytes[40] = 0x55;
		bytes[41] = 0x89;
		bytes[42] = 0xE5;
		bytes[100] = 0x8B;
		bytes[101] = 0x45;
		bytes[102] = 0x08;
		bytes[120] = 0x55;
		bytes[121] = 0x89;
		bytes[122] = 0xE5;
		bytes[123] = 0x57;
		bytes[124] = 0x56;
		bytes[125] = 0xE8;
		bytes[126] = 0x11;
		bytes[127] = 0x22;
		bytes[128] = 0x33;
		bytes[129] = 0x44;
		bytes[160] = 0x55;
		bytes[161] = 0x57;
		bytes[162] = 0x56;
		bytes[163] = 0x53;
		bytes[164] = 0xE8;
		bytes[165] = 0x11;
		bytes[166] = 0x22;
		bytes[167] = 0x33;
		bytes[168] = 0x44;
		return bytes;
	}

	std::vector<int16_t> patternBytes(std::initializer_list<int16_t> values)
	{
		return {values};
	}

	PatternCache::Catalog coldCatalog(const std::vector<std::uint8_t>& bytes)
	{
		const auto first = MemHlp::scanPatternRange(
			patternBytes({0x55, 0x89, 0xE5}),
			reinterpret_cast<std::uintptr_t>(bytes.data()),
			reinterpret_cast<std::uintptr_t>(bytes.data() + bytes.size()),
			false
		);
		const auto second = MemHlp::scanPatternRange(
			patternBytes({0x8B, 0x45, -1}),
			reinterpret_cast<std::uintptr_t>(bytes.data()),
			reinterpret_cast<std::uintptr_t>(bytes.data() + bytes.size()),
			false
		);
		expect(first.matches == 1 && second.matches == 1,
		       "fixture cold scan resolves every locator once");
		const auto base = reinterpret_cast<std::uintptr_t>(bytes.data());
		return
		{
			identity(),
			{
				{"Patterns::First", first.address - base, first.address - base,
				 "55 89 E5", "None", true},
				{"Patterns::Second", second.address - base, second.address - base,
				 "8B 45 ?", "None", false},
			},
		};
	}

	struct TempDirectory
	{
		std::filesystem::path path;

		TempDirectory()
		{
			path = std::filesystem::temp_directory_path()
			       / "slsteam-pattern-cache-test";
			std::error_code error;
			std::filesystem::remove_all(path, error);
			std::filesystem::create_directories(path, error);
			expect(!error, "temporary cache directory is created");
		}

		~TempDirectory()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	};
}

int main()
{
	const auto bytes = fixtureBytes();
	const auto expected = identity();
	const auto catalog = coldCatalog(bytes);
	const PatternCache::CompiledLocator policy[] =
	{
		{"Patterns::First", true, "55 89 E5", "None"},
		{"Patterns::Second", false, "8B 45 ?", "None"},
	};

	expect(PatternCache::identityMatches(catalog.identity, expected),
	       "build-id, size, and mtime identity matches exactly");
	{
		auto changed = expected;
		++changed.mtimeNanoseconds;
		expect(!PatternCache::identityMatches(catalog.identity, changed),
		       "mtime change invalidates the local identity");
	}
	{
		auto changed = expected;
		++changed.size;
		expect(!PatternCache::identityMatches(catalog.identity, changed),
		       "module size change invalidates the local identity");
	}
	{
		auto changed = expected;
		changed.gnuBuildId[0] = 'f';
		expect(!PatternCache::identityMatches(catalog.identity, changed),
		       "build-id change invalidates the local identity");
	}

	expect(PatternCache::signatureMatches(
		"8B 45 ?", std::span<const std::uint8_t>(bytes.data() + 100, 3)),
		"wildcard signature validates the bytes at a cached match");
	expect(!PatternCache::signatureMatches(
		"8B 46 ?", std::span<const std::uint8_t>(bytes.data() + 100, 3)),
		"fixed signature byte mismatch rejects the cache");
	expect(!PatternCache::signatureMatches(
		"8B 45 ?", std::span<const std::uint8_t>(bytes.data() + 100, 2)),
		"truncated signature bytes reject the cache");

	expect(PatternCache::policyMatches(catalog, policy),
	       "the compiled policy accepts the fixture cache");
	if (const auto first = catalog.entry("Patterns::First"); first != nullptr)
	{
		const auto cold = MemHlp::scanPatternRange(
			patternBytes({0x55, 0x89, 0xE5}),
			reinterpret_cast<std::uintptr_t>(bytes.data()),
			reinterpret_cast<std::uintptr_t>(bytes.data() + bytes.size()),
			false
		);
		expect(first->targetRva == cold.address
		       - reinterpret_cast<std::uintptr_t>(bytes.data()),
		       "cache-hit first RVA equals the cold-scan RVA");
		expect(PatternCache::targetRvaMatches(
			first->followMode, first->matchRva, first->targetRva, first->matchRva),
		       "cached None target is bound to its matched signature");

		auto mutated = *first;
		++mutated.targetRva;
		expect(!PatternCache::targetRvaMatches(
			mutated.followMode, mutated.matchRva, mutated.targetRva,
			mutated.matchRva),
		       "independent target RVA mutation rejects the cache locator");
	}
	expect(PatternCache::targetRvaMatches("Relative", 40, 120, 120),
	       "relative cache target accepts the derived target");
	expect(!PatternCache::targetRvaMatches("Relative", 40, 121, 120),
	       "relative target mutation rejects the cache locator");
	expect(PatternCache::targetRvaMatches("PrologueUpwards", 40, 32, 32),
	       "prologue cache target accepts the derived target");
	expect(!PatternCache::targetRvaMatches("PrologueUpwards", 40, 33, 32),
	       "prologue target mutation rejects the cache locator");
	if (const auto second = catalog.entry("Patterns::Second"); second != nullptr)
	{
		const auto cold = MemHlp::scanPatternRange(
			patternBytes({0x8B, 0x45, -1}),
			reinterpret_cast<std::uintptr_t>(bytes.data()),
			reinterpret_cast<std::uintptr_t>(bytes.data() + bytes.size()),
			false
		);
		expect(second->targetRva == cold.address
		       - reinterpret_cast<std::uintptr_t>(bytes.data()),
		       "cache-hit second RVA equals the cold-scan RVA");
	}

	{
		const PatternCache::CompiledLocator signatureMismatch[] =
		{
			{"Patterns::First", true, "55 8B EC", "None"},
			{"Patterns::Second", false, "8B 45 ?", "None"},
		};
		expect(!PatternCache::policyMatches(catalog, signatureMismatch),
		       "compiled signature mismatch rejects the local cache policy");
		const PatternCache::CompiledLocator followMismatch[] =
		{
			{"Patterns::First", true, "55 89 E5", "Relative"},
			{"Patterns::Second", false, "8B 45 ?", "None"},
		};
		expect(!PatternCache::policyMatches(catalog, followMismatch),
		       "compiled follow-mode mismatch rejects the local cache policy");
	}
	{
		const PatternCache::CompiledLocator incomplete[] =
		{
			{"Patterns::First", true, "55 89 E5", "None"},
		};
		expect(!PatternCache::policyMatches(catalog, incomplete),
		       "a cached locator the build no longer compiles rejects the cache");
	}
	{
		auto incompleteCatalog = catalog;
		incompleteCatalog.locators.pop_back();
		const PatternCache::CompiledLocator required[] =
		{
			{"Patterns::First", true, "55 89 E5", "None"},
			{"Patterns::Second", true, "8B 45 ?", "None"},
		};
		expect(!PatternCache::policyMatches(incompleteCatalog, required),
		       "a missing required locator rejects the complete cache policy");
		const PatternCache::CompiledLocator optional[] =
		{
			{"Patterns::First", true, "55 89 E5", "None"},
			{"Patterns::Second", false, "8B 45 ?", "None"},
		};
		expect(PatternCache::policyMatches(incompleteCatalog, optional),
		       "a missing optional locator may fall back without invalidating required hits");
	}

	{
		PatternCache::Catalog clientHotReload
		{
			expected,
			{
				{"Patterns::CAppInfoCache::GetOrAddAppData", 40, 40,
				 "55 89 E5", "None", false},
				{"Patterns::CAppInfoCache::ThreadedReadFromDisk", 120, 120,
				 "55 89 E5 57 56 E8 ? ? ? ?", "None", false},
				{"Patterns::CAppInfoCache::SkipFlagReference", 100, 100,
				 "8B 45 ?", "None", false},
			},
		};
		const PatternCache::CompiledLocator clientPolicy[] =
		{
			{"Patterns::CAppInfoCache::GetOrAddAppData", false,
			 "55 89 E5", "None"},
			{"Patterns::CAppInfoCache::ThreadedReadFromDisk", false,
			 "55 89 E5 57 56 E8 ? ? ? ?", "None"},
			{"Patterns::CAppInfoCache::SkipFlagReference", false,
			 "8B 45 ?", "None"},
		};
		expect(PatternCache::policyMatches(clientHotReload, clientPolicy),
			"exact local client cache accepts function and instruction locators");
		expect(clientHotReload.entry(
			"Patterns::CAppInfoCache::GetOrAddAppData") != nullptr &&
			!clientHotReload.entry(
				"Patterns::CAppInfoCache::GetOrAddAppData")->required,
			"local client function preserves optionality");
		expect(clientHotReload.entry(
			"Patterns::CAppInfoCache::ThreadedReadFromDisk") != nullptr &&
			!clientHotReload.entry(
				"Patterns::CAppInfoCache::ThreadedReadFromDisk")->required,
			"local live disk reload preserves optionality");

		auto uiIdentity = expected;
		uiIdentity.component = "steamui";
		uiIdentity.moduleName = "steamui.so";
		PatternCache::Catalog steamUi
		{
			uiIdentity,
			{
				{"Patterns::SteamUI::AppControllerRunFrame", 160, 160,
				 "55 57 56 53 E8 ? ? ? ?", "None", false},
			},
		};
		const PatternCache::CompiledLocator uiPolicy[] =
		{
			{"Patterns::SteamUI::AppControllerRunFrame", false,
			 "55 57 56 53 E8 ? ? ? ?", "None"},
		};
		expect(PatternCache::identityMatches(steamUi.identity, uiIdentity) &&
		       PatternCache::policyMatches(steamUi, uiPolicy),
		       "exact local SteamUI cache is independently valid");
		expect(!PatternCache::identityMatches(steamUi.identity, expected),
		       "SteamUI cache cannot be reused for steamclient");
	}

	const std::string serialized = PatternCache::serialize(catalog);
	expect(!serialized.empty() && serialized.back() == '\n',
	       "local catalog serialization is newline terminated");
	std::string parseError;
	const auto parsed = PatternCache::parse(serialized, &parseError);
	expect(parsed.has_value() && parseError.empty(),
	       "local catalog round-trips through the bounded parser");
	if (!parsed)
		std::cerr << "parse diagnostic: " << parseError << '\n';
	if (parsed)
	{
		expect(parsed->identity.mtimeSeconds == expected.mtimeSeconds
		       && parsed->identity.mtimeNanoseconds == expected.mtimeNanoseconds,
		       "serialized seconds and nanoseconds are retained");
		expect(parsed->locators.size() == catalog.locators.size(),
		       "serialized locator count is retained");
	}
	{
		auto oversized = serialized;
		oversized.append(PatternCache::kMaximumBodySize, 'x');
		expect(!PatternCache::parse(oversized, &parseError),
		       "oversized local catalog is rejected before use");
	}

	{
		TempDirectory temporary;
		const auto path = temporary.path / "steamclient.catalog";
		expect(PatternCache::writeAtomic(path, serialized),
		       "local catalog is written atomically");
		const auto loaded = PatternCache::load(path, expected, &parseError);
		expect(loaded.has_value() && parseError.empty(),
		       "written local catalog loads with its exact identity");
		std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
		corrupt << "schema=1\n";
		corrupt.close();
		expect(!PatternCache::load(path, expected, &parseError),
		       "truncated local catalog falls back instead of being used");
	}

	expect(PatternCache::enabled(true, nullptr),
	       "config enables the local pattern cache by default");
	expect(!PatternCache::enabled(false, nullptr),
	       "config kill-switch disables the local pattern cache");
	expect(!PatternCache::enabled(true, "0"),
	       "environment kill-switch disables the local pattern cache");
	expect(!PatternCache::enabled(true, "off"),
	       "text environment kill-switch disables the local pattern cache");
	expect(PatternCache::enabled(false, "1"),
	       "explicit environment enable overrides the config value");

	if (failures != 0)
	{
		std::cerr << failures << " pattern cache test(s) failed\n";
		return 1;
	}
	std::cout << "pattern cache tests passed\n";
	return 0;
}
