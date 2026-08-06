#include "steam.hpp"

#include "../log.hpp"

#include <dlfcn.h>
#include <sstream>

namespace Steam
{
	Plat_Alloc_t Plat_Alloc;
	Plat_Free_t Plat_Free;
	Plat_Realloc_t Plat_Realloc;
}

void Steam::free(void* mem)
{
	Plat_Free(mem);
}

bool Steam::init()
{
	void* tier0 = dlopen("libtier0_s.so", RTLD_NOW);
	if (!tier0)
	{
		return false;
	}

	Plat_Alloc = reinterpret_cast<Plat_Alloc_t>(dlsym(tier0, "Plat_Alloc"));
	Plat_Free = reinterpret_cast<Plat_Free_t>(dlsym(tier0, "Plat_Free"));
	Plat_Realloc = reinterpret_cast<Plat_Realloc_t>(dlsym(tier0, "Plat_Realloc"));

	if (!Plat_Alloc || !Plat_Free || !Plat_Realloc)
	{
		return false;
	}

	g_pLog->debug("Plat_Alloc at %p\n", Plat_Alloc);
	g_pLog->debug("Plat_Free at %p\n", Plat_Free);
	g_pLog->debug("Plat_Realloc at %p\n", Plat_Realloc);

	return true;
}

std::string EIPCCmd_ToString(const EIPCCmd cmd)
{
	switch(cmd)
	{
		case EIPCCmd::RunInterface: return "RunInterface";
		case EIPCCmd::SerializeCallbacks: return "SerializeCallbacks";
		case EIPCCmd::CreateGlobalUser: return "CreateGlobalUser";
		case EIPCCmd::DisconnectGlobalUser: return "DisconnectGlobalUser";
		case EIPCCmd::ClosePipe: return "ClosePipe";
		case EIPCCmd::Heartbeat: return "Heartbeat";
		case EIPCCmd::ConnectPipe: return "ConnectPipe";
	}

	std::ostringstream ss;
	ss << "Unknown IPCCmd 0x" << std::hex << static_cast<unsigned int>(cmd);
	return ss.str();
}

std::string EIPCInterface_ToString(const EIPCInterface interface)
{
	switch(interface)
	{
		case EIPCInterface::User: return "User";
		case EIPCInterface::GameServerInternal: return "GameServerInternal";
		case EIPCInterface::Friends: return "Friends";
		case EIPCInterface::Utils: return "Utils";
		case EIPCInterface::Billing: return "Billing";
		case EIPCInterface::Matchmaking: return "Matchmaking";
		case EIPCInterface::Apps: return "Apps";
		case EIPCInterface::UserStats: return "UserStats";
		case EIPCInterface::Networking: return "Networking";
		case EIPCInterface::RemoteStorage: return "RemoteStorage";
		case EIPCInterface::DepotBuilder: return "DepotBuilder";
		case EIPCInterface::AppManager: return "AppManager";
		case EIPCInterface::ConfigStore: return "ConfigStore";
		case EIPCInterface::GameCoordinator: return "GameCoordinator";
		case EIPCInterface::GameServerStats: return "GameServerStats";
		case EIPCInterface::GameStats: return "GameStats";
		case EIPCInterface::HTTP: return "HTTP";
		case EIPCInterface::Screenshots: return "Screenshots";
		case EIPCInterface::Audio: return "Audio";
		case EIPCInterface::UnifiedMessages: return "UnifiedMessages";
		case EIPCInterface::StreamLauncher: return "StreamLauncher";
		case EIPCInterface::ParentalSettings: return "ParentalSettings";
		case EIPCInterface::NetworkDeviceManager: return "NetworkDeviceManager";
		case EIPCInterface::Music: return "Music";
		case EIPCInterface::RemoteClientManager: return "RemoteClientManager";
		case EIPCInterface::UGC: return "UGC";
		case EIPCInterface::StreamClient: return "StreamClient";
		case EIPCInterface::ProductBuilder: return "ProductBuilder";
		case EIPCInterface::Shortcuts: return "Shortcuts";
		case EIPCInterface::GameNotifications: return "GameNotifications";
		case EIPCInterface::Video: return "Video";
		case EIPCInterface::Inventory: return "Inventory";
		case EIPCInterface::VR: return "VR";
		case EIPCInterface::ControllerSerialized: return "ControllerSerialized";
		case EIPCInterface::AppDisableUpdate: return "AppDisableUpdate";
		case EIPCInterface::SharedConnection: return "SharedConnection";
		case EIPCInterface::Shader: return "Shader";
		case EIPCInterface::NetworkingSocketsSerialized: return "NetworkingSocketsSerialized";
		case EIPCInterface::Compat: return "Compat";
		case EIPCInterface::Parties: return "Parties";
		case EIPCInterface::NetworkingUtilsSerialized: return "NetworkingUtilsSerialized";
		case EIPCInterface::RemotePlay: return "RemotePlay";
		case EIPCInterface::GameServerPacketHandler: return "GameServerPacketHandler";
		case EIPCInterface::SystemManager: return "SystemManager";
		case EIPCInterface::SystemPerfManager: return "SystemPerfManager";
		case EIPCInterface::SystemDockManager: return "SystemDockManager";
		case EIPCInterface::SystemAudioManager: return "SystemAudioManager";
		case EIPCInterface::SystemDisplayManager: return "SystemDisplayManager";
		case EIPCInterface::Timeline: return "Timeline";
	}

	std::ostringstream ss;
	ss << "Unknown IPCInterface 0x" << std::hex << static_cast<unsigned int>(interface);
	return ss.str();
}
