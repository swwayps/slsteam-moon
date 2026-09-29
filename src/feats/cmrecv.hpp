// SPDX-License-Identifier: AGPL-3.0-only
//
// CM-receive dispatch.
//
// The client delivers incoming CM protobuf messages from CCMInterface::RecvPkt
// as CNetPacket frames (it no longer builds a CProtoBufMsgBase for them), so
// SLSsteam's message features (DepotKey, PICS, Ticket, Misc, ManifestDonor
// license/logoff, Achievements login-state) are all wired here.
//
// Handlers that mutate a response (depot-key substitution, ownership-ticket
// eresult stamp, cached encrypted ticket, PICS changelist filter, wallet/email
// spoof) rewrite the packet body in place before Steam processes it.

#pragma once

class CNetPacket;

namespace CmRecv
{
	// Dispatch a validated protobuf CNetPacket through the feature handlers,
	// reserializing the packet for the mutating handlers. Call after the
	// Family-Share choke and before the original RecvPkt runs.
	void dispatchNetPacket(CNetPacket* pkt);
}
