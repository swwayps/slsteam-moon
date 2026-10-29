#include "patterns.hpp"

#include "globals.hpp"
#include "memhlp.hpp"
#include "pattern_catalog.hpp"
#include "pattern_cache.hpp"
#include "runtime_attestation.hpp"
#include "config.hpp"
#include "utils.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>

#include <sys/stat.h>


namespace
{
	std::optional<PatternCatalog::Catalog> g_steamClientCatalog;
	std::optional<PatternCatalog::Catalog> g_steamUiCatalog;

	struct LocalCatalogState
	{
		std::optional<PatternCache::ModuleIdentity> identity;
		std::optional<PatternCache::Catalog> catalog;
		bool writeEligible = false;
		bool dirty = false;
	};

	LocalCatalogState g_steamClientLocal;
	LocalCatalogState g_steamUiLocal;

	std::optional<std::filesystem::path> patternRoot();

	bool localCacheEnabled()
	{
		return PatternCache::enabled(
			g_config.patternCache.get(), std::getenv("SLSSTEAM_PATTERN_CACHE")
		);
	}

	std::optional<PatternCache::ModuleIdentity> moduleIdentity(
		const char* component,
		const char* moduleName,
		const lm_module_t& module
	)
	{
		struct stat value {};
		if (stat(module.path, &value) != 0 || !S_ISREG(value.st_mode) || value.st_size <= 0)
			return std::nullopt;
		const std::string buildId = Utils::getBuildId(module.path);
		if (buildId.empty())
			return std::nullopt;
		return PatternCache::ModuleIdentity
		{
			component,
			moduleName,
			buildId,
			static_cast<std::uint64_t>(value.st_size),
			static_cast<std::int64_t>(value.st_mtim.tv_sec),
			static_cast<std::int64_t>(value.st_mtim.tv_nsec),
		};
	}

	std::filesystem::path localCatalogPath(
		const std::filesystem::path& root,
		const PatternCache::ModuleIdentity& identity
	)
	{
		const std::string key = identity.gnuBuildId + "-"
			+ std::to_string(identity.size) + "-"
			+ std::to_string(identity.mtimeSeconds) + "-"
			+ std::to_string(identity.mtimeNanoseconds) + ".cache";
		return root / "local" / identity.component / key;
	}

	LocalCatalogState loadLocalCatalog(
		const char* component,
		const char* moduleName,
		const lm_module_t& module
	)
	{
		LocalCatalogState state;
		if (!localCacheEnabled())
			return state;
		state.identity = moduleIdentity(component, moduleName, module);
		const auto root = patternRoot();
		if (!state.identity || !root)
			return state;

		std::string error;
		state.catalog = PatternCache::load(
			localCatalogPath(*root, *state.identity), *state.identity, &error
		);
		if (!state.catalog)
			state.writeEligible = true;
		return state;
	}

	LocalCatalogState& localStateFor(const Pattern_t& pattern)
	{
		return pattern.module == &g_modSteamUI ? g_steamUiLocal : g_steamClientLocal;
	}

	const PatternCatalog::Catalog* catalogFor(const Pattern_t& pattern)
	{
		const auto& selected = pattern.module == &g_modSteamUI
			? g_steamUiCatalog
			: g_steamClientCatalog;
		return selected ? &*selected : nullptr;
	}
	void dropCatalogFor(const Pattern_t& pattern)
	{
		(pattern.module == &g_modSteamUI ? g_steamUiCatalog : g_steamClientCatalog).reset();
	}

	const char* followModeName(MemHlp::SigFollowMode mode)
	{
		switch (mode)
		{
			case MemHlp::SigFollowMode::Relative: return "Relative";
			case MemHlp::SigFollowMode::PrologueUpwards: return "PrologueUpwards";
			default: return "None";
		}
	}

	// A cached or published entry names `targetRva` from a signature match at
	// `matchRva`. Both must still hold in this module: the target is executable,
	// the bytes at the match still match the compiled signature and they lead to
	// the target. Returns why they do not, or nullptr.
	const char* verifyCachedTarget(
		const Pattern_t& pattern,
		const lm_module_t& module,
		const MemHlp::CodeSegments& code,
		std::uint64_t matchRva,
		std::uint64_t targetRva,
		lm_address_t& matchOut,
		lm_address_t& targetOut
	)
	{
		if (targetRva >= module.size
		    || targetRva > static_cast<std::uint64_t>(LM_ADDRESS_BAD - module.base))
			return "target RVA is outside the module";
		const lm_address_t candidate = module.base
			+ static_cast<lm_address_t>(targetRva);
		const bool targetInside = candidate >= module.base && candidate < module.end;
		const bool targetExecutable = targetInside && code.find(candidate) != nullptr;
		if (!targetExecutable)
			return "target RVA is not executable";

		const auto signatureBytes = PatternCache::signatureSize(pattern.pattern);
		if (!signatureBytes || matchRva >= module.size
		    || *signatureBytes > module.size - matchRva
		    || matchRva > static_cast<std::uint64_t>(LM_ADDRESS_BAD - module.base))
			return "match RVA is outside the module";
		const lm_address_t match = module.base
			+ static_cast<lm_address_t>(matchRva);
		const lm_segment_t* matchSegment = code.find(match);
		if (matchSegment == nullptr || matchSegment->end - match < *signatureBytes)
			return "signature range is not executable";
		if (!PatternCache::signatureMatches(
			pattern.pattern,
			std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(match), *signatureBytes
			)))
			return "signature bytes changed";

