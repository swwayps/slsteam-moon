// Standalone test for the empty-depot predicate (emptydepot.hpp).
//
// A content depot whose public-branch manifest reports size 0 has no files;
// its manifest is a degenerate stub (a single file mapping with an empty
// name).  Steam's own loader asserts !m_strName.IsEmpty()
// (src/common/contentmanifest.cpp:1630) and SEGV-crashes when it reconfigures
// such a depot for download.  We therefore drop these depots during prune so
// Steam never plans them.  This predicate is the pure decision; it only
// touches yaml-cpp so it is host-unit-testable.
//
// Build: make test-emptydepot

#include "../src/feats/emptydepot.hpp"
#include "yaml-cpp/yaml.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using AppInfoProvision::depotPublicManifestIsEmpty;

int main()
{
	// 1) Real empty depot (size "0") — the crash trigger.  This mirrors
	//    Dave the Diver depot 4394810: dlcappid + manifests.public.size "0".
	{
		YAML::Node d;
		d["dlcappid"] = "4394810";
		d["manifests"]["public"]["gid"]  = "6080006835337499181";
		d["manifests"]["public"]["size"] = "0";
		CHECK(depotPublicManifestIsEmpty(d), "size \"0\" -> empty (drop)");
	}

	// 2) Normal depot with real content -> not empty (keep).
	{
		YAML::Node d;
		d["manifests"]["public"]["gid"]  = "4277352553098425670";
		d["manifests"]["public"]["size"] = "71790521";
		CHECK(!depotPublicManifestIsEmpty(d), "non-zero size -> not empty");
	}

	// 3) Virtual DLC entry (dlcappid only, no manifests) -> not empty.
	//    These carry no payload and are kept; they are never staged.
	{
		YAML::Node d;
		d["dlcappid"] = "2463870";
		CHECK(!depotPublicManifestIsEmpty(d), "no manifests block -> not empty");
	}

	// 4) Has manifests but no public branch -> not empty (nothing to judge).
	{
		YAML::Node d;
		d["manifests"]["beta"]["size"] = "0";
		CHECK(!depotPublicManifestIsEmpty(d), "no public branch -> not empty");
	}

	// 5) public branch present but no size key -> not empty (don't guess).
	{
		YAML::Node d;
		d["manifests"]["public"]["gid"] = "123";
		CHECK(!depotPublicManifestIsEmpty(d), "public without size -> not empty");
	}

	// 6) Defensive: undefined / non-map nodes never look empty.
	{
		YAML::Node undef;
		CHECK(!depotPublicManifestIsEmpty(undef), "undefined node -> not empty");
		YAML::Node scalar = YAML::Load("just-a-string");
		CHECK(!depotPublicManifestIsEmpty(scalar), "scalar node -> not empty");
	}

	// 7) size "0" with surrounding whitespace still counts as empty
	//    (KV1 values are clean, but be robust to a stray space).
	{
		YAML::Node d;
		d["manifests"]["public"]["size"] = " 0 ";
		CHECK(depotPublicManifestIsEmpty(d), "whitespace-padded \"0\" -> empty");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
