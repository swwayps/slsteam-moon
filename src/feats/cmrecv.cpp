// SPDX-License-Identifier: AGPL-3.0-only

#include "cmrecv.hpp"

#include "achievements.hpp"
#include "depotkey.hpp"
#include "manifestcode.hpp"
#include "manifestdonor.hpp"
#include "misc.hpp"
#include "pics.hpp"
#include "ticket.hpp"

#include "../config.hpp"
#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/protobufs/steammessages_base.pb.h"
#include "../sdk/protobufs/steammessages_clientserver.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_2.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_appinfo.pb.h"

#include <exception>


namespace
{
	// Server pushes not present in the SLSsteam EMsg enum (Achievements
	// login-state + donor reset).
	constexpr uint32_t kEMsgClientLogOnResponse = 751;
	constexpr uint32_t kEMsgClientLoggedOff     = 757;
	constexpr uint32_t kEMsgClientLicenseList   = 780;

void dispatch(CNetPacket* pkt, uint32_t eMsg)
{

	// Login-state invalidation for 751/757/780 (Achievements first, then
	// per-type handling).
	if (eMsg == kEMsgClientLogOnResponse ||
	    eMsg == kEMsgClientLoggedOff ||
	    eMsg == kEMsgClientLicenseList)
	{
		CMsgProtoBufHeader header;
		const bool haveHeader = pkt->deserializeHeader(header);
		Achievements::recvMessage(
			eMsg,
			haveHeader && header.has_steamid(),
			(haveHeader && header.has_steamid()) ? header.steamid() : 0);
	}

	switch (eMsg)
	{
		case kEMsgClientLicenseList:
		{
			auto body = pkt->deserializeBody<CMsgClientLicenseList>();
			ManifestDonor::onLicenseList(&body);
			break;
		}

		case kEMsgClientLoggedOff:
		{
			ManifestDonor::onLoggedOff();
			ManifestCode::resetSession();
			break;
		}

		// A logon answer means a new connection: drop the send context the
		// donor captured on the previous one.
		case kEMsgClientLogOnResponse:
			ManifestCode::resetDonorContext();
			break;

		case k_EMsgClientGetDepotDecryptionKeyResponse:
		{
			auto body = pkt->deserializeBody<CMsgClientGetDepotDecryptionKeyResponse>();
			if (DepotKey::recvDepotKey(&body))
				pkt->serialize(body);
			break;
		}

		// Replies to rewritten achievements requests: keep the schema, drop the
		// owner's progress, or fail closed.
		case k_EMsgServiceMethodResponse:
		case k_EMsgClientGetUserStatsResponse:
			Achievements::recvReply(pkt);
			break;
		case k_EMsgClientGetAppOwnershipTicketResponse:
		{
			auto body = pkt->deserializeBody<CMsgClientGetAppOwnershipTicketResponse>();
			if (Ticket::recvAppTicket(&body))
				pkt->serialize(body);
			break;
		}

		case k_EMsgClientRequestEncryptedAppTicketResponse:
		{
			auto body = pkt->deserializeBody<CMsgClientRequestEncryptedAppTicketResponse>();
			if (Ticket::recvEncryptedAppTicket(&body))
				pkt->serialize(body);
			break;
		}

		case k_EMsgClientWalletInfoUpdate:
		{
			auto body = pkt->deserializeBody<CMsgClientWalletInfoUpdate>();
			if (Misc::recvWalletInfo(&body))
				pkt->serialize(body);
			break;
		}

		case k_EMsgClientEmailAddrInfo:
		{
			auto body = pkt->deserializeBody<CMsgClientEmailAddrInfo>();
			if (Misc::recvEmailInfo(&body))
				pkt->serialize(body);
			break;
		}
		case k_EMsgClientPersonaState:
		{
			// Only deserialized when FakeName is set: persona updates are frequent.
			if (g_config.fakeName.get().empty())
				break;
			auto body = pkt->deserializeBody<CMsgClientPersonaState>();
			if (Misc::recvPersonaState(&body))
				pkt->serialize(body);
			break;
		}

		case k_EMsgClientPICSProductInfoResponse:
		{
			// Observe-only: the handler reads the response (cache repair,
			// background refresh scheduling) and never rewrites it for Steam.
			auto body = pkt->deserializeBody<CMsgClientPICSProductInfoResponse>();
			PICS::recvProductInfoResponse(&body);
			break;
		}

		case k_EMsgClientPICSChangesSinceResponse:
		{
			auto body = pkt->deserializeBody<CMsgClientPICSChangesSinceResponse>();
			const int before = body.app_changes_size();
			PICS::recvChangesSinceResponse(&body);
			// Only reserialize when the changelist was actually filtered
			// (locally authoritative apps stripped) so an untouched response
			// never churns the Steam-owned packet body.
			if (body.app_changes_size() != before)
				pkt->serialize(body);
			break;
		}

		default:
			break;
	}
}

} // namespace

// Runs inside Steam's CM receive hook, so nothing may unwind into Steam: a
// handler that throws is logged and Steam gets the packet as it is.
void CmRecv::dispatchNetPacket(CNetPacket* pkt)
{
	if (!pkt) return;
	const uint32_t eMsg = pkt->getProtoBufType();
	try
	{
		dispatch(pkt, eMsg);
	}
	catch (const std::exception& e)
	{
		if (g_pLog)
			g_pLog->infoOnce("CmRecv: handler for message %u failed: %s\n", eMsg, e.what());
	}
	catch (...)
	{
		if (g_pLog)
			g_pLog->infoOnce("CmRecv: handler for message %u failed\n", eMsg);
	}
}