		lm_address_t derivedTarget = match;
		switch (pattern.followMode)
		{
			case MemHlp::SigFollowMode::Relative:
				derivedTarget = MemHlp::getJmpTarget(match);
				break;
			case MemHlp::SigFollowMode::PrologueUpwards:
				derivedTarget = MemHlp::findPrologue(
					match,
					MemHlp::prologueLowerBound(module.base, matchSegment->base),
					pattern.prologue.empty() ? nullptr : pattern.prologue.data(),
					pattern.prologue.size()
				);
				break;
			case MemHlp::SigFollowMode::None:
				break;
		}
		if (derivedTarget == LM_ADDRESS_BAD
		    || derivedTarget < module.base || derivedTarget >= module.end)
			return "followed target is outside the module";
		const auto derivedTargetRva = static_cast<std::uint64_t>(
			derivedTarget - module.base
		);
		if (!PatternCache::targetRvaMatches(
			followModeName(pattern.followMode), matchRva, targetRva,
			derivedTargetRva
		))
			return "target RVA does not match the matched signature";
		matchOut = match;
		targetOut = candidate;
		return nullptr;
	}

	std::optional<lm_address_t> localCatalogAddress(
		const Pattern_t& pattern,
		const MemHlp::CodeSegments& code,
		lm_address_t* matchAddressOut
	)
	{
		LocalCatalogState& state = localStateFor(pattern);
		if (!state.catalog)
			return std::nullopt;

		const auto reject = [&](const char* message) -> std::optional<lm_address_t>
		{
			state.catalog.reset();
			state.writeEligible = true;
			state.dirty = true;
			g_pLog->warn("Local pattern cache for '%s' rejected: %s; using embedded resolver\n",
			             pattern.name.c_str(), message);
			return std::nullopt;
		};
		const PatternCache::Locator* entry = state.catalog->entry(pattern.symbol);
		if (entry == nullptr)
		{
			// Optional patterns may legitimately be absent from a catalog when
			// they were unresolved during the cold pass.  Keep the required
			// cached hits usable and let this optional feature fall back alone.
			if (pattern.optional)
				return std::nullopt;
			return reject("locator is missing from the complete cache policy");
		}
		if (entry->required != !pattern.optional)
			return reject("compiled policy mismatch");
		if (entry->followMode != followModeName(pattern.followMode)
		    || entry->signature != pattern.pattern)
			return reject("compiled resolver metadata mismatch");

		const lm_module_t& module = pattern.module ? *pattern.module : g_modSteamClient;
		lm_address_t match = LM_ADDRESS_BAD;
		lm_address_t candidate = LM_ADDRESS_BAD;
		if (const char* reason = verifyCachedTarget(
			pattern, module, code, entry->matchRva, entry->targetRva, match, candidate))
			return reject(reason);
		if (matchAddressOut != nullptr)
			*matchAddressOut = match;
		return candidate;
	}

	std::optional<lm_address_t> catalogAddress(
		const Pattern_t& pattern,
		const MemHlp::CodeSegments& code,
		lm_address_t* matchAddressOut
	)
	{
		if (const auto local = localCatalogAddress(pattern, code, matchAddressOut))
			return local;
		const PatternCatalog::Catalog* catalog = catalogFor(pattern);
		if (catalog == nullptr)
			return std::nullopt;
		const PatternCatalog::Locator* entry = catalog->entry(pattern.symbol);
		if (entry == nullptr)
			return std::nullopt;
		if (entry->required != !pattern.optional)
		{
			g_pLog->warn("Pattern catalog policy mismatch for '%s'; using embedded resolver\n",
			             pattern.name.c_str());
			return std::nullopt;
		}
		if (entry->signature != pattern.pattern ||
			entry->followMode != followModeName(pattern.followMode))
		{
			g_pLog->warn(
				"Pattern catalog resolver mismatch for '%s'; using embedded resolver\n",
				pattern.name.c_str());
			return std::nullopt;
		}

		const lm_module_t& module = pattern.module ? *pattern.module : g_modSteamClient;
		lm_address_t match = LM_ADDRESS_BAD;
		lm_address_t candidate = LM_ADDRESS_BAD;
		const char* reason = entry->matchRva
			? verifyCachedTarget(pattern, module, code, *entry->matchRva,
			                     entry->targetRva, match, candidate)
			: "it names no match RVA";
		if (reason != nullptr)
		{
			// The rest of this catalog is not trusted either. The embedded
			// resolver takes over, and its result becomes the local cache.
			g_pLog->warn("Pattern catalog entry for '%s' rejected: %s; ignoring the catalog\n",
			             pattern.name.c_str(), reason);
			dropCatalogFor(pattern);
			return std::nullopt;
		}
		if (matchAddressOut != nullptr)
			*matchAddressOut = match;
		return candidate;
	}

	std::optional<std::filesystem::path> patternRoot()
	{
		if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0')
		{
			std::filesystem::path root(xdg);
			if (root.is_absolute())
				return root / "SLSsteam" / "patterns";
			return std::nullopt;
		}
		if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
		{
			std::filesystem::path root(home);
			if (root.is_absolute())
				return root / ".config" / "SLSsteam" / "patterns";
		}
		return std::nullopt;
	}

	std::optional<PatternCatalog::Catalog> loadCatalog(
		const char* component,
		const char* moduleName,
		const lm_module_t& module
	)
	{
		const auto root = patternRoot();
		if (!root)
			return std::nullopt;
		const std::filesystem::path componentRoot = *root / component;
		std::error_code error;
		if (!std::filesystem::is_directory(componentRoot, error) || error)
			return std::nullopt;

		try
		{
			const std::string sha256 = Utils::getFileSHA256(module.path);
			const std::string buildId = Utils::getBuildId(module.path);
			if (sha256.size() != 64 || buildId.empty())
				return std::nullopt;
			const auto moduleSize = std::filesystem::file_size(module.path, error);
			if (error || moduleSize == 0)
				return std::nullopt;
			const std::filesystem::path path = componentRoot / (sha256 + ".toml");
			const auto status = std::filesystem::symlink_status(path, error);
			if (error || status.type() != std::filesystem::file_type::regular)
				return std::nullopt;
			const auto bodySize = std::filesystem::file_size(path, error);
			if (error || bodySize == 0 || bodySize > PatternCatalog::kMaximumBodySize)
				return std::nullopt;
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return std::nullopt;
			std::string body(static_cast<std::size_t>(bodySize), '\0');
			stream.read(body.data(), static_cast<std::streamsize>(body.size()));
			if (!stream || stream.gcount() != static_cast<std::streamsize>(body.size()))
				return std::nullopt;

			std::string parseError;
			auto catalog = PatternCatalog::parseCanonical(
				body,
				{
					component,
					moduleName,
					sha256,
					moduleSize,
					buildId,
				},
				&parseError
			);
			if (!catalog)
			{
				g_pLog->warn("Pattern catalog for %s rejected: %s\n",
				             component, parseError.c_str());
				return std::nullopt;
			}
			return catalog;
		}
		catch (const std::exception&)
		{
			g_pLog->warn("Pattern catalog for %s could not be read; using embedded resolvers\n",
			             component);
			return std::nullopt;
		}
	}

	std::vector<PatternCatalog::CompiledLocator> compiledPolicy(bool steamUi)
	{
		std::vector<PatternCatalog::CompiledLocator> result;
		for (const Pattern_t* pattern : Patterns::patterns())
		{
			if ((pattern->module == &g_modSteamUI) != steamUi)
				continue;
			result.push_back({
				pattern->symbol,
				!pattern->optional,
				pattern->pattern,
				followModeName(pattern->followMode),
			});
		}
		return result;
	}

	void loadActiveCatalogs()
	{
		g_steamClientLocal = loadLocalCatalog(
			"steamclient", "steamclient.so", g_modSteamClient
		);
		g_steamUiLocal = loadLocalCatalog("steamui", "steamui.so", g_modSteamUI);
		g_steamClientCatalog.reset();
		g_steamUiCatalog.reset();

		const auto clientPolicy = compiledPolicy(false);
		if (g_steamClientLocal.catalog
		    && !PatternCache::policyMatches(g_steamClientLocal.catalog.value(), clientPolicy))
		{
			g_pLog->warn("Local pattern cache for steamclient changed compiled locator policy; ignoring it\n");
			g_steamClientLocal.catalog.reset();
			g_steamClientLocal.writeEligible = true;
			g_steamClientLocal.dirty = true;
		}
		const auto uiPolicy = compiledPolicy(true);
		if (g_steamUiLocal.catalog
		    && !PatternCache::policyMatches(g_steamUiLocal.catalog.value(), uiPolicy))
		{
			g_pLog->warn("Local pattern cache for steamui changed compiled locator policy; ignoring it\n");
			g_steamUiLocal.catalog.reset();
			g_steamUiLocal.writeEligible = true;
			g_steamUiLocal.dirty = true;
		}

		// The local catalog is attempted before SHA-256 attestation.  If it is
		// absent or disabled, retain the existing signed remote catalog path;
		// otherwise a warm boot does no full-module hashing at all.
		if (!g_steamClientLocal.catalog)
			g_steamClientCatalog = loadCatalog("steamclient", "steamclient.so", g_modSteamClient);
		if (!g_steamUiLocal.catalog)
			g_steamUiCatalog = loadCatalog("steamui", "steamui.so", g_modSteamUI);

		if (g_steamClientCatalog && !g_steamClientCatalog->validatePolicy(clientPolicy))
		{
			g_pLog->warn("Pattern catalog for steamclient changed compiled locator policy; ignoring it\n");
			g_steamClientCatalog.reset();
		}
		if (g_steamUiCatalog && !g_steamUiCatalog->validatePolicy(uiPolicy))
		{
			g_pLog->warn("Pattern catalog for steamui changed compiled locator policy; ignoring it\n");
			g_steamUiCatalog.reset();
		}
		if (g_steamClientLocal.catalog)
			g_pLog->info("Pattern cache: loaded steamclient local catalog\n");
		if (g_steamUiLocal.catalog)
			g_pLog->info("Pattern cache: loaded steamui local catalog\n");
		if (g_steamClientCatalog)
			g_pLog->info("Pattern catalog: loaded steamclient revision %llu\n",
			             static_cast<unsigned long long>(g_steamClientCatalog->revision()));
		if (g_steamUiCatalog)
			g_pLog->info("Pattern catalog: loaded steamui revision %llu\n",
			             static_cast<unsigned long long>(g_steamUiCatalog->revision()));
	}

	std::optional<PatternCache::Catalog> buildLocalCatalog(bool steamUi)
	{
		LocalCatalogState& state = steamUi ? g_steamUiLocal : g_steamClientLocal;
		if (!state.identity)
			return std::nullopt;
		const lm_module_t& module = steamUi ? g_modSteamUI : g_modSteamClient;
		PatternCache::Catalog result {*state.identity, {}};
		for (const Pattern_t* pattern : Patterns::patterns())
		{
			if ((pattern->module == &g_modSteamUI) != steamUi)
				continue;
			if (pattern->address == LM_ADDRESS_BAD || pattern->matchAddress == LM_ADDRESS_BAD)
			{
				if (!pattern->optional)
					return std::nullopt;
				continue;
			}
			if (pattern->address < module.base || pattern->matchAddress < module.base
			    || pattern->address >= module.end || pattern->matchAddress >= module.end)
				return std::nullopt;
			const lm_address_t targetRva = pattern->address - module.base;
			const lm_address_t matchRva = pattern->matchAddress - module.base;
			if (targetRva >= module.size || matchRva >= module.size)
				return std::nullopt;
			result.locators.push_back(
				{
					pattern->symbol,
					static_cast<std::uint64_t>(targetRva),
					static_cast<std::uint64_t>(matchRva),
					pattern->pattern,
					followModeName(pattern->followMode),
					!pattern->optional,
				}
			);
		}
		return result.locators.empty() ? std::nullopt : std::optional<PatternCache::Catalog>(std::move(result));
	}

	void writeLocalCatalog(bool steamUi)
	{
		LocalCatalogState& state = steamUi ? g_steamUiLocal : g_steamClientLocal;
		if ((!state.writeEligible && !state.dirty)
		    || (steamUi ? g_steamUiCatalog.has_value() : g_steamClientCatalog.has_value()))
			return;
		const auto root = patternRoot();
		const auto catalog = buildLocalCatalog(steamUi);
		if (!root || !catalog)
			return;
		const std::string body = PatternCache::serialize(*catalog);
		if (body.empty())
			return;
		if (PatternCache::writeAtomic(
			localCatalogPath(*root, catalog->identity), body
		))
		{
			g_pLog->info("Pattern cache: wrote %s local catalog (%zu locators)\n",
			             steamUi ? "steamui" : "steamclient", catalog->locators.size());
			state.writeEligible = false;
			state.dirty = false;
		}
	}

	void writeLocalCatalogs()
	{
		writeLocalCatalog(false);
		writeLocalCatalog(true);
	}
}


