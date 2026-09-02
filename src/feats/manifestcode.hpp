
#pragma once

#include "../sdk/CWebSocketFrame.hpp"

#include <cstdint>


namespace ManifestCode
{
	bool hkBBuildAndAsyncSendFrame(void* pConnection,
	                               EWebSocketOpCode eOpCode,
	                               uint8_t* pubData,
	                               uint32_t cubData);

	void* hkRecvPkt(void* pManager, CRemoteClientPacket* pPacket);

	bool hkBRouteMsgToJob(void* pJobMgr, void* arg2, void* pMsg, void* pJob);

	bool hkCDepotDownloadMgr_BYldRequestDepotManifest(void* pthis, uint32_t appId, uint32_t depotId, uint64_t manifestId, const char* branch, void* arg20);
}
