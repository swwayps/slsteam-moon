#include "IClientAppManager.hpp"

#include "../memhlp.hpp"
#include "../vftableinfo.hpp"

#include <cstdint>


uint32_t IClientAppManager::getLibraryFolderLabel(const uint32_t index, char* pChLabel, const uint32_t labelSize)
{
	return MemHlp::callVFunc<uint32_t(*)(void*, uint32_t, char*, uint32_t)>
	(
		VFTIndexes::IClientAppManager::GetLibraryFolderLabel.index,
		this,
		index,
		pChLabel,
		labelSize
	);
}

uint32_t IClientAppManager::getLibraryFolderPath(const uint32_t index, char* pChPath, const uint32_t pathSize)
{
	return MemHlp::callVFunc<uint32_t(*)(void*, uint32_t, char*, uint32_t)>
	(
		VFTIndexes::IClientAppManager::GetLibraryFolderPath.index,
		this,
		index,
		pChPath,
		pathSize
	);
}

uint32_t IClientAppManager::getNumLibraryFolders()
{
	return MemHlp::callVFunc<uint32_t(*)(void*)>(VFTIndexes::IClientAppManager::GetNumLibraryFolders.index, this);
}

bool IClientAppManager::installApp(const AppId_t appId, const uint32_t libraryIndex)
{
	return MemHlp::callVFunc<bool(*)(void*, AppId_t, uint32_t, uint8_t)>(VFTIndexes::IClientAppManager::InstallApp.index, this, appId, libraryIndex, 0);
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