Pattern_t::Pattern_t(const char* name, const char* pattern,
	MemHlp::SigFollowMode followMode, lm_module_t* module, const char* symbol)
	:
	Pattern_t(name, pattern, followMode, std::vector<int16_t>(), module, symbol)
{
}

Pattern_t::Pattern_t(const char* name, const char* pattern,
	MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue,
	lm_module_t* module, const char* symbol)
	:
	name(name),
	symbol(symbol != nullptr ? symbol : std::string("Patterns::") + name),
	pattern(pattern),
	followMode(followMode),
	prologue(prologue.begin(), prologue.end()),
	module(module)
{
	Patterns::patterns().emplace_back(this);
}

Pattern_t::Pattern_t(const char* name, const char* pattern,
	MemHlp::SigFollowMode followMode, std::vector<int16_t> prologue,
	lm_module_t* module, const char* symbol)
	:
	name(name),
	symbol(symbol != nullptr ? symbol : std::string("Patterns::") + name),
	pattern(pattern),
	followMode(followMode),
	prologue(std::move(prologue)),
	module(module)
{
	Patterns::patterns().emplace_back(this);
}

bool Pattern_t::find(const MemHlp::CodeSegments& code)
{
	lm_module_t& targetModule = module ? *module : g_modSteamClient;
	matchAddress = LM_ADDRESS_BAD;
	if (pattern.empty())
	{
		address = LM_ADDRESS_BAD;
		return false;
	}
	lm_address_t cachedMatch = LM_ADDRESS_BAD;
	if (const auto trusted = catalogAddress(*this, code, &cachedMatch))
	{
		address = *trusted;
		matchAddress = cachedMatch;
	}
	else
	{
		const auto resolved = MemHlp::searchSignatureDetailed
		(
			name.c_str(), pattern.c_str(), targetModule, followMode,
			prologue.empty() ? nullptr : prologue.data(), prologue.size(), &code
		);
		matchAddress = resolved.match;
		address = resolved.target;

		// The hooked address is usually NOT the matched one: Relative follows a
		// call/jmp operand and PrologueUpwards walks backwards, so the derived
		// address carries none of the scan's guarantees.  Prove it lands in this
		// module's executable memory before anyone detours it.  The cache and
		// catalog paths already do this on their own inputs; this closes the
		// embedded resolver, which is the path every unknown client build takes.
		if (address != LM_ADDRESS_BAD)
		{
			const bool inside = address >= targetModule.base
			                 && address < targetModule.base + targetModule.size;
			const bool executable = code.find(address) != nullptr;
			if (!inside || !executable)
			{
				g_pLog->warn(
					"Resolved target for '%s' is not executable module memory; "
					"refusing to use it\n", name.c_str());
				address = LM_ADDRESS_BAD;
				matchAddress = LM_ADDRESS_BAD;
			}
		}
	}
	const bool resolved = address != LM_ADDRESS_BAD;

	if (RuntimeAttestation::enabled())
	{
		const char* moduleName = module == &g_modSteamUI ? "steamui" : "steamclient";
		if (!resolved)
		{
			RuntimeAttestation::emit
			(
				"locator-missing",
				{
					RuntimeAttestation::Field::text("locator", name),
					RuntimeAttestation::Field::text("module", moduleName),
					RuntimeAttestation::Field::boolean("optional", optional),
				}
			);
		}
		else
		{
			const bool executable = code.find(address) != nullptr;
			const bool insideModule = address >= targetModule.base
			                       && address < targetModule.base + targetModule.size;
			RuntimeAttestation::emit
			(
				"locator-resolved",
				{
					RuntimeAttestation::Field::text("locator", name),
					RuntimeAttestation::Field::text("module", moduleName),
					RuntimeAttestation::Field::number
					(
						"target_rva", insideModule ? address - targetModule.base : 0
					),
					RuntimeAttestation::Field::boolean("inside_module", insideModule),
					RuntimeAttestation::Field::boolean("executable", executable),
				}
			);
		}
	}

	return resolved;
}

