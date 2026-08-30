#include "patterns.hpp"

#include "globals.hpp"
#include "memhlp.hpp"
#include "feats/ipcframe.hpp"
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

	const char* followModeName(MemHlp::SigFollowMode mode)
	{
		switch (mode)
		{
			case MemHlp::SigFollowMode::Relative: return "Relative";
			case MemHlp::SigFollowMode::PrologueUpwards: return "PrologueUpwards";
			default: return "None";
		}
	}

	std::optional<lm_address_t> localCatalogAddress(
		const Pattern_t& pattern,
		bool logInvalid,
		lm_address_t* matchAddressOut
	)
	{
		LocalCatalogState& state = localStateFor(pattern);
		if (!state.catalog)
			return std::nullopt;

		const auto reject = [&](const char* message) -> std::optional<lm_address_t>
		{
			// The structural IPC-root probe runs before the embedded signature is
			// normalized.  A failed non-logging probe is therefore provisional:
			// let autoResolveIpcFrameRoots adjust the root and recheck the local
			// catalog during the definitive Pattern_t::find() pass.  Normal
			// resolving (logInvalid=true) remains transactional.
			if (logInvalid)
			{
				state.catalog.reset();
				state.writeEligible = true;
				state.dirty = true;
			}
			if (logInvalid)
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
		if (entry->targetRva >= module.size
		    || entry->targetRva > static_cast<std::uint64_t>(LM_ADDRESS_BAD - module.base))
			return reject("target RVA is outside the module");
		const lm_address_t candidate = module.base
			+ static_cast<lm_address_t>(entry->targetRva);
		lm_segment_t targetSegment {};
		const bool targetInside = candidate >= module.base && candidate < module.end;
		const bool targetExecutable = targetInside && LM_FindSegment(candidate, &targetSegment)
			&& (targetSegment.prot & LM_PROT_XR) == LM_PROT_XR;
		if (!targetExecutable)
			return reject("target RVA is not executable");

		const auto signatureBytes = PatternCache::signatureSize(entry->signature);
		if (!signatureBytes || entry->matchRva >= module.size
		    || *signatureBytes > module.size - entry->matchRva
		    || entry->matchRva > static_cast<std::uint64_t>(LM_ADDRESS_BAD - module.base))
			return reject("match RVA is outside the module");
		const lm_address_t match = module.base
			+ static_cast<lm_address_t>(entry->matchRva);
		lm_segment_t matchSegment {};
		if (!LM_FindSegment(match, &matchSegment)
		    || (matchSegment.prot & LM_PROT_XR) != LM_PROT_XR
		    || match < matchSegment.base
		    || match >= matchSegment.end
		    || matchSegment.end - match < *signatureBytes)
			return reject("signature range is not executable");
		if (!PatternCache::signatureMatches(
			entry->signature,
			std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(match), *signatureBytes
			)))
			return reject("signature bytes changed");

		lm_address_t derivedTarget = match;
		switch (pattern.followMode)
		{
			case MemHlp::SigFollowMode::Relative:
				derivedTarget = MemHlp::getJmpTarget(match);
				break;
			case MemHlp::SigFollowMode::PrologueUpwards:
				derivedTarget = MemHlp::findPrologue(
					match,
					MemHlp::prologueLowerBound(module.base, matchSegment.base),
					pattern.prologue.empty() ? nullptr : pattern.prologue.data(),
					pattern.prologue.size()
				);
				break;
			case MemHlp::SigFollowMode::None:
				break;
		}
		if (derivedTarget == LM_ADDRESS_BAD
		    || derivedTarget < module.base || derivedTarget >= module.end)
			return reject("followed target is outside the module");
		const auto derivedTargetRva = static_cast<std::uint64_t>(
			derivedTarget - module.base
		);
		if (!PatternCache::targetRvaMatches(
			entry->followMode, entry->matchRva, entry->targetRva,
			derivedTargetRva
		))
			return reject("target RVA does not match the matched signature");

		if (matchAddressOut != nullptr)
			*matchAddressOut = match;
		return candidate;
	}

	std::optional<lm_address_t> catalogAddress(
		const Pattern_t& pattern,
		bool logInvalid,
		lm_address_t* matchAddressOut = nullptr
	)
	{
		if (const auto local = localCatalogAddress(pattern, logInvalid, matchAddressOut))
			return local;
		const PatternCatalog::Catalog* catalog = catalogFor(pattern);
		if (catalog == nullptr)
			return std::nullopt;
		const PatternCatalog::Locator* entry = catalog->entry(pattern.symbol);
		if (entry == nullptr)
			return std::nullopt;
		if (entry->required != !pattern.optional)
		{
			if (logInvalid)
				g_pLog->warn("Pattern catalog policy mismatch for '%s'; using embedded resolver\n",
				             pattern.name.c_str());
			return std::nullopt;
		}
		if (entry->signature != pattern.pattern ||
			entry->followMode != followModeName(pattern.followMode))
		{
			if (logInvalid)
				g_pLog->warn(
					"Pattern catalog resolver mismatch for '%s'; using embedded resolver\n",
					pattern.name.c_str());
			return std::nullopt;
		}

		const lm_module_t& module = pattern.module ? *pattern.module : g_modSteamClient;
		const lm_address_t rva = static_cast<lm_address_t>(entry->targetRva);
		if (rva >= module.size || module.base > LM_ADDRESS_BAD - rva)
			return std::nullopt;
		const lm_address_t candidate = module.base + rva;
		const bool inside = candidate >= module.base && candidate < module.end;
		lm_segment_t segment {};
		const bool executable = inside && LM_FindSegment(candidate, &segment)
		                     && (segment.prot & LM_PROT_XR) == LM_PROT_XR;
		if (!inside || !executable)
		{
			if (logInvalid)
				g_pLog->warn("Pattern catalog RVA for '%s' is not executable; using embedded resolver\n",
				             pattern.name.c_str());
			return std::nullopt;
		}
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

	std::vector<PatternCache::CompiledLocator> compiledLocalPolicy(bool steamUi)
	{
		std::vector<PatternCache::CompiledLocator> result;
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
		const auto localClientPolicy = compiledLocalPolicy(false);
		if (g_steamClientLocal.catalog
		    && !PatternCache::policyMatches(g_steamClientLocal.catalog.value(), localClientPolicy))
		{
			g_pLog->warn("Local pattern cache for steamclient changed compiled locator policy; ignoring it\n");
			g_steamClientLocal.catalog.reset();
			g_steamClientLocal.writeEligible = true;
			g_steamClientLocal.dirty = true;
		}
		const auto uiPolicy = compiledPolicy(true);
		const auto localUiPolicy = compiledLocalPolicy(true);
		if (g_steamUiLocal.catalog
		    && !PatternCache::policyMatches(g_steamUiLocal.catalog.value(), localUiPolicy))
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
	Pattern_t(name, pattern, followMode, std::vector<uint8_t>(), module, symbol)
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
	prologue(prologue),
	module(module)
{
	Patterns::patterns().emplace_back(this);
}

bool Pattern_t::find()
{
	lm_module_t& targetModule = module ? *module : g_modSteamClient;
	matchAddress = LM_ADDRESS_BAD;
	lm_address_t cachedMatch = LM_ADDRESS_BAD;
	if (const auto trusted = catalogAddress(*this, true, &cachedMatch))
	{
		address = *trusted;
		matchAddress = cachedMatch;
	}
	else
	{
		const auto resolved = MemHlp::searchSignatureDetailed
		(
			name.c_str(), pattern.c_str(), targetModule, followMode,
			prologue.empty() ? nullptr : prologue.data(), prologue.size()
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
			lm_segment_t targetSegment {};
			const bool inside = address >= targetModule.base
			                 && address < targetModule.base + targetModule.size;
			const bool executable = LM_FindSegment(address, &targetSegment)
			                     && (targetSegment.prot & LM_PROT_XR) == LM_PROT_XR;
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
			lm_segment_t segment {};
			const bool executable = LM_FindSegment(address, &segment)
			                     && (segment.prot & LM_PROT_XR) == LM_PROT_XR;
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

// Re-derive the volatile dispatch-tree root of every IClient*::RunIPCFrame in
// one pass over the steamclient module's executable segments, then point each
// pattern's seed at the nearest live root.  Pure decision logic lives in
// feats/ipcframe.hpp (host-unit-tested); this only walks live memory.
static void autoResolveIpcFrameRoots()
{
	const std::array<Pattern_t*, 6> targets =
	{
		&Patterns::IClientApps::RunIPCFrame,
		&Patterns::IClientAppManager::RunIPCFrame,
		&Patterns::IClientRemoteStorage::RunIPCFrame,
		&Patterns::IClientUGC::RunIPCFrame,
		&Patterns::IClientUserStats::RunIPCFrame,
		&Patterns::IClientUser::RunIPCFrame,
	};
	std::array<bool, targets.size()> catalogResolved {};
	for (size_t i = 0; i < targets.size(); ++i)
		catalogResolved[i] = catalogAddress(*targets[i], false).has_value();

	struct Ctx
	{
		std::vector<IpcFrame::Cand> cands;
	} ctx;

	// Collect candidates from executable segments belonging to steamclient.
	// IpcFrame::scan bounds every read by the segment size, so this never
	// touches a guard page (same safety contract as MemHlp::patternScan).
	const auto enumSegments = [](lm_segment_t* seg, lm_void_t* arg) -> lm_bool_t
	{
		auto* c = reinterpret_cast<Ctx*>(arg);
		if ((seg->prot & LM_PROT_XR) != LM_PROT_XR)
			return LM_TRUE;
		// Restrict to the steamclient module's address range.
		if (seg->base + seg->size <= g_modSteamClient.base)
			return LM_TRUE;
		if (seg->base >= g_modSteamClient.base + g_modSteamClient.size)
			return LM_TRUE;

		const auto local = IpcFrame::scan(reinterpret_cast<const uint8_t*>(seg->base), seg->size);
		for (const auto& cand : local)
			c->cands.push_back({
				static_cast<size_t>(seg->base) + cand.offset,
				cand.root,
				cand.available,
			});
		return LM_TRUE;
	};
	if (!IpcFrame::scanWhenCatalogIncomplete(catalogResolved, [&]
	{
		LM_EnumSegments(enumSegments, &ctx);
	}))
	{
		return;
	}

	if (ctx.cands.empty())
	{
		g_pLog->debug("IpcFrame: no RunIPCFrame candidates found; keeping embedded roots\n");
		return;
	}

	for (size_t i = 0; i < targets.size(); ++i)
	{
		Pattern_t* p = targets[i];
		// A validated exact-SHA catalog is authoritative for this locator.  Do
		// not mutate or scan the embedded fallback unless the catalog entry is
		// absent or fails executable-range validation.
		if (catalogResolved[i])
			continue;
		const uint32_t seed = IpcFrame::parseTrailingRoot(p->pattern);
		size_t idx = IpcFrame::resolveConfident(ctx.cands, seed, IpcFrame::kMaxRootDrift);

		// The 2026-08-05 RemoteStorage tree kept its internal comparisons but
		// drifted every message id (median/root moved ~9, siblings ~3-8) and
		// selected a numerically distant median/root. Match three independent
		// pivots chosen mid-way between the 2026-07-21 and 2026-08-05 values so a
		// single bounded literal set still resolves both builds; never widen the
		// global numeric band and risk assigning an unrelated interface.
		if (idx == SIZE_MAX && p == &Patterns::IClientRemoteStorage::RunIPCFrame)
		{
			static constexpr uint32_t fingerprint[] =
			{
				0x5DB47296, 0x7F3F564A, 0x84692E73,
			};
			size_t matches = 0;
			for (size_t i = 0; i < ctx.cands.size(); ++i)
			{
				const auto& cand = ctx.cands[i];
				if (IpcFrame::matchesCmpFingerprint(
					reinterpret_cast<const uint8_t*>(cand.offset), cand.available,
					fingerprint, std::size(fingerprint), 4))
				{
					idx = i;
					++matches;
				}
			}
			if (matches != 1)
				idx = SIZE_MAX;
		}
		if (idx == SIZE_MAX)
		{
			// No confident match: drift too large, or an ambiguous neighbour.
			// Keep the embedded root; find() then either still matches it or
			// fails loudly as a required pattern (no silent mis-hook).
			continue;
		}

		const uint32_t found = ctx.cands[idx].root;
		if (found != seed)
		{
			g_pLog->warn
			(
				"IpcFrame: %s root drifted 0x%08X -> 0x%08X; auto-resolved\n",
				p->name.c_str(), seed, found
			);
			IpcFrame::setTrailingRoot(p->pattern, found);
		}
	}
}


bool Patterns::init()
{
	bool found = true;

	// Establish immutable compiled policy before reading any remote-derived
	// metadata.  A catalog whose required flags or symbols disagree with this
	// registry is rejected in full.
	CUser::MarkLicenseAsChanged.optional = true;
	CUser::ProcessPendingLicenseUpdates.optional = true;
	CUser::NotifyLicensesUpdated.optional = true;
	CUser::Offset_CompatManager.optional = true;
	CAppInfoCache::GetOrAddAppData.optional = true;
	CAppInfoCache::ThreadedReadFromDisk.optional = true;
	CAppInfoCache::SkipFlagReference.optional = true;
	CAppInfoCache::ShaReference.optional = true;
	CDepotDownloadMgr::ProcessDepotManifest.optional = true;
	CDepotDownloadMgr::PrepareDepotDownload.optional = true;
	CDepotDownloadMgr::BuildDepotDependency.optional = true;
	CDepotDownloadMgr::EvaluateConfigChanges.optional = true;
	CDepotDownloadMgr::OnChunkUnpackedStack.optional = true;
	CDepotDownloadMgr::OnChunkUnpackedReg.optional = true;
	ParentalSignatureCheck.optional = true;
	ParentalSettingsReceived.optional = true;
	SteamUI::AppControllerRunFrame.optional = true;
	SteamUI::GetAppByID.optional = true;
	SteamUI::MarkAppChange.optional = true;
	SteamUI::BuildCompleteAppOverviewChange.optional = true;
	SteamUI::OwnershipFlagsReference.optional = true;

	loadActiveCatalogs();

	// Self-heal the IClient*::RunIPCFrame signatures before scanning.  Their
	// only volatile byte is the dispatch-tree root message id, which drifts
	// when Steam adds/removes interface methods (the 2026-06-23 update broke
	// all four this way).  Re-derive each root structurally so a constant-only
	// drift no longer needs a code change; on any failure the embedded root is
	// kept verbatim (no regression).
	autoResolveIpcFrameRoots();
	for(auto& pattern : patterns())
	{
		if (!pattern->find())
		{
			if (pattern->optional)
			{
				// Optional patterns degrade to a safe no-op in their
				// dependent feature; don't fail the whole load.
				g_pLog->warn
				(
					"Optional pattern '%s' not found; dependent feature disabled\n",
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
	Pattern_t FamilyGroupRunningApp
	{
		"FamilyGroupRunningApp",
		"E8 ? ? ? ? 83 C4 10 83 EC 08 C7 46 ? 01 00 00 00 C6 46 ? 01 56 57 E8 ? ? ? ? 83 C4 1C B8 01 00 00 00 5B 5E 5F 5D C3 ? ? ? ? ? ? ? 83 EC 04",
		SigFollowMode::Relative
	};
	Pattern_t StopPlayingBorrowedApp
	{
		"StopPlayingBorrowedApp",
		"8B 40 ? 83 EC 0C 89 F3 8B 95",
		SigFollowMode::PrologueUpwards,
		std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
	};
	Pattern_t ParentalSignatureCheck
	{
		"ParentalSignatureCheck",
		"84 C0 75 27 8B 85 ? ? ? ? 8D 9D ? ? ? ? 83 EC 04 FF B0 82 01 00 00 8D 86 ? ? ? ? 50 53 E8 ? ? ? ?",
		SigFollowMode::None
	};
	Pattern_t ParentalSettingsReceived
	{
		"ParentalSettingsReceived",
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 00 02 00 00 8B 45 08 8B 55 1C 8B 7D 0C",
		SigFollowMode::None
	};

	Pattern_t TraceIPC
	{
		"TraceIPC",
		"E8 ? ? ? ? 83 C4 10 85 FF 74 ? 8B 07 83 EC 04 FF B5 ? ? ? ? FF B5 ? ? ? ? 57 FF 10 83 C4 10 8D 45 ? 83 EC 04 89 F3 6A 04 50 FF 75",
		SigFollowMode::Relative
	};

	namespace SteamUI
	{
		// Linux i386 steamui.so build
		// 38adc592f0ab97349639b297203739db70b6547f (SHA-256
		// 833914b45fcb407e50631d1f62558a28c2f74dc7431b0f0aebd6d25e45b4cd72).
		// Each complete signature below has exactly one .text match.  They are
		// optional as a group: live visual removal must fail closed without
		// affecting package refresh when Steam moves any UI implementation.
		Pattern_t AppControllerRunFrame
		{
			"CSteamUIAppController::RunFrame",
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 7C E8 ? ? ? ? 8B 84 24 90 00 00 00 D9 5C 24 0C D9 44 24 0C",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::AppControllerRunFrame"
		};
		// Direct cdecl entry: (controller, appid, create).  The opening PIC
		// thunk is part of the entry and must be repaired in a trampoline only
		// if this locator is detoured in the future; LibraryRemoval calls it.
		Pattern_t GetAppByID
		{
			"CSteamUIAppController::GetAppByID",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 5D 10 8B 75 08 89 45 C8 8B 80 E0 09 00 00",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::GetAppByID"
		};
		// Direct cdecl entry: (source, appid, change_flags).  Hooking this
		// captures the otherwise private source receiver used by RunFrame.
		Pattern_t MarkAppChange
		{
			"CUpdateManager::MarkAppChange",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 3C 8B 75 0C 8B 5D 10 89 45 D4 8B 80 E0 09 00 00",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::MarkAppChange"
		};
		// Direct cdecl entry: (controller, change, optional_callback).  The
		// detour only publishes an atomic reassert request after the original.
		Pattern_t BuildCompleteAppOverviewChange
		{
			"BuildCompleteAppOverviewChange",
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC 8C 00 00 00 8B 7D 08 89 5D A4 E8 ? ? ? ? 84 C0",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::BuildCompleteAppOverviewChange"
		};
		// FillInAppOverview reads CSteamApp::OwnershipFlags through `mov
		// eax,[ecx+disp8]` immediately before serializing eAppOwnershipFlags.
		// Keep the instruction address so LibraryRemoval derives and validates
		// the Linux field offset instead of importing the unrelated Windows
		// layout constant.
		//
		// FillInAppOverview emits a run of fields that all share this shape
		// (type check -> `mov eax,[app+disp8]` -> push value, push descriptor,
		// push sink, call emitter), so the field displacement is what tells them
		// apart and MUST stay pinned: ownership is 0x18 on both the 2026-08-03
		// and the 2026-08-16 client, and the block right after it reads 0x50
		// through a different emitter.  What actually drifted is only the
		// register Steam parked the app pointer in (ecx -> edx), so the modrm
		// byte is the wildcard.
		//
		// Do NOT wildcard the displacement instead.  That also yields exactly
		// one match on the newer client, but it is the neighbouring field, and
		// LibraryRemoval would then flip hidden-ownership bits inside it.
		Pattern_t OwnershipFlagsReference
		{
			"CSteamApp::OwnershipFlagsReference",
			"8B ? 18 8B 9D ? ? ? ? 83 EC 04 50 8D 83 ? ? ? ? 50 FF B5 ? ? ? ? E8 ? ? ? ?",
			SigFollowMode::None,
			&g_modSteamUI,
			"Patterns::SteamUI::OwnershipFlagsReference"
		};
	}

	namespace CAPIJob
	{
		Pattern_t GetPlayerStats
		{
			"CAPIJob::GetPlayerStats",
			"E8 ? ? ? ? 83 C4 10 89 C5 E9 ? ? ? ? ? ? 80 BE ? ? ? ? 00",
			SigFollowMode::Relative
		};
	}

	namespace CProtoBufMsgBase
	{
		Pattern_t InitFromPacket
		{
			"CProtoBufMsgBase::InitFromPacket",
			"E8 ? ? ? ? 58 8B 45 ? 8B 8D",
			SigFollowMode::Relative
		};
		Pattern_t Send
		{
			"CProtoBufMsgBase::Send",
			"E8 ? ? ? ? 59 5A 50 56 E8 ? ? ? ? 83 C4 0C",
			SigFollowMode::Relative
		};
	};

	namespace CSteamEngine
	{
		Pattern_t Init
		{
			"CSteamEngine::Init",
			"E8 ? ? ? ? 83 C4 10 8D 83 ? ? ? ? 83 EC 0C 89 AB",
			SigFollowMode::Relative
		};
		Pattern_t SetAppIdForCurrentPipe
		{
			"CSteamEngine::SetAppIdForCurrentPipe",
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? 8B 85 ? ? ? ? 83 EC 08 FF B5",
			SigFollowMode::Relative
		};
		Pattern_t Offset_User
		{
			"CSteamEngine::m_pUser",
			"8B 80 ? ? ? ? FF 75 ? 8D 34",
			SigFollowMode::None,
			nullptr,
			"Patterns::CSteamEngine::Offset_User"
		};
	}

	namespace CSteamMatchmakingServers
	{
		Pattern_t GetServerDetails
		{
			"CSteamMatchmakingServers::GetServerDetails",
			"89 45 ? 83 C4 10 83 EC 0C 89 F3",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t RequestInternetServerList
		{
			"CSteamMatchmakingServers::RequestInternetServerList",
			"C7 04 24 ? ? 00 00 E8 ? ? ? ? 5A 89 45 ? 59 FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? 6A 01",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace CUser
	{
		// CCompatManager is an embedded member of the local CUser. Locate the
		// constructor call and retain the LEA so CSteamEngine can decode its
		// displacement instead of pinning a build-specific class offset.
		Pattern_t Offset_CompatManager
		{
			"CUser::m_CompatManager",
			"8D 9E ? ? ? ? 89 9D ? ? ? ? 53 89 FB E8 ? ? ? ? 58 5A "
			"C7 86 ? ? ? ? FF FF FF FF",
			SigFollowMode::None,
			nullptr,
			"Patterns::CUser::Offset_CompatManager"
		};
		Pattern_t CheckAppOwnership
		{
			"CUser::CheckAppOwnership",
			"E8 ? ? ? ? 88 45 ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 45 ? 80 7D ? 00",
			SigFollowMode::Relative
		};
		Pattern_t GetSubscribedApps
		{
			"CUser::GetSubscribedApps",
			"E8 ? ? ? ? 89 C6 83 C4 10 85 C0 0F 84 ? ? ? ? 8B 9D ? ? ? ? 39 D8",
			SigFollowMode::Relative
		};
		Pattern_t PostCallback
		{
			"CSteamEngine::PostCallback",
			"E8 ? ? ? ? 8D 86 ? ? ? ? 83 C4 18 68 F6 01 00 00",
			SigFollowMode::Relative,
			nullptr,
			"Patterns::CUser::PostCallback"
		};
		Pattern_t PostCallbackToAppId
		{
			"CUser::PostCallbackToAppId",
			"84 C0 0F 45 F8 89 F8",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xE8, 0x53, 0x56, 0x57, 0x55 }
		};
		Pattern_t UpdateAppOwnershipTicket
		{
			"IClientUser::UpdateAppOwnershipTicket",
			"E8 ? ? ? ? E9 ? ? ? ? ? ? ? ? ? ? 8D 45 ? 89 45 ? EB",
			SigFollowMode::Relative,
			nullptr,
			"Patterns::CUser::UpdateAppOwnershipTicket"
		};
		// Current Linux i386 entry at RVA 0x0186BDC0.  Callers at
		// 0x0187309x/0x018731ax push this/package-id/bool, and the body hashes
		// the package id into the changed-license table.  This is a direct
		// cdecl entry with a PIC thunk call; the full signature has one match in
		// the inspected steamclient.so.
		Pattern_t MarkLicenseAsChanged
		{
			"CUser::MarkLicenseAsChanged",
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 74 24 40 8B 44 24 48 8B BE ? ? 00 00 88 44 24 1C 8D 86 ? ? 00 00 89 44 24 0C 85 FF 0F 84 ? ? ? ?",
			SigFollowMode::None
		};
		// Current Linux i386 entry at RVA 0x0186C800.  Its cdecl receiver is
		// read from [esp+0x40] after the saved-register prologue; it walks the
		// pending-license vector, validates CAppData, and drives the downstream
		// callback path containing callback 0xF90BE.  Caller 0x0187A67B passes
		// the same local-user receiver.  The full direct-entry signature has one
		// module match.
		Pattern_t ProcessPendingLicenseUpdates
		{
			"CUser::ProcessPendingLicenseUpdates",
			// The trailing `lea eax,[ebx+disp32]` reaches a data symbol through
			// the PIC base, so its displacement tracks the GOT layout and moves
			// on an unrelated client change (0x3B314 -> 0x3B714 between the
			// 2026-08-03 and 2026-08-16 builds).  Nothing here reads it -- the
			// hook only calls the resolved entry -- so it is location-only and
			// stays wildcarded.  The member offsets loaded above (0x1C7C count,
			// 0x1C70 array base) ARE consumed by the function and remain pinned
			// as identity; the whole relaxed form still has one module match.
			"55 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 83 EC 2C 8B 44 24 40 8B 88 7C 1C 00 00 85 C9 0F 8E ? ? ? ? 05 70 1C 00 00 89 44 24 18 8D 83 ? ? ? ? 8B 30",
			SigFollowMode::None
		};
		// CUser::<broadcast LicensesUpdated_t>(CUser* this)
		//
		// The license-update notifier: rebuilds the LicensesUpdated_t
		// callback (callback id 0x7d) from the CUser's own license vector
		// and posts it to every subscriber via the PostCallback dispatch.
		// Used as the post-injection reconcile: after we append our
		// AdditionalApps into package 0's AppIdVec, invoking this on the
		// local CUser forces Steam's ownership/library layer to re-read
		// licenses (and therefore package 0, now containing our appids),
		// which breaks the cold-cache PICS product-info request loop.
		//
		// Positively identified via the RTTI string "17LicensesUpdated_t"
		// referenced just before it posts callback 0x7d.  Single stack arg
		// (`this`, read from [ebp+0x8]); standard cdecl, safe to call by
		// resolved pointer with g_pLocalUser as `this`.
		//
		// Direct prologue match (push ebp / mov ebp,esp / push edi,esi,ebx
		// / get_pc_thunk + add ebx / sub esp,0x1bc / mov edi,[ebp+0x8] /
		// mov edi,[eax+<off>] / mov [ebp-0x1ac],ebx / test edi,edi).  The
		// get_pc_thunk call rel, the PIC add immediate, the frame size, and
		// the [ebp-0x1ac] spill offset are masked so local-frame reshuffles
		// across builds stay compatible.  The member offset (`8B B8 ?? ?? 00
		// 00` = mov edi,[eax+0x1bXX]) is ALSO masked: it is a CUser layout
		// field whose value drifted 0x1b18 -> 0x1b14 on the 2026-06-23 client
		// (a 4-byte shift; RequiresLegacyCDKey moved the same way).  SLSsteam
		// only needs to LOCATE this function (it invokes it by pointer with
		// g_pLocalUser as `this`); the offset value itself is internal to the
		// function, so wildcarding it makes the signature self-heal across that
		// class of layout drift while staying unique.  Verified: exactly 1
		// match in both the pre- and post-2026-06-23 steamclient.so.
		Pattern_t NotifyLicensesUpdated
		{
			"CUser::NotifyLicensesUpdated",
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC ? ? ? ? 8B 45 08 8B B8 ? ? 00 00 89 9D ? ? FF FF 85 FF",
			SigFollowMode::None
		};
	}

	namespace CAppInfoCache
	{
		// Current Linux i386 cdecl entry at RVA 0x00FCA900.  The current
		// module's GetOrAddAppData source assertion xref is inside this body;
		// [ebp+8]/[ebp+0xc]/[ebp+0x10] are cache/appid/create and the returned
		// node is consumed as CAppData by ProcessPendingLicenseUpdates.  The
		// direct PIC-thunk prologue is the trampoline entry; one full match.
		// The separate 0x00FD4070 regparm helper is a lookup path, not this
		// cdecl GetOrAddAppData target.
		Pattern_t GetOrAddAppData
		{
			"CAppInfoCache::GetOrAddAppData",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 2C 8B 75 0C 8B 7D 10 89 45 D0 8B 80 BC 08 00 00 8B 00 85 C0 0F 85 ? ? ? ?",
			SigFollowMode::None
		};
		// CAppInfoCache::ThreadedReadFromDisk is Steam's own background cache
		// loader.  The current Linux i386 entry is RVA 0x00FC89F0 and takes the
		// CAppInfoCache pointer as its sole cdecl argument.  The 0x110c-byte
		// parser frame plus the saved cache argument makes this prologue unique
		// in the current module (one full match); signed catalogs can replace the
		// optional locator independently when a client update changes the frame.
		Pattern_t ThreadedReadFromDisk
		{
			"CAppInfoCache::ThreadedReadFromDisk",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 0C 11 00 00 8B 45 08 89 85 0C EF FF FF",
			SigFollowMode::None
		};
		// Instruction site at RVA 0x0186C929: cmp byte ptr [esi+disp8],0
		// tests the same CAppData returned by GetOrAddAppData.  None is
		// intentional because AppDataLayout decodes this instruction in place;
		// the full reference signature has one module match.
		Pattern_t SkipFlagReference
		{
			"CAppInfoCache::SkipFlagReference",
			"80 7E 10 00 0F 44 C8 88 4C 24 1E 8B 44 24 10 83 C7 01 3B 7D 44 8B 30 7C 9E",
			SigFollowMode::None
		};
		// Instruction site at RVA 0x0186C914: lea eax,[eax+disp8], using the
		// GetOrAddAppData return while the preceding mov esi,eax preserves a copy
		// for the later skip test.  A 20-byte SHA comparison follows.  None keeps
		// the instruction address for runtime layout derivation; one full match.
		Pattern_t ShaReference
		{
			"CAppInfoCache::ShaReference",
			"8D 40 1C 50 E8 ? ? ? ? 83 C4 10 85 C0 75 10 0F B6 4C 24 1E 80 7E 10 00 0F 44 C8 88 4C 24 1E",
			SigFollowMode::None
		};
	}

	namespace IClientAppManager
	{
		Pattern_t RunIPCFrame
		{
			"IClientAppManager::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D B7 85 0A 7A",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t BCanRemotePlayTogether
		{
			"IClientAppManager::BCanRemotePlayTogether",
			"58 5A FF 74 24 ? 56 E8 ? ? ? ? 83 C4 10 85 C0 74",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0xe8, 0x53, 0x56, 0x57 }
		};
	}

	namespace IClientApps
	{
		Pattern_t RunIPCFrame
		{
			"IClientApps::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 37 9C 88 A6",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientRemoteStorage
	{
		Pattern_t RunIPCFrame
		{
			"IClientRemoteStorage::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 6C E8 2F 87",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUser
	{
		Pattern_t RunIPCFrame
		{
			"IClientUser::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 10 A3 86 73",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};

		Pattern_t BLoggedOn
		{
			"IClientUser::BLoggedOn",
			"E9 ? ? ? ? ? ? ? ? ? ? 5B 5E 5F FF E0",
			SigFollowMode::Relative
		};
		Pattern_t BUpdateAppOwnershipTicket
		{
			"IClientUser::BUpdateAppOwnershipTicket",
			"83 EC 0C 89 F3 8B 7D ? FF 30 E8 ? ? ? ? 83 C4 10 83 FF 01 77 ? 84 C0 75 ? 80 7D ? 00 74 ? 80 7D ? 00 0F 84 ? ? ? ? 8D 65",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t GetAppOwnershipTicketExtendedData
		{
			"IClientUser::GetAppOwnershipTicketExtendedData",
			"83 EC 24 FF 74 24 ? 8B 44 24",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
		};
		Pattern_t GetSteamId
		{
			"IClientUser::GetSteamID",
			"E8 ? ? ? ? 89 D8 83 C4 0C 83 C4 08 5B C2 04 00 ? 83 EC 08 50 53 FF D2 89 D8 83 C4 0C 83 C4 08 5B C2 04 00",
			SigFollowMode::Relative,
			nullptr,
			"Patterns::IClientUser::GetSteamId"
		};
		Pattern_t IsUserSubscribedAppInTicket
		{
			"IClientUser::IsUserSubscribedAppInTicket",
			"E8 ? ? ? ? 89 C3 83 C4 20 8B ? ? ? ? ? 8B",
			SigFollowMode::Relative
		};
		// The tail thunk adjusts the interface `this` to the implementation
		// pointer with `sub eax,<off>` (`2D ?? ?? 00 00`).  That offset is a
		// layout constant that drifted 0x18d8 -> 0x18d4 on the 2026-06-23
		// client; it is only part of the LOCATION signature (SLSsteam hooks the
		// function, it does not use the offset value), so it is masked here to
		// self-heal across that drift.  The long surrounding tail keeps the
		// match unique — verified exactly 1 hit in both the pre- and
		// post-2026-06-23 steamclient.so.
		Pattern_t RequiresLegacyCDKey
		{
			"IClientUser::RequiresLegacyCDKey",
			"75 ? 83 C4 1C 31 C0 5B 5E 5F 5D C3 ? ? ? ? ? 8B 44 24 ? 83 C4 1C 89 F9 89 F2 5B 5E 5F 5D 2D ? ? 00 00",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x53, 0x56, 0x57, 0x55 }
		};
	}

	namespace IClientUGC
	{
		Pattern_t RunIPCFrame
		{
			"IClientUGC::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 62 0C D2 71",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace IClientUserStats
	{
		Pattern_t RunIPCFrame
		{
			"IClientUserStats::RunIPCFrame",
			"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 8F 65 6D 87",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
	}

	namespace CPackageInfoCache
	{
		Pattern_t LoadPackage
		{
			"CPackageInfoCache::LoadPackage",
			"E8 ? ? ? ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 95 ? ? FF FF 8B 7A 18 83 FF FF",
			SigFollowMode::Relative
		};
	}

	namespace CUtlMemory
	{
		Pattern_t Grow
		{
			"CUtlMemory::Grow",
			"E8 ? ? ? ? 8B 85 ? ? FF FF 83 C4 10 8B 40 44 89 85 ? ? FF FF 83 C0 01 E9",
			SigFollowMode::Relative
		};
	}

	namespace CDepotDownloadMgr
	{
		// Two cooperating hook points in CDepotDownloadMgr, both with the
		// same 7-dword signature (context, ., appId, depotId, uint64 gid, .)
		// and both self-contained PIC functions.
		//
		// (1) ProcessDepotManifest (the manifest-acquisition LEAF, 5 callers):
		//     builds "<root>/depotcache/<depot>_<gid>.manifest" from its gid
		//     arg, checks it on disk, and only calls BYldRequestDepotManifest
		//     when missing.  Redirecting the gid here makes the on-disk check
		//     find the locally-staged (zip) manifest and SKIP the request-code
		//     fetch — this is what lets a providers-down install proceed past
		//     "No internet connection".  PIC get_pc_thunk is the FIRST insn, so
		//     fixPICThunkCall must repair the relocated thunk in the tramp.
		//
		// (2) PrepareDepotDownload (one of the 5 callers, a LATER pipeline
		//     stage): after calling the leaf it looks the depot up in the
		//     per-download table BY the gid it was called with and derefs the
		//     per-manifest state pointer.  If the leaf was redirected to the
		//     zip gid but this frame still looks up the public gid -> miss ->
		//     NULL deref -> SIGSEGV at Reconfiguring (core-dump confirmed).
		//     Redirecting the
		//     gid here too keeps the leaf call and the table lookup consistent.
		//     PIC get_pc_thunk is at +5 (after the 5-byte prologue we relocate)
		//     so fixPICThunkCall is a harmless no-op for this hook point.
		//
		// Both verified: 1 match each in .text.
		Pattern_t ProcessDepotManifest
		{
			"CDepotDownloadMgr::ProcessDepotManifest",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 55 1C 89 45 C0 8B 45 18 89 55 CC 89 45 C8",
			SigFollowMode::None
		};

		Pattern_t PrepareDepotDownload
		{
			"CDepotDownloadMgr::PrepareDepotDownload",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 60 8B 7D 08 8B 45 18 8B 55 1C FF 75 20 89 45 98 52 50 FF 75 14 89 55 9C FF 75 10 FF 75 0C 57 E8 ? ? ? ? 8B 47 4C 83 C4 20 83 F8 FF",
			SigFollowMode::None
		};

		// (3) BuildDepotDependency (the install-plan CONSUMER, located via a
		//     runtime stack trace).  The
		//     per-app planner: receives an already-built CUtlVector<DepotEntry>
		//     (arg2 = [ebp+0x10]; count @ +0xc, element base @ +0, stride 0x20)
		//     and, per entry, copies ManifestGid (+0x8) into the context's
		//     planned-gid vector (ctx+0x664) AND drives ProcessDepotManifest /
		//     the shared-depot handler.  Patching depots[i].ManifestGid here
		//     (before the original runs) is the LumaCore manifest-override
		//     point: it mutates the SOURCE the commit reads from, not the
		//     by-value gid the acquisition leaf gets (which the commit ignores).
		//     PIC get_pc_thunk is the FIRST insn (like ProcessDepotManifest) ->
		//     fixPICThunkCall repairs the relocated thunk in the tramp.
		//     Verified: 1 match in .text (entry VA 0x11413e0 on build
		//     cfe99f0cc8fee644e2a8e3d1a0794e49).
		Pattern_t BuildDepotDependency
		{
			"CDepotDownloadMgr::BuildDepotDependency",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 8C 04 00 00 8B 55 10 8B 7D 0C 89 85 A0 FB FF FF 8B 45 08",
			SigFollowMode::None
		};

		// Structured chunk-completion callbacks. Steam reaches these before it
		// formats the content_log line, with the unpack result still available as
		// the final argument. There are two compiler-generated ABI variants for
		// the same body: ordinary cdecl and regparm(3). Both are optional and
		// independently hooked; a future signature drift disables quarantine
		// detection rather than affecting the download pipeline.
		Pattern_t OnChunkUnpackedStack
		{
			"CDepotDownloadMgr::OnChunkUnpacked[cdecl]",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 7C 04 00 00 8B 45 0C 8B 7D 08 89 85 90 FB FF FF 8B 45 10 89 85 8C FB FF FF",
			SigFollowMode::None,
			nullptr,
			"Patterns::CDepotDownloadMgr::OnChunkUnpackedStack"
		};

		Pattern_t OnChunkUnpackedReg
		{
			"CDepotDownloadMgr::OnChunkUnpacked[regparm3]",
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 89 C6 53 81 EC 7C 04 00 00 8B 45 0C 89 95 90 FB FF FF 89 8D 8C FB FF FF 89 85 94 FB FF FF",
			SigFollowMode::None,
			nullptr,
			"Patterns::CDepotDownloadMgr::OnChunkUnpackedReg"
		};

		// (4) EvaluateConfigChanges (the post-commit reconcile, located via
		//     static RE).  Emits the
		//     content_log "AppID %u ...config changed : added/removed/updated
		//     depots %s" lines and decides "Update Required".  It diffs the
		//     app's installed depot vector (ptr @ ctx+0x78, count @ ctx+0x84,
		//     stride 0x20, ManifestGid @ +0x8) against an appinfo-derived
		//     target list, flagging a depot "updated" when the two gids differ
		//     (the movq/pxor compare at VA 0xfe4598).  THIS is what perpetually
		//     re-flags a downgraded (pinned) install whose appinfo still
		//     carries the public gid -> the loop.  Hook target for the (B) fix.
		//     Calling convention: a global anchor/manager pointer comes in EAX
		//     (used as both the manager object and the PIC string anchor); the
		//     three args are on the stack (ctx @ ebp+0x8, appId @ ctx+0x8).
		//     The prologue's first insn is `push ebp` (NOT a get_pc_thunk), so
		//     no fixPICThunkCall is needed; a regparm(1) detour preserves EAX.
		//     Verified: 1 match in .text (entry VA 0xfe425a on build
		//     cfe99f0cc8fee644e2a8e3d1a0794e49).
		Pattern_t EvaluateConfigChanges
		{
			"CDepotDownloadMgr::EvaluateConfigChanges",
			"55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF 8B 45 10 89 85 40 FF FF FF 8B 45 08 8B 40 04",
			SigFollowMode::None
		};
	}

	namespace IClientFriends
	{
		Pattern_t GetFriendGamePlayed
		{
			"IClientFriends::GetFriendGamePlayed",
			"C7 45 ? A2 F1 5B 33 6A 04 50 57 E8 ? ? ? ?",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xE5, 0x89, 0x55 }
		};
	}

	namespace IClientUtils
	{
		Pattern_t RunIPCFrame
		{
			"IClientUtils::RunIPCFrame",
			"83 EC 08 89 F3 50 57 E8 ? ? ? ? 58 FF B5 ? ? ? ? E8 ? ? ? ? 58 8D 45",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x56, 0x57, 0xe5, 0x89, 0x55 }
		};
		Pattern_t Offset_GetPipeIndex
		{
			"IClientUtils::m_PipeIndex",
			"8B 91 ? ? ? ? 83 F8 FF 74 ? 8B 89 ? ? ? ? EB ? ? ? ? 8B 00 83 F8 FF 74 ? 8D 04 ? 8D 04 ? 3B 50",
			SigFollowMode::None,
			nullptr,
			"Patterns::IClientUtils::Offset_GetPipeIndex"
		};
	}

	namespace ISteamMatchmakingPingResponse
	{
		Pattern_t ServerResponded
		{
			"ISteamMatchmakingPingResponse::ServerResponded",
			"8B 85 ? ? ? ? 8B 40 ? 85 C0 0F 84 ? ? ? ? 39 46",
			SigFollowMode::PrologueUpwards,
			std::vector<uint8_t> { 0x57, 0xe5, 0x89, 0x55 },
			&g_modSteamUI
		};
	}

	namespace CWebSocketConnection
	{
		Pattern_t BBuildAndAsyncSendFrame
		{
			"CWebSocketConnection::BBuildAndAsyncSendFrame",
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 81 EC AC 00 00 00 "
			"8B 45 10 8B 55 08 89 85 ? ? FF FF 89 95 ? ? FF FF "
			"65 A1 14 00 00 00",
			SigFollowMode::None
		};
	}

	namespace CRemoteClientManager
	{
		Pattern_t RecvPkt
		{
			"CRemoteClientManager::RecvPkt",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 1C "
			// [esi+0x8XX]: PIC-relative global slot whose displacement
			// drifts between Steam client builds (0x8B0 -> 0x8B4 on
			// 1781041600).  Wildcard the displacement byte so the match
			// survives that shift; the rest of the body keeps it unique.
			"8B 86 ? 08 00 00 8B 00 85 C0 0F 85 ? ? ? ? "
			"C7 45 E4 00 00 00 00 83 EC 08 89 F3 6A 01 FF 75 0C "
			"E8 ? ? ? ? 89 C7 83 C4 10 85 C0 0F 84 ? ? ? ? 83 EC 0C 50",
			SigFollowMode::None
		};
	}

	namespace CJobMgr
	{
		Pattern_t BRouteMsgToJob
		{
			"CJobMgr::BRouteMsgToJob",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 7C "
			"8B 45 08 8B 4D 14 89 45 90 8B 45 0C 89 4D A0 89 45 88 "
			"8B 45 10 89 45 A4 65 8B 0D 14 00 00",
			SigFollowMode::None
		};
	}

	namespace CDepotDownloadMgr
	{
		Pattern_t BYldRequestDepotManifest
		{
			"CDepotDownloadMgr::BYldRequestDepotManifest",
			"55 B9 FD FF FF FF 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 53 83 EC 7C 8B 45 14 8B 55 18",
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
