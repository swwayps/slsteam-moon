// SPDX-License-Identifier: AGPL-3.0-only
//
// emptydepot — pure predicate that flags a content depot whose public-branch
// manifest reports zero content (size "0").
//
// Why this exists: some titles ship empty placeholder depots (e.g. a DLC that
// only marks ownership).  Their manifest is a degenerate stub — a single file
// mapping with an EMPTY name and no real files.  When such a depot is part of
// the install plan, Steam's manifest loader trips its own invariant
//   Assert( !m_strName.IsEmpty() ) : src/common/contentmanifest.cpp:1630
// and SEGV-crashes the client while "Reconfiguring" the app for download
// (confirmed live: Dave the Diver app=1868140, depot=4394810,
// gid=6080006835337499181, size 0).
//
// A size-0 depot has nothing to install, so dropping it from the provisioned
// appinfo loses no content and keeps Steam from ever planning/loading the
// crash-inducing manifest.  Kept free of globals/I/O (only yaml-cpp) so it is
// host-unit-testable (tools/test_emptydepot.cpp); the prune glue lives in
// appinfo_provision.cpp::pruneUnsupportedDepots.

#pragma once

#include "yaml-cpp/yaml.h"

#include <string>

namespace AppInfoProvision
{
	// True iff `depotNode` is a content depot whose public-branch manifest
	// declares size "0".  Conservative: a missing manifests/public/size leaf
	// returns false (we only drop a depot we can positively prove is empty),
	// so virtual DLC entries (dlcappid, no manifests) and depots without size
	// info are left untouched.
	inline bool depotPublicManifestIsEmpty(const YAML::Node& depotNode)
	{
		if (!depotNode || !depotNode.IsMap()) return false;

		YAML::Node manifests = depotNode["manifests"];
		if (!manifests || !manifests.IsMap()) return false;

		YAML::Node pub = manifests["public"];
		if (!pub || !pub.IsMap()) return false;

		YAML::Node size = pub["size"];
		if (!size || !size.IsScalar()) return false;

		std::string s;
		try { s = size.as<std::string>(); }
		catch (...) { return false; }

		// Trim surrounding whitespace, then test for a pure-zero value.
		const auto first = s.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return false;
		const auto last = s.find_last_not_of(" \t\r\n");
		const std::string trimmed = s.substr(first, last - first + 1);

		return trimmed == "0";
	}
}
