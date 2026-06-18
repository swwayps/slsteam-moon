
#include "manifestcode.hpp"

#include "depotkey.hpp"
#include "manifestid.hpp"
#include "achievements.hpp"
#include "playerstats.hpp"

#include "../config.hpp"
#include "../hooks.hpp"
#include "../log.hpp"

#include "../sdk/EResult.hpp"
#include "../sdk/protobufs/steammessages_base.pb.h"
#include "../sdk/protobufs/steammessages_contentserverdirectory.pb.h"

#include "../utils/ManifestFetch.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>


namespace ManifestCode
{
namespace
{

constexpr uint32_t kMaxBodySize    = 262144;
constexpr uint32_t kMaxHdrSize     = 1024;
constexpr uint32_t kMaxPacketSize  = sizeof(MsgHdr) + kMaxHdrSize + kMaxBodySize;
constexpr int      kPacketPoolSize = 8;

std::mutex g_RxLock;
std::mutex g_TxLock;

uint8_t  g_RxBody[kMaxBodySize];
uint32_t g_RxBodyLen = 0;
uint8_t  g_RxHdr[kMaxHdrSize];
uint32_t g_RxHdrLen  = 0;
bool     g_PatchRx    = false;
bool     g_PatchRxHdr = false;

uint8_t  g_RxPool[kPacketPoolSize][kMaxPacketSize];
int      g_RxPoolIdx = 0;

// Outgoing-frame replacement (used to spoof a Player.GetUserStats request).
// Guarded by g_TxLock; valid only for the duration of one send-hook call.
uint8_t  g_TxFrame[kMaxPacketSize];
uint32_t g_TxFrameLen = 0;
bool     g_PatchTx    = false;

constexpr uint32_t fnvHash(const char* s)
{
	uint32_t h = 0x811c9dc5u;
	while (*s)
	{
		h ^= static_cast<uint32_t>(static_cast<unsigned char>(*s++));
		h *= 0x01000193u;
	}
	return h;
}

constexpr uint32_t kHashGetManifestRequestCode =
    fnvHash("ContentServerDirectory.GetManifestRequestCode#1");

constexpr uint32_t kHashPlayerGetUserStats =
    fnvHash("Player.GetUserStats#1");



inline bool decodeFrame(const uint8_t* data, uint32_t size,
                        uint32_t& eMsg,
                        const uint8_t*& pHdr, uint32_t& cbHdr,
                        const uint8_t*& pBody, uint32_t& cbBody)
{
	if (!data || size < sizeof(MsgHdr))
	{
		return false;
	}
	const auto* hdr = reinterpret_cast<const MsgHdr*>(data);
	if (!(hdr->eMsg & kMsgHdrProtoFlag))
	{
		return false;
	}
	eMsg  = hdr->eMsg & ~kMsgHdrProtoFlag;
	cbHdr = hdr->headerLength;
	const uint32_t off = sizeof(MsgHdr) + cbHdr;
	if (off > size)
	{
		return false;
	}
	pHdr   = data + sizeof(MsgHdr);
	pBody  = data + off;
	cbBody = size - off;
	return true;
}

inline void patchRecvFrame(CNetPacket* p,
                           const uint8_t* pNewHdr, uint32_t cbNewHdr,
                           const uint8_t* pNewBody, uint32_t cbNewBody)
{
	const uint32_t newSize = sizeof(MsgHdr) + cbNewHdr + cbNewBody;
	if (newSize > sizeof(g_RxPool[0])) return;

	std::lock_guard<std::mutex> lk(g_RxLock);
	uint8_t* buf = g_RxPool[g_RxPoolIdx];
	const auto* orig = reinterpret_cast<const MsgHdr*>(p->m_pubData);
	auto* out = reinterpret_cast<MsgHdr*>(buf);
	out->eMsg         = orig->eMsg;
	out->headerLength = cbNewHdr;
	std::memcpy(buf + sizeof(MsgHdr), pNewHdr, cbNewHdr);
	if (cbNewBody)
	{
		std::memcpy(buf + sizeof(MsgHdr) + cbNewHdr, pNewBody, cbNewBody);
	}
	p->m_pubData = buf;
	p->m_cubData = newSize;

	g_RxPoolIdx = (g_RxPoolIdx + 1) % kPacketPoolSize;
}

// Assemble a replacement outgoing frame (MsgHdr + header + new body) into
// g_TxFrame and flag it for the send hook. `rawEMsg` must keep the proto
// flag. Caller holds g_TxLock.
inline void buildReplacementFrame(uint32_t rawEMsg,
                                  const uint8_t* pHdr, uint32_t cbHdr,
                                  const uint8_t* pNewBody, uint32_t cbNewBody)
{
	const uint32_t newSize = sizeof(MsgHdr) + cbHdr + cbNewBody;
	if (newSize > sizeof(g_TxFrame))
	{
		return;
	}
	auto* out = reinterpret_cast<MsgHdr*>(g_TxFrame);
	out->eMsg         = rawEMsg;
	out->headerLength = cbHdr;
	std::memcpy(g_TxFrame + sizeof(MsgHdr), pHdr, cbHdr);
	if (cbNewBody)
	{
		std::memcpy(g_TxFrame + sizeof(MsgHdr) + cbHdr, pNewBody, cbNewBody);
	}
	g_TxFrameLen = newSize;
	g_PatchTx    = true;
}

// Outgoing Player.GetUserStats#1 (eMsg 151): the modern library page fetches
// a game's achievement schema through this unified method. For an
// AdditionalApp the account doesn't own server-side, the request returns
// empty and the Achievements tab never appears. Rewrite the steamid to a
// real owner of the game so the server returns a populated schema.
void handleSend_PlayerGetUserStats(const uint8_t* pBody, uint32_t cbBody,
                                   const uint8_t* pHdr, uint32_t cbHdr,
                                   uint32_t eMsg)
{
	if (!g_config.achievements.get())
	{
		return;
	}

	const auto appId = PlayerStats::parseRequestAppId(pBody, cbBody);
	if (!appId || !g_config.isAddedAppId(*appId))
	{
		return;
	}

	const uint64_t owner = Achievements::resolveOwnerSteamId(
	    *appId,
	    g_config.achievementOwners.get(),
	    g_config.achievementOwnerId.get());

	const auto newBody = PlayerStats::buildSpoofedRequest(owner, *appId);
	buildReplacementFrame(eMsg | kMsgHdrProtoFlag, pHdr, cbHdr,
	                      newBody.data(), static_cast<uint32_t>(newBody.size()));

	g_pLog->debug("Achievements: spoofing Player.GetUserStats owner for %u\n", *appId);
}



void handleSend_GetManifestRequestCode(const uint8_t* pBody, uint32_t cbBody,
                                       const uint8_t* pHdr, uint32_t cbHdr)
{
	CContentServerDirectory_GetManifestRequestCode_Request req;
	if (!req.ParseFromArray(pBody, cbBody))
	{
		g_pLog->warn("ManifestCode send: parse failed (cbBody=%u)\n", cbBody);
		return;
	}
	if (!req.has_depot_id() || !req.has_manifest_id())
	{
		g_pLog->debug("ManifestCode send: depot/manifest missing, skip\n");
		return;
	}
	const uint32_t depotId = req.depot_id();
	const uint64_t gid     = req.manifest_id();
	const uint32_t appId   = req.has_app_id() ? req.app_id() : 0;

	const bool inScope =
	    (appId   && g_config.isAddedAppId(appId))
	    || (depotId && g_config.isAddedAppId(depotId))
	    || DepotKey::isManagedDepot(depotId)
	    || !ManifestId::getPinnedGid(depotId).empty();
	if (!inScope)
	{
		g_pLog->debug("ManifestCode send: app=%u depot=%u gid=%llu not in scope, skip\n",
		              appId, depotId, static_cast<unsigned long long>(gid));
		return;
	}

	CMsgProtoBufHeader hdr;
	if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_jobid_source())
	{
		g_pLog->warn("ManifestCode send: missing jobid_source, skip\n");
		return;
	}
	const uint64_t jobId = hdr.jobid_source();

