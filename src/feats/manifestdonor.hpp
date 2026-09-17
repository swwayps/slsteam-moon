#pragma once

#include <cstdint>

class CMsgClientLicenseList;
struct PackageInfo;

namespace ManifestDonor
{
	void observePackage(const PackageInfo* package, bool available);
	void onLicenseList(const CMsgClientLicenseList* message);
	void onLoggedOff();
	uint64_t sessionGeneration();
	void observeDepot(uint32_t appId, uint32_t depotId, uint64_t gid);
	void submitCapturedCode(uint32_t depotId, uint64_t gid, uint64_t code,
	                        uint64_t generation);
	void stop();
}
