#pragma once

#include "pattern_catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace PatternCache
{
	inline constexpr std::uint32_t kSchema = 1;
	inline constexpr std::size_t kMaximumBodySize = 64 * 1024;
	inline constexpr std::size_t kMaximumLocators = 128;

	struct ModuleIdentity
	{
		std::string component;
		std::string moduleName;
		std::string gnuBuildId;
		std::uint64_t size = 0;
		std::int64_t mtimeSeconds = 0;
		std::int64_t mtimeNanoseconds = 0;
	};

	struct Locator
	{
		std::string symbol;
		std::uint64_t targetRva = 0;
		std::uint64_t matchRva = 0;
		std::string signature;
		std::string followMode;
		bool required = true;
	};

	struct Catalog
	{
		ModuleIdentity identity;
		std::vector<Locator> locators;

		const Locator* entry(std::string_view symbol) const noexcept;
	};

	using CompiledLocator = PatternCatalog::CompiledLocator;

	bool identityMatches(
		const ModuleIdentity& cached,
		const ModuleIdentity& current
	) noexcept;

	std::optional<std::size_t> signatureSize(std::string_view signature) noexcept;
	bool signatureMatches(
		std::string_view signature,
		std::span<const std::uint8_t> bytes
	) noexcept;

	// The live resolver derives a target from the matched instruction.  A
	// cached target is usable only when it equals that derived address; None
	// additionally requires the target to be the signature match itself.
	bool targetRvaMatches(
		std::string_view followMode,
		std::uint64_t matchRva,
		std::uint64_t cachedTargetRva,
		std::uint64_t derivedTargetRva
	) noexcept;

	// Validate the complete local hit as one transaction.  A single locator
	// mismatch rejects the catalog so callers can run the embedded resolver and
	// publish a new catalog after a successful cold scan.
	bool policyMatches(
		const Catalog& catalog,
		std::span<const CompiledLocator> compiled
	) noexcept;
	bool validate(
		const Catalog& catalog,
		const ModuleIdentity& current,
		std::span<const CompiledLocator> compiled,
		std::span<const std::uint8_t> moduleBytes
	) noexcept;

	std::string serialize(const Catalog& catalog);
	std::optional<Catalog> parse(
		std::string_view body,
		std::string* error = nullptr
	);

	std::optional<Catalog> load(
		const std::filesystem::path& path,
		const ModuleIdentity& current,
		std::string* error = nullptr
	);

	bool writeAtomic(
		const std::filesystem::path& path,
		std::string_view body
	);

	// `environmentOverride` is a test seam; runtime callers pass getenv's
	// SLSSTEAM_PATTERN_CACHE value.  Invalid values leave the config decision
	// unchanged, while 0/no/false/off and 1/yes/true/on are explicit.
	bool enabled(bool configEnabled, const char* environmentOverride) noexcept;
}