	g_pLog->info("ManifestCode send: depot=%u gid=%llu app=%u jobid=%llu\n",
	             depotId, static_cast<unsigned long long>(gid),
	             appId, static_cast<unsigned long long>(jobId));
	ManifestFetch::submit(jobId, gid, appId, depotId);

	ManifestFetch::submitManifestBlob(gid, appId, depotId);
}

void handleRecv_GetManifestRequestCode(const uint8_t* pHdr, uint32_t cbHdr,
                                       const uint8_t* pBody, uint32_t cbBody)
{
	(void)pBody;
	CMsgProtoBufHeader hdr;
	if (!hdr.ParseFromArray(pHdr, cbHdr))
	{
		g_pLog->warn("ManifestCode recv: header parse failed\n");
		return;
	}
	if (!hdr.has_jobid_target())
	{
		g_pLog->debug("ManifestCode recv: no jobid_target\n");
		return;
	}
	const uint64_t jobId = hdr.jobid_target();

	auto resolved = ManifestFetch::resolve(jobId);
	if (!resolved)
	{
		g_pLog->debug("ManifestCode recv: jobid=%llu no patch (cbBody=%u eresult=%d)\n",
		              static_cast<unsigned long long>(jobId), cbBody, hdr.eresult());
		return;
	}

	hdr.set_eresult(static_cast<int32_t>(ERESULT_OK));
	const std::size_t hdrSize = hdr.ByteSizeLong();
	if (hdrSize > kMaxHdrSize || !hdr.SerializeToArray(g_RxHdr, kMaxHdrSize))
	{
		g_pLog->warn("ManifestCode recv: header re-serialise failed (size=%zu)\n", hdrSize);
		return;
	}
	g_RxHdrLen = static_cast<uint32_t>(hdrSize);

	CContentServerDirectory_GetManifestRequestCode_Response resp;
	resp.set_manifest_request_code(*resolved);
	const std::size_t bodySize = resp.ByteSizeLong();
	if (bodySize > kMaxBodySize || !resp.SerializeToArray(g_RxBody, kMaxBodySize))
	{
		g_pLog->warn("ManifestCode recv: body re-serialise failed (size=%zu)\n", bodySize);
		return;
	}
	g_RxBodyLen = static_cast<uint32_t>(bodySize);

	g_PatchRxHdr = true;
	g_PatchRx    = true;
	g_pLog->info("ManifestCode recv: jobid=%llu injected code=%llu (orig cbBody=%u)\n",
	             static_cast<unsigned long long>(jobId),
	             static_cast<unsigned long long>(*resolved),
	             cbBody);
}



void dispatchSend(uint32_t eMsg,
                  const uint8_t* pBody, uint32_t cbBody,
                  const uint8_t* pHdr,  uint32_t cbHdr)
{
	if (eMsg != kEMsgServiceMethodCallFromClient)
	{
		return;
	}
	CMsgProtoBufHeader hdr;
	if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_target_job_name())
	{
		return;
	}
	g_pLog->debug("WebSocket service method send: %s\n", hdr.target_job_name().c_str());
	const auto h = fnvHash(hdr.target_job_name().c_str());
	switch (h)
	{
	case kHashGetManifestRequestCode:
		handleSend_GetManifestRequestCode(pBody, cbBody, pHdr, cbHdr);
		return;
	case kHashPlayerGetUserStats:
		handleSend_PlayerGetUserStats(pBody, cbBody, pHdr, cbHdr, eMsg);
		return;
	default:
		return;
	}
}