bool Patterns::init()
{
	LOG_DEBUG("Patterns::init()\n");

	bool found = true;

	// Establish immutable compiled policy before reading any remote-derived
	// metadata.  A catalog whose required flags or symbols disagree with this
	// registry is rejected in full.

	// Locators without an x86-64 signature yet: their features stay off.
	CPackageInfoCache::LoadPackage.optional = true;
	CUtlMemory::Grow.optional = true;
	CJobMgr::BRouteMsgToJob.optional = true;
	CDepotDownloadMgr::BYldRequestDepotManifest.optional = true;
	CUser::MarkLicenseAsChanged.optional = true;
	CUser::ProcessPendingLicenseUpdates.optional = true;
	CUser::NotifyLicensesUpdated.optional = true;
	// Only LaunchOptions depends on it.
	CUser::SpawnGameId.optional = true;
	CAppInfoCache::GetOrAddAppData.optional = true;
	CAppInfoCache::ThreadedReadFromDisk.optional = true;
	CAppInfoCache::SkipFlagReference.optional = true;
	CAppInfoCache::ShaReference.optional = true;
	CDepotDownloadMgr::ProcessDepotManifest.optional = true;
	CDepotDownloadMgr::PrepareDepotDownload.optional = true;
	CDepotDownloadMgr::BuildDepotDependency.optional = true;
	CUserAppManager::BuildDepotDependency.optional = true;
	CDepotDownloadMgr::EvaluateConfigChanges.optional = true;
	CDepotDownloadMgr::OnChunkUnpackedStack.optional = true;
	CDepotDownloadMgr::OnChunkUnpackedReg.optional = true;
	TraceIPC.optional = true;
	ParentalSignatureCheck.optional = true;
	ParentalSettingsReceived.optional = true;
	SteamUI::AppControllerRunFrame.optional = true;
	SteamUI::GetAppByID.optional = true;
	SteamUI::MarkAppChange.optional = true;
	SteamUI::OwnershipFlagsReference.optional = true;

	loadActiveCatalogs();

	// One read of the process segments serves every locator below.
	const auto code = MemHlp::CodeSegments::read();
	for(auto& pattern : patterns())
	{
		if (!pattern->find(code))
		{
			if (pattern->optional)
			{
				// Optional patterns degrade to a safe no-op in their
				// dependent feature; don't fail the whole load.
				g_pLog->warn
				(
					pattern->pattern.empty()
						? "Pattern '%s' has no x86-64 signature yet; dependent feature disabled\n"
						: "Optional pattern '%s' not found; dependent feature disabled\n",
					pattern->name.c_str()
				);
				continue;
			}
			// Required pattern missing: log WHICH one so a Steam-client
			// update that drifts a signature is diagnosable from the log
			// instead of just "Failed to find all patterns".
			g_pLog->warn
			(
				"Required pattern '%s' not found\n",
				pattern->name.c_str()
			);
			found = false;
		}
	}

	if (found)
		writeLocalCatalogs();
	return found;
}

