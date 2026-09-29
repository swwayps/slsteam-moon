#include "IClientAppManager.hpp"

#include "../memhlp.hpp"
#include "../vftableinfo.hpp"

#include <cstdint>

bool IClientAppManager::installApp(uint32_t appId, uint32_t librarIndex)
{
	return MemHlp::callVFunc<bool(*)(void*, uint32_t, uint32_t, uint8_t)>(VFTIndexes::IClientAppManager::InstallApp, this, appId, librarIndex, 0);
}

uint32_t IClientAppManager::uninstallApp(uint32_t appId)
{
	return MemHlp::callVFunc<uint32_t(*)(void*, uint32_t)>(VFTIndexes::IClientAppManager::UninstallApp, this, appId);
}

EAppState IClientAppManager::getAppInstallState(uint32_t appId)
{
	return MemHlp::callVFunc<EAppState(*)(void*, uint32_t)>(VFTIndexes::IClientAppManager::GetAppInstallState, this, appId);
}

IClientAppManager* g_pClientAppManager;
