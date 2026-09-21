#pragma once

class CProtoBufMsgBase;
class CMsgClientWalletInfoUpdate;
class CMsgClientEmailAddrInfo;

namespace Misc
{
	bool shouldFakeOffline();
	void recvMsg(CProtoBufMsgBase* msg);

	// Typed handlers shared by the classic CProtoBufMsgBase path and the
	// CNetPacket CM-receive path. Each returns true iff it rewrote the
	// message (so the CNetPacket caller knows to reserialize the packet).
	bool recvWalletInfo(CMsgClientWalletInfoUpdate* body);
	bool recvEmailInfo(CMsgClientEmailAddrInfo* body);
}