using SigFollowMode = MemHlp::SigFollowMode;

namespace Patterns
{
	// x86-64 client. Signatures for locators shared with upstream come from
	// upstream dev-X86_64. An empty signature marks a locator that has not been
	// derived for x86-64 yet: it never resolves and its feature stays off.

	Pattern_t ParentalSignatureCheck
	{
		"ParentalSignatureCheck",
		"",
		SigFollowMode::None
	};
	Pattern_t ParentalSettingsReceived
	{
		"ParentalSettingsReceived",
		"",
		SigFollowMode::None
	};

	Pattern_t TraceIPC
	{
		"TraceIPC",
		"48 89 D7 C1 E8",
		SigFollowMode::PrologueUpwards,
		std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 }
	};

	namespace SteamUI
	{
		// x86-64 steamui.so (beta 1793235415 build-id 4089b816..., addresses
		// below). Each signature has one match on that build, on 1792711043
		// and on the 2026-10-21 stable x86-64 client. LibraryRemoval detours
		// RunFrame and MarkAppChange; their first 14 bytes hold no
		// RIP-relative operand, which libmem copies into the trampoline as is.
		//
		// Slot 3 of the 21CSteamUIAppController primary vtable (0x19d5300),
		// as on i386: (this). It compares the frame time against members
		// before the per-frame work.
		Pattern_t AppControllerRunFrame
		{
			"CSteamUIAppController::RunFrame",
			"41 57 41 56 41 55 41 54 55 53 48 83 EC 38 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 28 48 89 FB E8 ? ? ? ? 66 0F EF C9 66 0F EF D2 F2 0F 5A C0 F3 0F 5A D0 F3 0F 5A 8B ? ? 00 00",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::AppControllerRunFrame"
		};
		// (controller, appid, create) at 0x19b80a0: the murmur3 hash lookup
		// MarkAppChange calls with create=0. Called, never detoured.
		Pattern_t GetAppByID
		{
			"CSteamUIAppController::GetAppByID",
			"41 57 41 56 49 89 FE 41 55 41 54 55 53 48 83 EC 38 89 74 24 0C 89 54 24 08 64 48 8B 04 25 28 00 00 00 48 89 44 24 28 48 8B 05 ? ? ? ? 8B 00 85 C0 0F 85 ? ? ? ? 48 C7 44 24 10 00 00 00 00 85 F6 89 F5",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::GetAppByID"
		};
		// (source, appid, change_flags) at 0x1b1b100, the function that walks
		// the "AppChangeNotification" listeners; it skips appid 7 and looks
		// the app up through GetAppByID first, as on i386.
		Pattern_t MarkAppChange
		{
			"CUpdateManager::MarkAppChange",
			"41 57 41 89 F7 41 56 41 55 41 54 55 48 89 FD 53 48 83 EC 28 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 18 89 D3 8B 10 85 DB 0F 94 C0 85 F6 0F 94 C1",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::MarkAppChange"
		};
		// FillInAppOverview's inlined OwnershipFlags read right before it
		// emits "eAppOwnershipFlags": `mov edx,[r14+0x1c]` (0x19c1fde), then
		// rdi=sink, rsi=name and the emitter call. The 0x1c displacement
		// stays pinned for the same reason as on i386 (the neighbouring
		// fields share the shape); the modrm byte is the wildcard and
		// LibraryRemovalPolicy::deriveOwnershipOffset checks it.
		Pattern_t OwnershipFlagsReference
		{
			"CSteamApp::OwnershipFlagsReference",
			"41 8B ? 1C 48 8B BD ? ? ? ? 48 8D 35 ? ? ? ? E8",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::OwnershipFlagsReference"
		};
	}

	namespace CSteamEngine
	{
		Pattern_t GetServerPipe
		{
			"CSteamEngine::GetServerPipe",
			"0F B7 CE 31 D2",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x2C, 0x74, 0xF6, 0x85 },
			nullptr,
			"Patterns::CSteamEngine::GetServerPipe"
		};
		Pattern_t SetAppIdForCurrentPipe
		{
			"CSteamEngine::SetAppIdForCurrentPipe",
			"0F 43 C6 83 FB",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 }
		};
		Pattern_t ProcessIPCFrame
		{
			"CSteamEngine::ProcessIPCFrame",
			"48 89 DA 44 89 E6 5B",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0xCD, 0x89, 0x49, 0x55, 0x41 },
			nullptr,
			"Patterns::CSteamEngine::ProcessIPCFrame"
		};
		// add r12, [rbp + disp32]: the CUser map offset is the disp32 at +3.
		Pattern_t Offset_User
		{
			"CSteamEngine::m_pUser",
			"4C 03 A5 ? ? ? ? 48 89 DF 4C 89 E2 E8 ? ? ? ? 49 8B 74 24",
			SigFollowMode::None,
			nullptr,
			"Patterns::CSteamEngine::Offset_User"
		};
	}

	namespace CUser
	{
		Pattern_t CheckAppOwnership
		{
			"CUser::CheckAppOwnership",
			"48 89 E8 31 ED",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0xD6, 0x89, 0x49, 0x56, 0x41, 0x57, 0x41 }
		};
		Pattern_t GetSubscribedApps
		{
			"CUser::GetSubscribedApps",
			"EB ? 41 8B 50",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 }
		};
		Pattern_t PostCallback
		{
			"CUser::PostCallback",
			"E8 ? ? ? ? 4D 8B AE",
			SigFollowMode::Relative,
			nullptr,
			"Patterns::CUser::PostCallback"
		};
		Pattern_t PostCallbackToAppId
		{
			"CUser::PostCallbackToAppId",
			"44 3B 69 ? 41 89 C5",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 }
		};
		Pattern_t SpawnGameId
		{
			"CUser::SpawnGameId",
			"31 C0 41 80 78",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0xE5, 0x89, 0x48, 0x55 }
		};
		Pattern_t UpdateAppOwnershipTicket
		{
			"CUser::UpdateAppOwnershipTicket",
			"41 89 CD 83 FD",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 },
			nullptr,
			"Patterns::CUser::UpdateAppOwnershipTicket"
		};
		// x86-64 (beta 1793235415 0x18e4ee0): (CUser*, uint32 packageId,
		// bool). Same body as the i386 function: the murmur3 finalizer of the
		// package id indexes the changed-license table at this+0x2380.
		Pattern_t MarkLicenseAsChanged
		{
			"CUser::MarkLicenseAsChanged",
			"41 57 48 8D 87 ? ? 00 00 41 89 F7 41 56 41 55 41 54 55 53 48 83 EC 28 88 54 24 18 8B 97 ? ? 00 00 89 74 24 1C",
			SigFollowMode::None
		};
		// x86-64 (0x18e5920): (CUser*) -> bool. Walks the pending-license
		// vector (base this+0x24b8, size +0x24c8, 0x50-byte entries), marks
		// each through MarkLicenseAsChanged and, when the changed-license count
		// at this+0x23c4 is positive, ends with NotifyLicensesUpdated and
		// returns true.
		Pattern_t ProcessPendingLicenseUpdates
		{
			"CUser::ProcessPendingLicenseUpdates",
			"41 54 49 89 FC 55 53 8B 9F ? ? 00 00 83 EB 01 78 ? 48 63 C3 48 8D 2C 80 48 C1 E5 04 EB ? 90 83 EB 01 48 83 ED 50 83 FB FF 74 ? 49 8B 84 24 ? ? 00 00 48 01 E8 83 78 14 50",
			SigFollowMode::None
		};
		// x86-64 (0x18e5180): (CUser*). Tests the changed-license count at
		// this+0x23c4 (CUser::BUpdateLicenses, named by its assert string,
		// reads the same field), then posts callback 0x7d with the
		// "17LicensesUpdated_t" type name, like the i386 function.
		Pattern_t NotifyLicensesUpdated
		{
			"CUser::NotifyLicensesUpdated",
			"41 57 41 56 41 55 41 54 55 53 48 81 EC ? ? 00 00 44 8B 8F ? ? 00 00 48 89 7C 24 28 64 48 8B 04 25 28 00 00 00 48 89 84 24 ? ? 00 00 31 C0 45 85 C9 0F 8E",
			SigFollowMode::None
		};
	}

	namespace CAppInfoCache
	{
		// x86-64 (beta 1793235415 0x1053f80): (rdi=cache, esi=appId,
		// edx=create), one of three functions under the "GetOrAddAppData"
		// VProf scope and the only one with the create flag. Its first three
		// instructions are 2-byte pushes, which the appinfostate detour moves
		// into its trampoline.
		Pattern_t GetOrAddAppData
		{
			"CAppInfoCache::GetOrAddAppData",
			"41 57 41 56 41 55 41 54 49 89 FC 55 89 F5 53 48 83 EC 18 48 8B 05 ? ? ? ? 64 48 8B 1C 25 28 00 00 00 48 89 5C 24 08 89 D3",
			SigFollowMode::None
		};
		// The CAppInfoCacheReadFromDiskThread body calls it with the cache
		// (rdi=[thread+0x48]) between two clock reads; the reader (0x10550d0,
		// "ThreadedReadFromDisk" VProf scope and log lines) shares its
		// prologue with eleven other functions, so the call site locates it.
		Pattern_t ThreadedReadFromDisk
		{
			"CAppInfoCache::ThreadedReadFromDisk",
			"E8 ? ? ? ? E8 ? ? ? ? 48 8B 14 24 48 89 44 24 08",
			SigFollowMode::Relative
		};
		// CUser's license loop (0x18ec250) reads the CAppData that
		// GetOrAddAppData returns: `movzx eax,byte [rax+0x14]` (the skip
		// flag) after comparing the 20-byte SHA at +0x20 with
		// `cmp [rax+0x20],rsi` / +0x28 / +0x30. AppDataLayout decodes both
		// displacements in place. Both sites match once on the betas; the
		// 2026-10-21 x86-64 build allocates other registers and stays off.
		Pattern_t SkipFlagReference
		{
			"CAppInfoCache::SkipFlagReference",
			"0F B6 40 ? 48 83 C3 01 20 44 24 ? 39 5D ?",
			SigFollowMode::None
		};
		Pattern_t ShaReference
		{
			"CAppInfoCache::ShaReference",
			"48 39 70 ? 75 ? 48 8B 7A 08 48 39 78 ? 75 ? 8B 7A 10 39 78 ?",
			SigFollowMode::None
		};
	}

	namespace CUserAppManager
	{
		Pattern_t BuildDepotDependency
		{
			"CUserAppManager::BuildDepotDependency",
			"4D 85 FF 88 44 24",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0xCF, 0x89, 0x4D, 0x57, 0x41 }
		};
	}

	namespace CPackageInfoCache
	{
		// x86-64 entry, called from CPackageInfoCache::ReadFromDisk (the
		// "%s/appcache/packageinfo.vdf" reader) right after
		// MemAlloc(0xa0) + CPackageInfo::CPackageInfo, with the result tested
		// as a bool. Args: rdi=PackageInfo*, rsi=sha1, edx=change number,
		// rcx=KeyValues node; `movsxd rbp,[rcx+0x10]` reads the node. The body
		// writes Status +0x18 and LicenseType +0x14 and rebuilds AppIdVec
		// (+0x40, size +0x50) and DepotIdVec (+0x58, size +0x68) through
		// CUtlMemory::Grow. One match on beta 1792711043, 1793235415 and the
		// 2026-10-21 x86-64 stable build.
		Pattern_t LoadPackage
		{
			"CPackageInfoCache::LoadPackage",
			"41 57 41 56 49 89 F6 41 55 41 54 55 53 48 89 FB 48 81 EC ? ? 00 00 4C 8B 25 ? ? ? ? 89 54 24 ? 48 63 69 10",
			SigFollowMode::None
		};
	}

	namespace CUtlMemory
	{
		// LoadPackage's AppIdVec append: `lea rdi,[rbx+0x40]; sub esi,eax;
		// call Grow; mov eax,[rbx+0x50]; lea esi,[rax+1]; jmp`. The 4-byte
		// element Grow(CUtlMemory*, int) has ~147 byte-identical copies, so
		// the call site picks the one LoadPackage itself uses.
		Pattern_t Grow
		{
			"CUtlMemory::Grow",
			"E8 ? ? ? ? 8B 43 50 8D 70 01 EB",
			SigFollowMode::Relative
		};
	}

	namespace CDepotDownloadMgr
	{
		// x86-64: (rdi=mgr, rsi=user, edx=appId, ecx=depotId, r8=gid,
		// r9=branch). ProcessDepotManifest formats
		// "%s/depotcache/%d_%llu.manifest" from ecx/r8 and, when the file is
		// missing, calls BYldRequestDepotManifest(user+0x18b8, app, depot,
		// gid, branch, mgr). PrepareDepotDownload saves the same registers,
		// calls ProcessDepotManifest with them and then looks the gid up in
		// the table at mgr+0x60/+0x78. Prologues saving those arguments match
		// once on beta 1792711043, 1793235415 and the 2026-10-21 stable build.
		Pattern_t ProcessDepotManifest
		{
			"CDepotDownloadMgr::ProcessDepotManifest",
			"41 57 41 56 41 89 CE 41 55 41 54 49 89 FC 55 53 4C 89 C3 48 83 EC 38 48 89 34 24 89 54 24 0C 4C 89 4C 24 10",
			SigFollowMode::None
		};
		Pattern_t PrepareDepotDownload
		{
			"CDepotDownloadMgr::PrepareDepotDownload",
			"41 57 41 56 41 55 41 54 55 53 48 89 FB 48 83 EC 78 48 89 74 24 08 89 4C 24 04 64 4C 8B 24 25 28 00 00 00 4C 89 64 24 68 4D 89 C4 E8",
			SigFollowMode::None
		};
		// Not ported: the x86-64 planner (the pair of calls with flag 0/1 into
		// contentupdatecontext.cpp, e.g. 0x11fc8e0 on beta 1793235415) takes
		// (ctx, flag, dword, vector) and walks 0x28-byte entries, while
		// ManifestBind's builder hook expects the i386 (ctx, flag, vector, dword)
		// and 0x20-byte DepotEntry layout. The leaf above already stages the
		// manifests; manifest pins need the builder.
		Pattern_t BuildDepotDependency
		{
			"CDepotDownloadMgr::BuildDepotDependency",
			"",
			SigFollowMode::Relative
		};
		Pattern_t OnChunkUnpackedStack
		{
			"CDepotDownloadMgr::OnChunkUnpacked[cdecl]",
			"",
			SigFollowMode::None,
			nullptr,
			"Patterns::CDepotDownloadMgr::OnChunkUnpackedStack"
		};
		Pattern_t OnChunkUnpackedReg
		{
			"CDepotDownloadMgr::OnChunkUnpacked[regparm3]",
			"",
			SigFollowMode::None,
			nullptr,
			"Patterns::CDepotDownloadMgr::OnChunkUnpackedReg"
		};
		Pattern_t EvaluateConfigChanges
		{
			"CDepotDownloadMgr::EvaluateConfigChanges",
			"",
			SigFollowMode::None
		};
	}

	namespace IClientUtils
	{
		// mov esi, [rdi + disp32]: the pipe index field offset is the disp32 at +2.
		Pattern_t Offset_GetPipeIndex
		{
			"IClientUtils::m_PipeIndex",
			"8B B7 ? ? ? ? 64 48 8B 1C 25 ? ? ? ? 48 89 5C 24 ? 48 89 FB 48 81 EF",
			SigFollowMode::None,
			nullptr,
			"Patterns::IClientUtils::Offset_GetPipeIndex"
		};
	}

	namespace CWebSocketConnection
	{
		Pattern_t BBuildAndAsyncSendFrame
		{
			"CWebSocketConnection::BBuildAndAsyncSendFrame",
			"45 31 F6 83 7F",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x55, 0x41, 0x56, 0x41, 0x57, 0x41 }
		};
	}

	namespace CJobMgr
	{
		Pattern_t BRouteMsgToJob
		{
			"CJobMgr::BRouteMsgToJob",
			"",
			SigFollowMode::None
		};
	}

	namespace CDepotDownloadMgr
	{
		Pattern_t BYldRequestDepotManifest
		{
			"CDepotDownloadMgr::BYldRequestDepotManifest",
			"",
			SigFollowMode::None
		};
	}

	std::vector<Pattern_t*>& patterns()
	{
		// Function-local static: guaranteed initialized on first call,
		// which happens from the first Pattern_t constructor above —
		// before init() ever iterates it.  Immune to static-init order.
		static std::vector<Pattern_t*> instance;
		return instance;
	}
}
