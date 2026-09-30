#pragma once

#include "types.hpp"

#include <cstdint>


class IClientAppManager;
class IClientApps;
class IClientUser;

enum class ECallbackType : uint32_t
{
	LicensesUpdate_t = 0x7d,
	AppOwnershipTicketReceived_t = 0xf907c,
	AppLicensesChanged_t = 0xf90be
};

SDK_Struct AppOwnershipTicketReceived_t
{
	EResult result;
	uint32_t appId;
};

SDK_Struct AppLicensesChanged_t
{
	static constexpr unsigned int MAX_APPS_PER_CALLBACK = 0x40;

	bool reloadAll;							//0x0
	bool firstLoad;							//0x1
	uint8_t __pad_0x2[0x2];					//0x2
	uint32_t remainingPackets;				//0x4
	uint32_t count;							//0x8
	uint32_t apps[MAX_APPS_PER_CALLBACK];	//0xC
	uint64_t appsAdded;						//0x10C
}; //0x114

SDK_Struct AppOwnershipInfo_t {
    AppId_t subId;
    EAppReleaseState releaseState;
    uint32_t owner;
    AppId_t masterSubscriptionAppId;
    uint32_t trialTime;
    uint32_t numLicenses;
    char region[4]; //Client copies this like a DWORD, so even though CountryCodes are only 2 bytes 4 seems to be correct
    uint32_t purchaseTime;
    uint32_t realOwner;
    bool ownsLicense;
    bool licenseExpired;
    bool field12_0x26;
    bool lowViolence;
    bool freeLicense;
    bool regionRestricted;
    bool fromFreeWeekend;
    bool licenseLocked;
    bool licensePending;
    bool retailLicense;
    bool autoGrant;
    bool licensePermanent;
    bool field21_0x30;
    bool field22_0x31;
    bool siteLicense;
    bool field24_0x33;
    bool field25_0x34;
    bool familyShared;
    bool field27_0x36;
    bool field28_0x37;
}; //0x38

SDK_Class CUser
{
public:
	IClientAppManager* getAppManager();
	IClientApps* getClientApps();
	IClientUser* getClientUser();
	bool checkAppOwnership(uint32_t appId, AppOwnershipInfo_t* pInfo);
	bool isSubscribed(uint32_t appId);

	void postCallback(ECallbackType type, void* pCallback, uint32_t callbackSize);
	void updateAppOwnershipTicket(uint32_t appId, void* pTicket, uint32_t len);

	// Broadcast a LicensesUpdated_t callback rebuilt from this user's
	// current license state.  Used after injecting AdditionalApps into
	// package 0 to force Steam's ownership/library layer to re-read
	// licenses (and package 0).  No-op if the underlying pattern did
	// not resolve.  Returns true if the call was made.
	bool notifyLicensesUpdated();
};