void dispatchRecv(uint32_t eMsg,
                  const uint8_t* pBody, uint32_t cbBody,
                  const uint8_t* pHdr,  uint32_t cbHdr)
{
	if (eMsg != kEMsgServiceMethodResponse)
	{
		return;
	}
	CMsgProtoBufHeader hdr;
	if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_target_job_name())
	{
		return;
	}
	g_pLog->debug("WebSocket service method recv: %s\n", hdr.target_job_name().c_str());
}

} // namespace



bool hkBBuildAndAsyncSendFrame(void* pConnection,
                               EWebSocketOpCode eOpCode,
                               uint8_t* pubData,
                               uint32_t cubData)
{
	if (eOpCode == k_eWebSocketOpCode_Binary)
	{
		uint32_t eMsg = 0;
		const uint8_t* pHdr  = nullptr;
		const uint8_t* pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		if (decodeFrame(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody))
		{
			std::lock_guard<std::mutex> lk(g_TxLock);
			g_PatchTx = false;
			dispatchSend(eMsg, pBody, cbBody, pHdr, cbHdr);
			if (g_PatchTx)
			{
				return Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn(
				    pConnection, eOpCode, g_TxFrame, g_TxFrameLen);
			}
		}
	}
	return Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn(
	    pConnection, eOpCode, pubData, cubData);
}

