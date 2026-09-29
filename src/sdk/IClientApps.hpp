#pragma once

#include "protobufs/enums.pb.h"

#include "types.hpp"

#include <cstdint>
#include <vector>

using EAppType = EProtoAppType;

enum EAppInfoSection
{
	k_EAppInfoSectionUnknown = 0x0,
	k_EAppInfoSectionAll = 0x1,
	k_EAppInfoSectionCommon = 0x2,
	k_EAppInfoSectionExtended = 0x3,
	k_EAppInfoSectionConfig = 0x4,
	k_EAppInfoSectionStats = 0x5,
	k_EAppInfoSectionInstall = 0x6,
	k_EAppInfoSectionDepots = 0x7,
	k_EAppInfoSectionVAC = 0x8,
	k_EAppInfoSectionDRM = 0x9,
	k_EAppInfoSectionUFS = 0xa,
	k_EAppInfoSectionOGG = 0xb,
	k_EAppInfoSectionItems = 0xc,
	k_EAppInfoSectionPolicies = 0xd,
	k_EAppInfoSectionSysreqs = 0xe,
	k_EAppInfoSectionCommunity = 0xf,
	k_EAppInfoSectionStore = 0x10,
	k_EAppInfoSectionLocalization = 0x11,
	k_EAppInfoSectionBroadcastgamedata = 0x12,
	k_EAppInfoSectionComputed = 0x13,
	k_EAppInfoSectionAlbummetadata = 0x14
};

class IClientApps
{
public:
	int32_t getAppData(uint32_t appId, const char* name, const char* pChOut, uint32_t outSize);
	uint32_t getAppDataSection(uint32_t appId, EAppInfoSection section, const char* pChOut, uint32_t outSize);
	bool requestAppInfoUpdate(const std::vector<uint32_t>& appIds);
	EAppType getAppType(uint32_t appId);
};

extern IClientApps* g_pClientApps;
