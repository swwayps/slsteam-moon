// SPDX-License-Identifier: AGPL-3.0-only

#include "cmrecv.hpp"

#include "achievements.hpp"
#include "depotkey.hpp"
#include "manifestcode.hpp"
#include "manifestdonor.hpp"
#include "misc.hpp"
#include "pics.hpp"
#include "ticket.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/protobufs/steammessages_base.pb.h"
#include "../sdk/protobufs/steammessages_clientserver.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_2.pb.h"
#include "../sdk/protobufs/steammessages_clientserver_appinfo.pb.h"


namespace
{
	// Server pushes not present in the SLSsteam EMsg enum (Achievements
	// login-state + donor reset).
	constexpr uint32_t kEMsgClientLogOnResponse = 751;
	constexpr uint32_t kEMsgClientLoggedOff     = 757;
	constexpr uint32_t kEMsgClientLicenseList   = 780;
}

void CmRecv::dispatchNetPacket(CNetPacket* pkt)
{
	if (!pkt) return;
	const uint32_t eMsg = pkt->getProtoBufType();

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

		case EMSG_GET_DEPOT_DECRYPTION_KEY_RESPONSE:
		{
			auto body = pkt->deserializeBody<CMsgClientGetDepotDecryptionKeyResponse>();
			if (DepotKey::recvDepotKey(&body))
				pkt->serialize(body);
			break;
		}

		case EMSG_APPOWNERSHIPTICKET_RESPONSE:
		{
			auto body = pkt->deserializeBody<CMsgClientGetAppOwnershipTicketResponse>();
			if (Ticket::recvAppTicket(&body))
				pkt->serialize(body);
			break;
		}

		case EMSG_ENCRYPTED_APPTICKET_RESPONSE:
		{
			auto body = pkt->deserializeBody<CMsgClientRequestEncryptedAppTicketResponse>();
			if (Ticket::recvEncryptedAppTicket(&body))
				pkt->serialize(body);
			break;
		}

		case EMSG_WALLET_INFO_UPDATE:
		{
			auto body = pkt->deserializeBody<CMsgClientWalletInfoUpdate>();
			if (Misc::recvWalletInfo(&body))
				pkt->serialize(body);
			break;
		}

		case EMSG_EMAIL_ADDRESS_INFO:
		{
			auto body = pkt->deserializeBody<CMsgClientEmailAddrInfo>();
			if (Misc::recvEmailInfo(&body))
				pkt->serialize(body);
			break;
		}

		case EMSG_PICS_PRODUCTINFO_RESPONSE:
		{
			// Observe-only: the handler reads the response (cache repair,
			// background refresh scheduling) and never rewrites it for Steam.
			auto body = pkt->deserializeBody<CMsgClientPICSProductInfoResponse>();
			PICS::recvProductInfoResponse(&body);
			break;
		}

		case EMSG_PICS_CHANGES_RESPONSE:
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