void* hkRecvPkt(void* pManager, CNetPacket* pPacket)
{
	if (pPacket && pPacket->m_pubData && pPacket->m_cubData)
	{
		uint32_t eMsg = 0;
		const uint8_t* pHdr  = nullptr;
		const uint8_t* pBody = nullptr;
		uint32_t cbHdr = 0, cbBody = 0;
		g_PatchRx    = false;
		g_PatchRxHdr = false;

		if (decodeFrame(pPacket->m_pubData, pPacket->m_cubData,
		                eMsg, pHdr, cbHdr, pBody, cbBody))
		{
			dispatchRecv(eMsg, pBody, cbBody, pHdr, cbHdr);

			if (g_PatchRxHdr || g_PatchRx)
			{
				patchRecvFrame(pPacket,
				               g_PatchRxHdr ? g_RxHdr  : pHdr,
				               g_PatchRxHdr ? g_RxHdrLen : cbHdr,
				               g_PatchRx    ? g_RxBody : pBody,
				               g_PatchRx    ? g_RxBodyLen : cbBody);
			}
		}
	}
	return Hooks::CRemoteClientManager_RecvPkt.tramp.fn(pManager, pPacket);
}


namespace
{
	constexpr int kRewritePoolSize  = 8;
	constexpr uint32_t kMaxRewriteSize = 4096;
	uint8_t  g_RewritePool[kRewritePoolSize][kMaxRewriteSize];
	int      g_RewritePoolIdx = 0;
	std::mutex g_RewriteLock;
}

