// SPDX-License-Identifier: AGPL-3.0-only
//
// Standalone tests for strict parsing of the deferred Proton mapping file.

#include "../src/feats/pending_proton.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string_view>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

static bool has(const std::set<uint32_t>& ids, uint32_t id)
{
	return ids.find(id) != ids.end();
}

int main()
{
	CHECK(AppInfoProvision::requiresProtonMapping(
	          /*keptDepots=*/1, {}, "windows"),
	      "windows-only common metadata covers depots without an oslist");
	CHECK(!AppInfoProvision::requiresProtonMapping(
	          /*keptDepots=*/1, {}, "windows,linux"),
	      "common metadata with native Linux support does not force Proton");
	CHECK(!AppInfoProvision::requiresProtonMapping(
	          /*keptDepots=*/1, {}, ""),
	      "unknown platform metadata does not alter synthetic app behavior");
	CHECK(!AppInfoProvision::requiresProtonMapping(
	          /*keptDepots=*/0, {}, "windows"),
	      "an app without usable content is not made installable by Proton alone");
	CHECK(!AppInfoProvision::requiresProtonMapping(
	          /*keptDepots=*/1, {"linux"}, "windows"),
	      "depot-specific Linux metadata takes precedence over common metadata");
	std::ifstream provisionSource("src/feats/appinfo_provision.cpp");
	const std::string provisionText(
		(std::istreambuf_iterator<char>(provisionSource)),
		std::istreambuf_iterator<char>());
	CHECK(provisionText.find("requiresProtonMapping(") != std::string::npos &&
	          provisionText.find("keptContent") != std::string::npos,
	      "depot pruning uses the platform fallback only for retained content");
	CHECK(provisionText.find("cachedWireRequiresProton(wire)") !=
	          std::string::npos,
	      "startup migrates validated caches created before the platform fix");

	{
		const auto parsed = AppInfoProvision::parsePendingProtonText(
		    "  123\n4294967295\n123\t\n");
		CHECK(parsed.status == AppInfoProvision::PendingProtonParseStatus::Valid,
		      "valid mapping file accepts whitespace and duplicate ids");
		CHECK(parsed.ids.size() == 2 && has(parsed.ids, 123) &&
		          has(parsed.ids, 4294967295u),
		      "valid mapping file keeps the complete uint32 id set");
	}

	{
		const auto parsed = AppInfoProvision::parsePendingProtonText(
		    "123\nnot-an-appid\n456\n");
		CHECK(parsed.status == AppInfoProvision::PendingProtonParseStatus::Invalid,
		      "malformed token invalidates the whole mapping file");
	}

	{
		const auto parsed = AppInfoProvision::parsePendingProtonText("0\n");
		CHECK(parsed.status == AppInfoProvision::PendingProtonParseStatus::Invalid,
		      "zero is not a valid Proton mapping appid");
	}

	{
		const auto parsed = AppInfoProvision::parsePendingProtonText(
		    "4294967296\n");
		CHECK(parsed.status == AppInfoProvision::PendingProtonParseStatus::Invalid,
		      "overflowing appids are rejected instead of truncated");
	}

	{
		const auto parsed = AppInfoProvision::parsePendingProtonText("+123\n");
		CHECK(parsed.status == AppInfoProvision::PendingProtonParseStatus::Invalid,
		      "non-decimal token syntax is rejected");
	}

	{
		const auto parsed = AppInfoProvision::parsePendingProtonText(" \n\t");
		CHECK(parsed.status == AppInfoProvision::PendingProtonParseStatus::Valid &&
		          parsed.ids.empty(),
		      "an empty mapping file is valid and contains no ids");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
