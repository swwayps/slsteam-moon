
#pragma once

#include "../sdk/CWebSocketFrame.hpp"

#include <cstdint>
#include <future>


class CNetPacket;

namespace ManifestCode
{
	struct CodeRequest
	{
		uint64_t jobId = 0;
		std::future<uint64_t> result;
	};
	CodeRequest requestCode(uint32_t appId, uint32_t depotId, uint64_t gid);
	void discardRequest(uint64_t jobId);
	// Drops the donor's captured send context and pending requests.
	void resetDonorContext();
	// resetDonorContext() plus the manifest fetch session (logoff, unload).
	void resetSession();
	bool hkBBuildAndAsyncSendFrame(void* pConnection,
	                               EWebSocketOpCode eOpCode,
	                               uint8_t* pubData,
	                               uint32_t cubData);


	bool hkBRouteMsgToJob(void* pJobMgr, void* arg2, void* pMsg, void* pJob);
	// A ServiceMethodResponse from the CM. Resolves the donor's request-code
	// jobs; returns true when the packet answered one of them.
	bool recvServiceMethodResponse(CNetPacket* pkt);

	bool hkCDepotDownloadMgr_BYldRequestDepotManifest(void* pthis, uint32_t appId, uint32_t depotId, uint64_t manifestId, const char* branch, void* arg20);
}
