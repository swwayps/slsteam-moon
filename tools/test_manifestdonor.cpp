#include "../src/feats/manifestdonor_policy.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

int main()
{
	assert(ManifestDonor::isBaseUrlAllowed("https://manifest.luastools.xyz"));
	assert(ManifestDonor::isBaseUrlAllowed("http://127.0.0.1:18765"));
	assert(ManifestDonor::isBaseUrlAllowed("http://localhost/test"));
	assert(ManifestDonor::isBaseUrlAllowed("http://[::1]:8080"));
	assert(!ManifestDonor::isBaseUrlAllowed("http://manifest.luastools.xyz"));
	assert(!ManifestDonor::isBaseUrlAllowed("http://127.0.0.1.example.com"));
	assert(!ManifestDonor::isBaseUrlAllowed("https://"));

	ManifestDonor::Wanted wanted;
	assert(ManifestDonor::parseWantedLine("0:123:456", wanted));
	assert(wanted.appId == 0 && wanted.depotId == 123 && wanted.gid == 456);
	assert(ManifestDonor::parseWantedLine("4294967295:4294967295:18446744073709551615", wanted));
	assert(wanted.appId == UINT32_MAX && wanted.depotId == UINT32_MAX &&
	       wanted.gid == UINT64_MAX);
	for (const auto* invalid : {
		"", "123", "1:2", "1:2:3:4", "1:0:3", "1:2:0", "-1:2:3",
		"1:2:3junk", " 1:2:3", "1:2:3 ", "4294967296:2:3",
		"1:4294967296:3", "1:2:18446744073709551616"
	})
		assert(!ManifestDonor::parseWantedLine(invalid, wanted));

	// A package load is ownership evidence only after the same package appears
	// in Steam's current license list. Metadata-only package loads must not
	// disable the managed routing preserved by fix(manifests).
	assert(!ManifestDonor::shouldObserveLicensedPackage(false, false));
	assert(!ManifestDonor::shouldObserveLicensedPackage(false, true));
	assert(!ManifestDonor::shouldObserveLicensedPackage(true, false));
	assert(ManifestDonor::shouldObserveLicensedPackage(true, true));

	// The documented passive path sends only an exact wanted manifest for a
	// depot present in the current account's license-derived depot set.
	assert(ManifestDonor::shouldSubmitCapturedCode(true, true, true, true, true));
	assert(!ManifestDonor::shouldSubmitCapturedCode(false, true, true, true, true));
	assert(!ManifestDonor::shouldSubmitCapturedCode(true, false, true, true, true));
	assert(!ManifestDonor::shouldSubmitCapturedCode(true, true, false, true, true));
	assert(!ManifestDonor::shouldSubmitCapturedCode(true, true, true, false, true));
	assert(!ManifestDonor::shouldSubmitCapturedCode(true, true, true, true, false));
	assert(ManifestDonor::shouldRetainCapturedCode(
		true, true, false, false, false));
	assert(!ManifestDonor::shouldRetainCapturedCode(
		true, true, true, false, false));
	assert(!ManifestDonor::shouldRetainCapturedCode(
		true, false, false, false, false));
	assert(!ManifestDonor::shouldRetainCapturedCode(
		true, true, false, true, false));
	assert(!ManifestDonor::shouldRetainCapturedCode(
		true, true, false, false, true));
	assert(!ManifestDonor::shouldFlushMintBatch(0, 60000));
	assert(!ManifestDonor::shouldFlushMintBatch(1, 29999));
	assert(ManifestDonor::shouldFlushMintBatch(1, 30000));
	assert(ManifestDonor::shouldForceWantedRefresh(30000, 0, 300000));
	assert(!ManifestDonor::shouldForceWantedRefresh(329999, 30000, 300000));
	assert(ManifestDonor::shouldForceWantedRefresh(330000, 30000, 300000));
	assert(!ManifestDonor::shouldForceWantedRefresh(
		30000 + 86399999LL, 30000, 86400000LL));
	assert(ManifestDonor::shouldForceWantedRefresh(
		30000 + 86400000LL, 30000, 86400000LL));

	const std::unordered_map<uint32_t, ManifestDonor::PackageRecord> packages = {
		{10, {{100, 101}, {1000, 1001}}},
		{20, {{200}, {2000}}},
	};
	const auto derived = ManifestDonor::deriveLicensedPackages(
		std::unordered_set<uint32_t>{10, 30}, packages);
	assert(derived.resolvedPackages == 1);
	assert(derived.ownedDepots.size() == 2);
	assert(derived.ownedDepots.contains(1000));
	assert(derived.ownedDepots.contains(1001));
	assert(!derived.ownedDepots.contains(2000));
	assert(derived.licensedApps.size() == 2);
	assert(derived.licensedApps[0].first == 10);
	assert(derived.licensedApps[1].first == 10);
}
