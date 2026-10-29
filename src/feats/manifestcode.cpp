
#include "manifestcode.hpp"

#include "depotkey.hpp"
#include "manifeststore.hpp"
#include "manifestdonor.hpp"
#include "achievements.hpp"
#include "apps.hpp"
#include "fakeappid.hpp"
#include "playerstats.hpp"

#include "../config.hpp"
#include "../hooks.hpp"
#include "../log.hpp"
#include "../lua.hpp"

#include "../sdk/steam.hpp"
#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/protobufs/steammessages_base.pb.h"
#include "../sdk/protobufs/steammessages_contentserverdirectory.pb.h"

#include "../utils/ManifestFetch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


namespace ManifestCode
{
namespace
{

constexpr uint32_t kMaxBodySize    = 262144;
constexpr uint32_t kMaxHdrSize     = 1024;
static_assert(kMaxBodySize >= Achievements::maxReplyBytes);
static_assert(kMaxHdrSize >= Achievements::maxHeaderBytes);
constexpr uint32_t kMaxPacketSize  = sizeof(MsgHdr) + kMaxHdrSize + kMaxBodySize;


constexpr uint64_t kDonorJobBase = 0x7e51000000000000ULL;
struct DonorPending { std::promise<uint64_t> result; };
struct PassiveRequest
{
	uint32_t depotId;
	uint64_t gid;
	uint64_t generation;
	std::chrono::steady_clock::time_point sentAt;
};
std::mutex g_DonorLock;
std::condition_variable g_DonorSendCv;
std::size_t g_DonorActiveSends = 0;
bool g_DonorResetting = false;
bool g_DonorDeferredReset = false;
thread_local unsigned int t_DonorSendDepth = 0;
uint64_t g_DonorContextGeneration = 1;
void* g_DonorConnection = nullptr;
std::vector<uint8_t> g_DonorHeader;
uint64_t g_NextDonorJob = kDonorJobBase;
std::unordered_map<uint64_t, DonorPending> g_DonorPending;
std::unordered_map<uint64_t, PassiveRequest> g_PassiveRequests;

void clearDonorSessionLocked()
{
	for (auto& [job, pending] : g_DonorPending)
		pending.result.set_value(0);
	g_DonorPending.clear();
	g_PassiveRequests.clear();
	g_DonorConnection = nullptr;
	g_DonorHeader.clear();
}

void prunePassiveRequestsLocked()
{
	const auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(1);
	for (auto it = g_PassiveRequests.begin(); it != g_PassiveRequests.end();)
	{
		if (it->second.sentAt < cutoff)
			it = g_PassiveRequests.erase(it);
		else
			++it;
	}
}

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
	if (cbHdr > size - sizeof(MsgHdr)) return false;
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

// Assemble a replacement outgoing frame (MsgHdr + header + new body) for the
// send hook to pass on instead of Steam's. `rawEMsg` must keep the proto flag.
inline void buildReplacementFrame(std::vector<uint8_t>& frame, uint32_t rawEMsg,
                                  const uint8_t* pHdr, uint32_t cbHdr,
                                  const uint8_t* pNewBody, uint32_t cbNewBody)
{
	const uint32_t newSize = sizeof(MsgHdr) + cbHdr + cbNewBody;
	if (newSize > kMaxPacketSize)
	{
		return;
	}
	frame.resize(newSize);
	auto* out = reinterpret_cast<MsgHdr*>(frame.data());
	out->eMsg         = rawEMsg;
	out->headerLength = cbHdr;
	std::memcpy(frame.data() + sizeof(MsgHdr), pHdr, cbHdr);
	if (cbNewBody)
	{
		std::memcpy(frame.data() + sizeof(MsgHdr) + cbHdr, pNewBody, cbNewBody);
	}
}

// Outgoing Player.GetUserStats#1 (eMsg 151): the modern library page fetches
// a game's achievement schema through this unified method. For an
// AdditionalApp the account doesn't own server-side, the request returns
// empty and the Achievements tab never appears. Rewrite the steamid to a
// real owner of the game so the server returns a populated schema.
void handleSend_PlayerGetUserStats(const uint8_t* pBody, uint32_t cbBody,
                                   const uint8_t* pHdr, uint32_t cbHdr,
                                   uint32_t eMsg, std::vector<uint8_t>& replacement)
{
	if (cbHdr > kMaxHdrSize) return;
	CMsgProtoBufHeader hdr;
	if (!hdr.ParseFromArray(pHdr, cbHdr)) return;
	const auto newBody = Achievements::rewriteRequest(true, pBody, cbBody, hdr);
	if (newBody.empty()) return;
	buildReplacementFrame(replacement, eMsg | kMsgHdrProtoFlag, pHdr, cbHdr,
	                      newBody.data(), static_cast<uint32_t>(newBody.size()));
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
	ManifestDonor::observeDepot(appId, depotId, gid);
	if (g_config.donate.get().enabled)
	{
		CMsgProtoBufHeader capturedHeader;
		if (capturedHeader.ParseFromArray(pHdr, cbHdr) &&
		    capturedHeader.has_jobid_source())
		{
			std::lock_guard lock(g_DonorLock);
			prunePassiveRequestsLocked();
			if (!g_DonorResetting && g_PassiveRequests.size() < 4096)
				g_PassiveRequests[capturedHeader.jobid_source()] = {
					depotId, gid, ManifestDonor::sessionGeneration(),
					std::chrono::steady_clock::now()
				};
		}
	}

	const bool inScope = DepotKey::manifestInManagedScope(
	    appId, depotId, g_config.getManifestPinForPlanner(appId, depotId) != 0);
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

	// Stage the manifest from the archive so Steam finds it on disk and skips
	// the request-code handshake entirely. We no longer answer that handshake
	// ourselves (the archive is the manifest source).
	ManifestFetch::submitManifestBlob(
		gid, appId, depotId,
		ManifestFetch::isAnyManagedDownloadActive(appId));
}

// Fills `replacement` when the frame has to go out rewritten.
void dispatchSend(uint32_t eMsg,
                  const uint8_t* pBody, uint32_t cbBody,
                  const uint8_t* pHdr,  uint32_t cbHdr,
                  std::vector<uint8_t>& replacement)
{
	if (eMsg == k_EMsgClientGetUserStats)
	{
		if (cbHdr > kMaxHdrSize) return;
		CMsgProtoBufHeader hdr;
		if (!hdr.ParseFromArray(pHdr, cbHdr)) return;
		const auto body = Achievements::rewriteRequest(false, pBody, cbBody, hdr);
		if (!body.empty()) buildReplacementFrame(replacement, eMsg | kMsgHdrProtoFlag,
			pHdr, cbHdr, body.data(), static_cast<uint32_t>(body.size()));
		return;
	}
	if (eMsg != k_EMsgServiceMethodCallFromClient)
	{
		return;
	}
	CMsgProtoBufHeader hdr;
	if (!hdr.ParseFromArray(pHdr, cbHdr) || !hdr.has_target_job_name())
	{
		return;
	}
	if (g_config.extendedLogging.get())
		g_pLog->debug("WebSocket service method send: %s\n", hdr.target_job_name().c_str());
	const auto h = fnvHash(hdr.target_job_name().c_str());
	switch (h)
	{
	case kHashGetManifestRequestCode:
		handleSend_GetManifestRequestCode(pBody, cbBody, pHdr, cbHdr);
		return;
	case kHashPlayerGetUserStats:
		handleSend_PlayerGetUserStats(pBody, cbBody, pHdr, cbHdr, eMsg, replacement);
		return;
	default:
		return;
	}
}

bool resolveDonorResponse(uint64_t job, const CMsgProtoBufHeader& header,
                          const uint8_t* pBody, uint32_t cbBody)
{
	std::promise<uint64_t> pending;
	bool originated = false, passive = false;
	PassiveRequest sent{};
	{
		std::lock_guard lock(g_DonorLock);
		if (auto it = g_DonorPending.find(job); it != g_DonorPending.end())
		{
			pending = std::move(it->second.result);
			g_DonorPending.erase(it);
			originated = true;
		}
		if (auto it = g_PassiveRequests.find(job); it != g_PassiveRequests.end())
		{
			sent = it->second;
			g_PassiveRequests.erase(it);
			passive = true;
		}
	}
	if (!originated && !passive) return false;

	uint64_t code = 0;
	if (header.has_eresult() && header.eresult() == k_EResultOK)
	{
		CContentServerDirectory_GetManifestRequestCode_Response response;
		if (response.ParseFromArray(pBody, cbBody) &&
		    response.has_manifest_request_code())
			code = response.manifest_request_code();
	}
	if (originated)
	{
		pending.set_value(code);
		g_pLog->info("Donor: Steam response job=%llu result=%d code=%s\n",
		             static_cast<unsigned long long>(job), header.eresult(),
		             code ? "present" : "absent");
	}
	if (passive && code)
		ManifestDonor::submitCapturedCode(
			sent.depotId, sent.gid, code, sent.generation);
	return true;
}

} // namespace

CodeRequest requestCode(uint32_t appId, uint32_t depotId, uint64_t gid)
{
	CodeRequest request;
	std::promise<uint64_t> promise;
	request.result = promise.get_future();
	auto fail = [&]() -> CodeRequest
	{
		promise.set_value(0);
		return std::move(request);
	};
	if (!depotId || !gid) return fail();

	void* connection = nullptr;
	decltype(Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn)
		sendFrame = nullptr;
	std::vector<uint8_t> frame;
	uint64_t job = 0;
	{
		std::lock_guard lock(g_DonorLock);
		if (g_DonorResetting || !g_DonorConnection || g_DonorHeader.empty() ||
		    !Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn ||
		    g_DonorPending.size() >= 128)
		{
			g_pLog->info("Donor: Steam WebSocket send context unavailable\n");
			return fail();
		}
		CMsgProtoBufHeader header;
		if (!header.ParseFromArray(g_DonorHeader.data(), g_DonorHeader.size()))
			return fail();
		request.jobId = ++g_NextDonorJob;
		header.set_target_job_name(
			"ContentServerDirectory.GetManifestRequestCode#1");
		header.set_jobid_source(request.jobId);
		header.clear_jobid_target();
		CContentServerDirectory_GetManifestRequestCode_Request body;
		body.set_app_id(appId);
		body.set_depot_id(depotId);
		body.set_manifest_id(gid);
		const auto headerSize = header.ByteSizeLong();
		const auto bodySize = body.ByteSizeLong();
		if (headerSize > kMaxHdrSize || bodySize > kMaxBodySize) return fail();
		frame.resize(sizeof(MsgHdr) + headerSize + bodySize);
		auto* prefix = reinterpret_cast<MsgHdr*>(frame.data());
		prefix->eMsg = k_EMsgServiceMethodCallFromClient | kMsgHdrProtoFlag;
		prefix->headerLength = static_cast<uint32_t>(headerSize);
		if (!header.SerializeToArray(frame.data() + sizeof(MsgHdr), headerSize) ||
		    !body.SerializeToArray(
			    frame.data() + sizeof(MsgHdr) + headerSize, bodySize))
			return fail();
		job = request.jobId;
		connection = g_DonorConnection;
		sendFrame = Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn;
		g_DonorPending.emplace(job, DonorPending{std::move(promise)});
		++g_DonorActiveSends;
	}

	// The trampoline may synchronously drive Steam networking. Do not hold the
	// donor state mutex across that foreign call; its response path takes the
	// same mutex to complete the promise.
	++t_DonorSendDepth;
	const bool accepted = sendFrame(
		connection, k_eWebSocketOpCode_Binary,
		frame.data(), static_cast<uint32_t>(frame.size()));
	--t_DonorSendDepth;
	{
		std::lock_guard lock(g_DonorLock);
		if (g_DonorActiveSends) --g_DonorActiveSends;
		if (g_DonorActiveSends == 0 && g_DonorDeferredReset)
		{
			clearDonorSessionLocked();
			g_DonorDeferredReset = false;
			g_DonorResetting = false;
		}
	}
	g_DonorSendCv.notify_all();
	if (!accepted)
	{
		g_pLog->info("Donor: Steam WebSocket send rejected request\n");
		std::lock_guard lock(g_DonorLock);
		if (const auto it = g_DonorPending.find(job); it != g_DonorPending.end())
		{
			it->second.result.set_value(0);
			g_DonorPending.erase(it);
		}
	}
	else
	{
		g_pLog->debug("Donor: Steam WebSocket accepted job=%llu (%zu bytes)\n",
		             static_cast<unsigned long long>(job), frame.size());
	}
	return request;
}

void discardRequest(uint64_t jobId)
{
	if (!jobId) return;
	std::lock_guard lock(g_DonorLock);
	const auto it = g_DonorPending.find(jobId);
	if (it == g_DonorPending.end()) return;
	it->second.result.set_value(0);
	g_DonorPending.erase(it);
}

void resetDonorContext()
{
	{
		std::unique_lock lock(g_DonorLock);
		if (g_DonorResetting)
		{
			if (t_DonorSendDepth != 0) return;
			g_DonorSendCv.wait(lock, [] { return !g_DonorResetting; });
		}
		g_DonorResetting = true;
		++g_DonorContextGeneration;
		clearDonorSessionLocked();
		if (t_DonorSendDepth != 0)
		{
			g_DonorDeferredReset = true;
			return;
		}
		g_DonorSendCv.wait(lock, [] { return g_DonorActiveSends == 0; });
		clearDonorSessionLocked();
		g_DonorResetting = false;
	}
	g_DonorSendCv.notify_all();
}

void resetSession()
{
	resetDonorContext();
	ManifestFetch::resetSessionState();
}



bool hkBBuildAndAsyncSendFrame(void* pConnection,
                               EWebSocketOpCode eOpCode,
                               uint8_t* pubData,
                               uint32_t cubData)
{
	// The scratch packet below is read through the packet field layout, which
	// the first received packet can still change; until then frames go out
	// as they came (nothing rewritten here is sent before the first receive).
	if (eOpCode == k_eWebSocketOpCode_Binary && CNetPacket::layoutSettled())
	{
		CNetPacket packet{};
		packet.setBody(reinterpret_cast<CNetPacketBody*>(Steam::Plat_Alloc(cubData)));
		if (packet.body())
		{
			std::memcpy(packet.body(), pubData, cubData);
			packet.setSize(cubData);
			if (packet.isValid() && packet.isProtoBuf())
			{
				Lua::fireCallback(Lua::Callbacks::Network_SendPkt, &packet);
				Apps::sendMsg(&packet);
				FakeAppIds::sendMsg(&packet);
				DepotKey::sendMsg(&packet);
			}
			pubData = reinterpret_cast<uint8_t*>(packet.body());
			cubData = packet.sizeBytes();

			uint32_t eMsg = 0;
			const uint8_t* pHdr  = nullptr;
			const uint8_t* pBody = nullptr;
			uint32_t cbHdr = 0, cbBody = 0;
			if (decodeFrame(pubData, cubData, eMsg, pHdr, cbHdr, pBody, cbBody))
			{
				uint64_t contextGeneration = 0;
				{
					std::lock_guard donorLock(g_DonorLock);
					if (!g_DonorResetting)
						contextGeneration = g_DonorContextGeneration;
				}
				if (eMsg == k_EMsgServiceMethodCallFromClient &&
				    cbHdr && cbHdr <= kMaxHdrSize)
				{
					CMsgProtoBufHeader header;
					if (header.ParseFromArray(pHdr, cbHdr) &&
					    header.has_target_job_name() &&
					    header.has_steamid() && header.steamid() &&
					    header.has_client_sessionid() && header.client_sessionid())
					{
						// Never block Steam's send thread: requestCode() copies
						// the context under this lock before it sends, so a
						// newer capture cannot disturb a send in flight. A
						// re-entrant send from the donor's own call is skipped.
						std::lock_guard donorLock(g_DonorLock);
						if (t_DonorSendDepth == 0 &&
						    pConnection && contextGeneration && !g_DonorResetting &&
						    contextGeneration == g_DonorContextGeneration)
						{
							const bool first = g_DonorHeader.empty();
							g_DonorConnection = pConnection;
							g_DonorHeader.assign(pHdr, pHdr + cbHdr);
							if (first)
								g_pLog->infoOnce(
								    "Donor: authenticated Steam send context captured\n");
						}
					}
				}
				std::vector<uint8_t> replacement;
				dispatchSend(eMsg, pBody, cbBody, pHdr, cbHdr, replacement);
				if (!replacement.empty())
				{
					const bool success =
						Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn(
							pConnection, eOpCode, replacement.data(),
							static_cast<uint32_t>(replacement.size()));
					packet.free();
					return success;
				}
			}

			const bool success =
				Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn(
					pConnection, eOpCode, pubData, cubData);
			packet.free();
			return success;
		}
	}
	return Hooks::CWebSocketConnection_BBuildAndAsyncSendFrame.tramp.fn(
	    pConnection, eOpCode, pubData, cubData);
}

bool hkBRouteMsgToJob(void* pJobMgr, void* arg2, void* pMsg, void* pJob)
{
	// Reject not just null but any implausible pointer before dereferencing.
	// A Steam client ABI change can shift the message/job structs these offsets
	// walk (a beta did), so a chained read (e.g. pInner = peek32(pMsg,0xc)) can
	// yield a 0xFFFFFFFF sentinel instead of a real inner pointer; dereferencing
	// it segfaults the whole client on the job-routing path.  Treat an
	// implausible pointer as "no data" so this hook falls through to the
	// original routing unchanged.
	auto plausible = [](const void* p) -> bool {
		const auto a = reinterpret_cast<uintptr_t>(p);
		return a >= 0x10000u && a < 0xFFFFF000u;
	};
	auto peek32 = [&](const void* p, int off) -> uint32_t {
		if (!plausible(p)) return 0;
		return *reinterpret_cast<const uint32_t*>(
		    static_cast<const uint8_t*>(p) + off);
	};
	auto peek64 = [&](const void* p, int off) -> uint64_t {
		if (!plausible(p)) return 0;
		return *reinterpret_cast<const uint64_t*>(
		    static_cast<const uint8_t*>(p) + off);
	};

	const uint32_t eMsg = peek32(pJob, 0x10);
	if (eMsg != k_EMsgServiceMethodResponse)
	{
		return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
	}

	auto* pInner = reinterpret_cast<uint8_t*>(peek32(pMsg, 0xc));
	auto* pBuf   = reinterpret_cast<uint8_t*>(peek32(pInner, 0x4));
	const uint32_t cbBuf = peek32(pInner, 0x8);
	const uint64_t jobIdTarget = peek64(pJob, 0x8);

	// pBuf is a value read out of the (client-owned) inner struct, then used as
	// a MsgHdr*.  On an ABI-shifted client that inner read yields a 0xFFFFFFFF
	// sentinel, which is non-null but derefs into unmapped memory (MsgHdr access
	// below segfaults the client).  Require a plausible pointer and a sane
	// buffer length so this whole interception falls through to the original
	// routing on a layout we no longer parse, instead of crashing.
	if (!plausible(pBuf) || cbBuf < sizeof(MsgHdr) + 1 || cbBuf > (64u * 1024 * 1024))
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
		if (g_config.extendedLogging.get())
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

	const uint32_t bodyOffset = sizeof(MsgHdr) + cbProtoHdr;
	// Feed the donor's own outstanding request-code sends (owned depots) so it
	// can share the minted codes. We no longer inject a code into Steam's
	// response — manifests are staged from the archive — so the message routes
	// through unchanged.
	resolveDonorResponse(jobIdTarget, hdr, pBuf + bodyOffset, cbBuf - bodyOffset);
	return Hooks::CJobMgr_BRouteMsgToJob.tramp.fn(pJobMgr, arg2, pMsg, pJob);
}

bool hkCDepotDownloadMgr_BYldRequestDepotManifest(void* pthis, uint32_t appId, uint32_t depotId, uint64_t manifestId, const char* branch, void* arg20)
{
	const bool inScope = DepotKey::manifestInManagedScope(
	    appId, depotId, g_config.getManifestPinForPlanner(appId, depotId) != 0);
	g_pLog->debug("BYldRequestDepotManifest: app=%u depot=%u manifest=%llu branch=%s\n",
	              appId, depotId, static_cast<unsigned long long>(manifestId), branch ? branch : "");

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
			std::error_code rootEc;
			if (std::filesystem::exists(candidate + "/steam.sh", rootEc))
			{
				steamRoot = candidate;
				break;
			}
		}
	}

	if (!steamRoot.empty())
	{
		const std::string manifestPath = steamRoot + "/depotcache/" + std::to_string(depotId) + "_" + std::to_string(manifestId) + ".manifest";

		// A missing file reports an error here, which counts as absent.
		std::error_code sizeEc;
		const auto manifestSize = std::filesystem::file_size(manifestPath, sizeEc);
		if (sizeEc || manifestSize == 0)
		{
			g_pLog->info("BYldRequestDepotManifest: manifest file missing, fetching synchronously: %s\n", manifestPath.c_str());
			// Fallback for a gid the install plan did not stage: wait for the
			// archive copy, then let Steam's own request run.
			const bool ok = ManifestFetch::awaitManifestBlob(
				manifestId, appId, depotId, ManifestFetch::getTimeoutSec(),
				ManifestFetch::isAnyManagedDownloadActive(appId));
			if (ok)
			{
				ManifestStore::archiveManifest(depotId, manifestId);
				if (g_config.getManifestPin(appId, depotId) != manifestId)
				{
					ManifestStore::markPreferredGid(depotId, manifestId);
				}
				g_pLog->info("BYldRequestDepotManifest: blob staged for depot=%u gid=%llu; "
				             "calling Steam's request\n",
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