bool hkBRouteMsgToJob(void* pJobMgr, void* arg2, void* pMsg, void* pJob)
{
	auto peek32 = [](const void* p, int off) -> uint32_t {
		if (!p) return 0;
		return *reinterpret_cast<const uint32_t*>(
		    static_cast<const uint8_t*>(p) + off);
	};
	auto peek64 = [](const void* p, int off) -> uint64_t {
		if (!p) return 0;
		return *reinterpret_cast<const uint64_t*>(
		    static_cast<const uint8_t*>(p) + off);
	};

	const uint32_t eMsg = peek32(pJob, 0x10);
	if (eMsg != 0x93 /* k_EMsgServiceMethodResponse */)
	{
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	auto* pInner = reinterpret_cast<uint8_t*>(peek32(pMsg, 0xc));
	auto* pBuf   = reinterpret_cast<uint8_t*>(peek32(pInner, 0x4));
	const uint32_t cbBuf = peek32(pInner, 0x8);
	const uint64_t jobIdTarget = peek64(pJob, 0x8);

	if (!pBuf || cbBuf < sizeof(MsgHdr) + 1)
	{
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	const auto* hdrPtr = reinterpret_cast<const MsgHdr*>(pBuf);
	const uint32_t cbProtoHdr = hdrPtr->headerLength;
	if (cbProtoHdr == 0 || sizeof(MsgHdr) + cbProtoHdr > cbBuf)
	{
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	CMsgProtoBufHeader hdr;
	if (hdr.ParseFromArray(pBuf + sizeof(MsgHdr), cbProtoHdr) && hdr.has_target_job_name())
	{
		g_pLog->debug("BRouteMsgToJob service method response: %s\n", hdr.target_job_name().c_str());
		if (fnvHash(hdr.target_job_name().c_str()) != kHashGetManifestRequestCode)
		{
			return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
		}
	}
	else
	{
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	auto resolved = ManifestFetch::resolve(jobIdTarget);
	if (!resolved)
	{
		g_pLog->debug(
		    "ManifestCode recv: jobid_target=%llu no patch "
		    "(eresult=%d cbBuf=%u)\n",
		    static_cast<unsigned long long>(jobIdTarget),
		    hdr.eresult(), cbBuf);
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	hdr.set_eresult(static_cast<int32_t>(ERESULT_OK));
	hdr.clear_error_message();
	hdr.clear_target_job_name();

	CContentServerDirectory_GetManifestRequestCode_Response resp;
	resp.set_manifest_request_code(*resolved);

	const std::size_t newProtoHdrLen = hdr.ByteSizeLong();
	const std::size_t newBodyLen     = resp.ByteSizeLong();
	const std::size_t newTotalLen    = sizeof(MsgHdr) + newProtoHdrLen + newBodyLen;
	if (newTotalLen > cbBuf)
	{
		g_pLog->warn(
		    "ManifestCode recv: rewrite needs %zu bytes but original buf is %u; pass-through\n",
		    newTotalLen, cbBuf);
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	auto* outHdr = reinterpret_cast<MsgHdr*>(pBuf);
	outHdr->eMsg         = hdrPtr->eMsg;
	outHdr->headerLength = static_cast<uint32_t>(newProtoHdrLen);
	if (!hdr.SerializeToArray(pBuf + sizeof(MsgHdr),
	                          static_cast<int>(newProtoHdrLen))
	    || !resp.SerializeToArray(pBuf + sizeof(MsgHdr) + newProtoHdrLen,
	                              static_cast<int>(newBodyLen)))
	{
		g_pLog->warn("ManifestCode recv: SerializeToArray failed; pass-through\n");
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	*reinterpret_cast<uint32_t*>(pInner + 0x8) = static_cast<uint32_t>(newTotalLen);

	g_pLog->info(
	    "ManifestCode recv: jobid_target=%llu injected code=%llu (cbBuf %u -> %zu in-place)\n",
	    static_cast<unsigned long long>(jobIdTarget),
	    static_cast<unsigned long long>(*resolved),
	    cbBuf, newTotalLen);

	return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
}

bool hkCDepotDownloadMgr_BYldRequestDepotManifest(void* pthis, uint32_t appId, uint32_t depotId, uint64_t manifestId, const char* branch, void* arg20)
{
	g_pLog->info("BYldRequestDepotManifest: app=%u depot=%u manifest=%llu branch=%s\n",
	             appId, depotId, static_cast<unsigned long long>(manifestId), branch ? branch : "");

	const bool inScope =
	    (appId   && g_config.isAddedAppId(appId))
	    || (depotId && g_config.isAddedAppId(depotId))
	    || DepotKey::isManagedDepot(depotId)
	    || !ManifestId::getPinnedGid(depotId).empty();

	if (!inScope)
	{
		g_pLog->debug("BYldRequestDepotManifest: app=%u depot=%u manifest=%llu not in scope, calling original function\n",
		              appId, depotId, static_cast<unsigned long long>(manifestId));
		return Hooks::CDepotDownloadMgr_BYldRequestDepotManifest.tramp.fn(pthis, appId, depotId, manifestId, branch, arg20);
	}

	std::string steamRoot;
	const char* home = std::getenv("HOME");
	if (home)
	{
		static const char* steamRoots[] = {
			"/.steam/steam",
			"/.steam/debian-installation",
			"/.local/share/Steam",
		};
		for (const char* suffix : steamRoots)
		{
			const auto candidate = std::string(home) + suffix;
			if (std::filesystem::exists(candidate + "/steam.sh"))
			{
				steamRoot = candidate;
				break;
			}
		}
	}

	if (!steamRoot.empty())
	{
		const std::string manifestPath = steamRoot + "/depotcache/" + std::to_string(depotId) + "_" + std::to_string(manifestId) + ".manifest";

		if (!std::filesystem::exists(manifestPath) || std::filesystem::file_size(manifestPath) == 0)
		{
			g_pLog->info("BYldRequestDepotManifest: manifest file missing, fetching synchronously: %s\n", manifestPath.c_str());
			// Block until the blob lands on disk (or times out).  We
			// pre-stage the manifest so Steam's downloader finds it
			// locally, then fall through to the original.
			//
			// NOTE (2026-06-04): this hook is a FALLBACK.  The real fix
			// stages the manifest in the PICS recv handler for the gid
			// Steam will actually request (the live public gid — we no
			// longer pin during provisioning, see appinfo_provision.cpp),
			// so for a correctly-provisioned app Steam finds the manifest
			// already on disk during planning and SKIPS this function
			// entirely (the dotAGE-proven path).  This block only runs if
			// that pre-staging missed (e.g. the gid Steam requested
			// differs from what we staged because steamcmd.net's public
			// gid lagged Steam's), in which case we stage the exact gid
			// Steam asked for and fall through to the original's
			// request-code handshake (our BRouteMsgToJob hook answers it).
			const bool ok = ManifestFetch::awaitManifestBlob(manifestId, depotId,
			                                                ManifestFetch::getTimeoutSec());
			if (ok)
			{
				g_pLog->info("BYldRequestDepotManifest: blob staged on disk for depot=%u gid=%llu; "
				             "passing through to drive the request-code handshake\n",
				             depotId, static_cast<unsigned long long>(manifestId));
			}
			else
			{
				g_pLog->info("BYldRequestDepotManifest: blob fetch failed for depot=%u gid=%llu, falling through to Steam's path\n",
				             depotId, static_cast<unsigned long long>(manifestId));
			}
		}
		else
		{
			g_pLog->debug("BYldRequestDepotManifest: manifest already present on disk: %s\n", manifestPath.c_str());
		}
	}
	else
	{
		g_pLog->warn("BYldRequestDepotManifest: could not resolve Steam root\n");
	}

	return Hooks::CDepotDownloadMgr_BYldRequestDepotManifest.tramp.fn(pthis, appId, depotId, manifestId, branch, arg20);
}

} // namespace ManifestCode
