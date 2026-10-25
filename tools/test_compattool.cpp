// Unit tests for CompatTool::parseDefaultTool — reading the user's default
// Steam Play compatibility tool (the "0" key in CompatToolMapping) out of a
// config.vdf text body.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_compattool.cpp -o /tmp/test_compattool && /tmp/test_compattool

#include "../src/feats/compattool.hpp"

#include <cstdio>
#include <string>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_EQ(got, want, msg)                                              \
	do {                                                                      \
		++g_checks;                                                           \
		std::string g = (got), w = (want);                                    \
		if (g != w) {                                                         \
			std::printf("FAIL: %s (got \"%s\", want \"%s\")\n",               \
			            msg, g.c_str(), w.c_str());                           \
			++g_failures;                                                     \
		}                                                                     \
	} while (0)

int main()
{
	// Realistic config.vdf slice: per-app entries plus the "0" default.
	const std::string full =
		"\"InstallConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n"
		"\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n"
		"\t\t\t\t\"CompatToolMapping\"\n\t\t\t\t{\n"
		"\t\t\t\t\t\"3949040\"\n\t\t\t\t\t{\n"
		"\t\t\t\t\t\t\"name\"\t\t\"proton_experimental\"\n"
		"\t\t\t\t\t\t\"config\"\t\t\"\"\n"
		"\t\t\t\t\t\t\"priority\"\t\t\"250\"\n\t\t\t\t\t}\n"
		"\t\t\t\t\t\"0\"\n\t\t\t\t\t{\n"
		"\t\t\t\t\t\t\"name\"\t\t\"proton-cachyos\"\n"
		"\t\t\t\t\t\t\"config\"\t\t\"\"\n"
		"\t\t\t\t\t\t\"priority\"\t\t\"75\"\n\t\t\t\t\t}\n"
		"\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
	CHECK_EQ(CompatTool::parseDefaultTool(full), "proton-cachyos",
	         "reads the default tool from the \"0\" key");

	// Default appears before any per-app entry.
	const std::string defaultFirst =
		"\"CompatToolMapping\"\n{\n"
		"\t\"0\"\n\t{\n\t\t\"name\"\t\t\"proton_9\"\n\t\t\"priority\"\t\t\"75\"\n\t}\n"
		"\t\"638510\"\n\t{\n\t\t\"name\"\t\t\"proton_experimental\"\n\t}\n}\n";
	CHECK_EQ(CompatTool::parseDefaultTool(defaultFirst), "proton_9",
	         "default-first layout");

	// No "0" default set -> empty (caller falls back to experimental).
	const std::string noDefault =
		"\"CompatToolMapping\"\n{\n"
		"\t\"284160\"\n\t{\n\t\t\"name\"\t\t\"proton_experimental\"\n\t}\n}\n";
	CHECK_EQ(CompatTool::parseDefaultTool(noDefault), "",
	         "no default key -> empty");

	// Empty mapping block -> empty.
	CHECK_EQ(CompatTool::parseDefaultTool("\"CompatToolMapping\"\n{\n}\n"), "",
	         "empty mapping -> empty");

	// No CompatToolMapping at all -> empty.
	CHECK_EQ(CompatTool::parseDefaultTool("\"Steam\"\n{\n}\n"), "",
	         "no mapping block -> empty");

	// App ids containing the digit 0 must not be mistaken for the "0" key.
	const std::string trickyIds =
		"\"CompatToolMapping\"\n{\n"
		"\t\"100\"\n\t{\n\t\t\"name\"\t\t\"a\"\n\t}\n"
		"\t\"284160\"\n\t{\n\t\t\"name\"\t\t\"b\"\n\t}\n"
		"\t\"3949040\"\n\t{\n\t\t\"name\"\t\t\"c\"\n\t}\n}\n";
	CHECK_EQ(CompatTool::parseDefaultTool(trickyIds), "",
	         "ids with 0 digits are not the default key");

	// Malformed (unbalanced) input must not crash and returns empty.
	CHECK_EQ(CompatTool::parseDefaultTool("\"CompatToolMapping\"\n{\n\t\"0\"\n\t{"), "",
	         "unbalanced object -> empty (no crash)");

	// An app counts as mapped only through a key directly inside the block.
	// The same id later in config.vdf (here a depots entry) or as a value
	// must not suppress the Proton mapping.
	const std::string laterDepot =
		"\"Steam\"\n{\n"
		"\t\"CompatToolMapping\"\n\t{\n"
		"\t\t\"7\"\n\t\t{\n\t\t\t\"name\"\t\t\"x\"\n\t\t\t\"priority\"\t\t\"42\"\n\t\t}\n"
		"\t}\n"
		"\t\"depots\"\n\t{\n\t\t\"42\"\n\t\t{\n\t\t\t\"CDN\"\t\t\"1\"\n\t\t}\n\t}\n}\n";
	CHECK_EQ(CompatTool::hasAppMapping(laterDepot, 42) ? "mapped" : "absent", "absent",
	         "an id outside the block or used as a value is not a mapping");
	CHECK_EQ(CompatTool::hasAppMapping(laterDepot, 7) ? "mapped" : "absent", "mapped",
	         "a key inside the block is a mapping");
	CHECK_EQ(CompatTool::hasAppMapping("\"Steam\"\n{\n}\n", 7) ? "mapped" : "absent", "absent",
	         "no mapping block -> not mapped");
	if (g_failures == 0)
		std::printf("OK: %d checks passed\n", g_checks);
	else
		std::printf("FAILED: %d/%d checks failed\n", g_failures, g_checks);
	return g_failures == 0 ? 0 : 1;
}
