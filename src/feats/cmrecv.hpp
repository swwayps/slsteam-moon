// SPDX-License-Identifier: AGPL-3.0-only
//
// CM-receive dispatch for the newer Steam client.
//
// Background: older clients deserialize every incoming CM protobuf message
// into a CProtoBufMsgBase and run CProtoBufMsgBase::InitFromPacket, which is
// where SLSsteam's message features (DepotKey, PICS, Ticket, Misc,
// ManifestDonor license/logoff, Achievements login-state) are wired. A newer
// client dispatches the same messages straight from CCMInterface::RecvPkt as
// CNetPacket protobuf frames and never constructs the CProtoBufMsgBase, so the
// classic hook only ever sees a heartbeat type and every one of those features
// silently stops firing (confirmed on the guest: depot-key responses, app
// ownership tickets, PICS product-info/changes, license list, wallet/email all
// arrive as RecvPkt proto frames while InitFromPacket sees only type 7).
//
// This module routes those CNetPacket frames through the SAME feature handlers,
// rewriting the packet body in place for the handlers that mutate the response
// (depot-key substitution, ownership-ticket eresult stamp, cached encrypted
// ticket, PICS changelist filter, wallet/email spoof).
//
// A single runtime latch picks whichever recv transport actually carries CM
// messages on this client, so the classic path and this one never both handle
// the same message.  RecvPkt runs before InitFromPacket for a given packet, so
// on every client the CNetPacket path claims first and the classic path stays
// dormant as a fallback for a hypothetical client that delivers CM messages via
// InitFromPacket without our RecvPkt hook seeing them first.

#pragma once

#include <cstdint>

class CNetPacket;

namespace CmRecv
{
	enum class Transport
	{
		Unknown,
		Classic, // CProtoBufMsgBase::InitFromPacket (older clients)
		NetPkt,  // CCMInterface::RecvPkt CNetPacket frames (newer clients)
	};

	// True for the CM message eMsgs both recv paths handle. Untracked types
	// are left entirely to Steam and never latch the transport.
	bool isTracked(uint32_t eMsg);

	// Latch the live transport on the first tracked message and report whether
	// `who` owns dispatch. Idempotent after the first call.
	bool claim(Transport who);

	// Dispatch a validated protobuf CNetPacket through the feature handlers,
	// reserializing the packet for the mutating handlers. Call only for a
	// tracked type this transport owns (see claim), after the Family-Share
	// choke, and before the original RecvPkt runs.
	void dispatchNetPacket(CNetPacket* pkt);
}
