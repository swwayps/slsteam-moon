#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace PatternCatalog
{
	inline constexpr std::size_t kMaximumBodySize = 256 * 1024;
	inline constexpr std::uint32_t kConsumerSchema = 1;

	struct ModuleIdentity
	{
		std::string component;
		std::string moduleName;
		std::string sha256;
		std::uint64_t size = 0;
		std::string gnuBuildId;
	};

	struct Locator
	{
		std::string symbol;
		std::string name;
		std::uint32_t targetRva = 0;
		std::optional<std::uint32_t> matchRva;
		std::string signature;
		std::string followMode;
		std::string resolver;
		bool required = true;
		std::uint32_t matchCount = 0;
		std::uint32_t targetCount = 0;
	};

	struct CompiledLocator
	{
		std::string_view symbol;
		bool required;
		std::string_view signature;
		std::string_view followMode;
	};


	class Catalog
	{
	public:
		Catalog(std::string component, std::uint64_t revision,
		        std::vector<Locator> locators);

		std::string_view component() const noexcept { return component_; }
		std::uint64_t revision() const noexcept { return revision_; }
		std::span<const Locator> locators() const noexcept { return locators_; }

		const Locator* entry(std::string_view symbol) const noexcept;
		bool validatePolicy(std::span<const CompiledLocator> compiled) const noexcept;

	private:
		std::string component_;
		std::uint64_t revision_;
		std::vector<Locator> locators_;
	};

	std::optional<Catalog> parseCanonical(
		std::string_view body,
		const ModuleIdentity& expected,
		std::string* error = nullptr
	);
}
