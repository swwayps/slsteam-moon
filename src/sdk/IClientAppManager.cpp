#include "IClientAppManager.hpp"

#include "../memhlp.hpp"
#include "../vftableinfo.hpp"

#include <cstdint>

bool IClientAppManager::installApp(const AppId_t appId, const uint32_t librarIndex)
{
	return MemHlp::callVFunc<bool(*)(void*, AppId_t, uint32_t, uint8_t)>(VFTIndexes::IClientAppManager::InstallApp.index, this, appId, librarIndex, 0);
}

uint32_t IClientAppManager::uninstallApp(const AppId_t appId)
{
	return MemHlp::callVFunc<uint32_t(*)(void*, AppId_t)>(VFTIndexes::IClientAppManager::UninstallApp.index, this, appId);
}

EAppState IClientAppManager::getAppInstallState(const AppId_t appId)
{
	return MemHlp::callVFunc<EAppState(*)(void*, AppId_t)>(VFTIndexes::IClientAppManager::GetAppInstallState.index, this, appId);
}

IClientAppManager* g_pClientAppManager;
