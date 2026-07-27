#pragma once

#include "../sdk/CSteamID.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class CMsgClientGetAppOwnershipTicketResponse;
class CMsgClientRequestEncryptedAppTicketResponse;
class CProtoBufMsgBase;

namespace Ticket
{
	// The downloader must treat both managed base apps and the DLC ids
	// registered from provisioned appinfo as covered by the eresult stamp.
	// Keep this pure so the ownership decision is testable without protobuf
	// arena objects or Steam runtime state.
	inline bool shouldStampAppOwnershipTicket(bool isAddedApp,
	                                           bool isAddedAppDlc) noexcept
	{
		return isAddedApp || isAddedAppDlc;
	}

	class SavedTicket
	{
public:
		CSteamId steamId;
		std::string ticket;

		constexpr bool isValid() const
		{
			return steamId.isSet() && ticket.size() > 0;
		}
	};

	extern CSteamId oneTimeSteamIdSpoof;
	extern std::unordered_map<AppId_t, SavedTicket> ticketMap;
	extern std::unordered_map<AppId_t, SavedTicket> encryptedTicketMap;
	inline std::mutex cacheMutex;
	inline std::unordered_set<uint32_t> invalidatedApps;

	// Invalidate both in-memory ticket caches when an app disappears from the
	// managed set. Disk artifacts are quarantined by AppInfoProvision, but the
	// process-local maps must be cleared immediately so a removed app cannot
	// reuse a ticket during the same Steam session. The tombstone is held under
	// the same mutex as disk loads/saves so an in-flight transaction cannot
	// repopulate either cache after removal.
	inline bool forgetApp(uint32_t appId)
	{
		if (appId == 0) return false;
		std::lock_guard<std::mutex> lock(cacheMutex);
		invalidatedApps.insert(appId);
		ticketMap.erase(appId);
		encryptedTicketMap.erase(appId);
		return true;
	}

	inline bool isAppInvalidated(uint32_t appId)
	{
		if (appId == 0) return false;
		std::lock_guard<std::mutex> lock(cacheMutex);
		return invalidatedApps.contains(appId);
	}

	// Clear the removal tombstone when a script is added again. This is
	// idempotent so callers can safely invoke it for every newly discovered app.
	inline bool restoreApp(uint32_t appId)
	{
		if (appId == 0) return false;
		std::lock_guard<std::mutex> lock(cacheMutex);
		invalidatedApps.erase(appId);
		return true;
	}

	std::string getTicketDir();

	//TODO: Fill with error checks
	std::string getTicketPath(uint32_t appId);
	SavedTicket getCachedTicket(uint32_t appId);
	bool saveTicketToCache(CMsgClientGetAppOwnershipTicketResponse* resp);

	void launchApp(uint32_t appId);
	void getTicketOwnershipExtendedData(uint32_t appId);

	std::string getEncryptedTicketPath(uint32_t appId);
	SavedTicket getCachedEncryptedTicket(uint32_t appId);
	bool saveEncryptedTicketToCache(CMsgClientRequestEncryptedAppTicketResponse* resp);

	void recvEncryptedAppTicket(CMsgClientRequestEncryptedAppTicketResponse* msg);
	void recvAppTicket(CMsgClientGetAppOwnershipTicketResponse* msg);
	void recvMsg(CProtoBufMsgBase* msg);
}
