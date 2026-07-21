// Standalone test for the post-prune usable-depot guard.
//
// Build (from repo root):
//   g++ -m32 -std=c++20 -D_GLIBCXX_USE_CXX11_ABI=0 -I include \
//       tools/test_usabledepot.cpp lib/libyaml-cpp.a \
//       -o /tmp/test_usabledepot && /tmp/test_usabledepot

#include "../src/feats/usabledepot.hpp"
#include "yaml-cpp/yaml.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using AppInfoProvision::hasUsableContentDepot;

int main()
{
	// A pruned appinfo may retain depot metadata even after every content
	// depot was rejected.  Metadata alone must not make it provisionable.
	{
		YAML::Node body;
		body["depots"]["branches"]["public"]["buildid"] = "123";
		CHECK(!hasUsableContentDepot(body),
		      "metadata-only depots -> not provisionable");
	}

	// A virtual DLC advertises ownership but carries no downloadable content.
	{
		YAML::Node body;
		body["depots"]["4556380"]["dlcappid"] = "4556380";
		CHECK(!hasUsableContentDepot(body),
		      "virtual DLC only -> not provisionable");
	}

	// A manifests container is only structural until it names an actual gid.
	{
		YAML::Node body;
		body["depots"]["1229491"]["manifests"] = YAML::Node(YAML::NodeType::Map);
		CHECK(!hasUsableContentDepot(body),
		      "empty manifests -> not provisionable");
		body["depots"]["1229491"]["manifests"]["public"]["size"] = "123";
		CHECK(!hasUsableContentDepot(body),
		      "manifest without gid -> not provisionable");
	}

	// A normal keyed content depot survives pruning and makes the app usable.
	{
		YAML::Node body;
		body["depots"]["1229491"]["manifests"]["public"]["gid"] = "456";
		CHECK(hasUsableContentDepot(body),
		      "content depot -> provisionable");
	}

	// Content DLC depots have both dlcappid and manifests and remain usable.
	{
		YAML::Node body;
		body["depots"]["4229451"]["dlcappid"] = "4229450";
		body["depots"]["4229451"]["manifests"]["public"]["gid"] = "789";
		CHECK(hasUsableContentDepot(body),
		      "content DLC depot -> provisionable");
	}

	// Missing or malformed depot blocks fail closed.
	{
		YAML::Node missing;
		CHECK(!hasUsableContentDepot(missing), "missing depots -> not provisionable");
		YAML::Node scalar = YAML::Load("depots: invalid");
		CHECK(!hasUsableContentDepot(scalar), "scalar depots -> not provisionable");
	}

	if (g_failures == 0) { std::printf("\nALL PASS\n"); return 0; }
	std::printf("\n%d CHECK(S) FAILED\n", g_failures);
	return 1;
}
