#include "IClientApps.hpp"

#include "../memhlp.hpp"
#include "../vftableinfo.hpp"

#include <cstdint>
#include <limits>

int32_t IClientApps::getAppData(uint32_t appId, const char* name, const char* pChOut, uint32_t outSize)
{
	return MemHlp::callVFunc<uint32_t(*)(void*, uint32_t, const char*, const char*, uint32_t)>
	(
		 VFTIndexes::IClientApps::GetAppData.index,
		 this,
		 appId,
		 name,
		 pChOut,
		 outSize
	);
}

bool IClientApps::requestAppInfoUpdate(const std::vector<uint32_t>& appIds)
{
	if (appIds.empty() ||
		appIds.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
	{
		return false;
	}
	return MemHlp::callVFunc<
		bool(*)(void*, const uint32_t*, int)>(
			VFTIndexes::IClientApps::RequestAppInfoUpdate.index,
			this,
			appIds.data(),
			static_cast<int>(appIds.size()));
}

EAppType IClientApps::getAppType(uint32_t appId)
{
	return MemHlp::callVFunc<EAppType(*)(void*, uint32_t)>(VFTIndexes::IClientApps::GetAppType.index, this, appId);
}

IClientApps* g_pClientApps;
