#pragma once

class CMsgClientWalletInfoUpdate;
class CMsgClientEmailAddrInfo;
class CMsgClientPersonaState;

namespace Misc
{
	bool shouldFakeOffline();

	// Typed handlers for the CM-receive dispatch. Each returns true iff it
	// rewrote the message, so the caller knows to reserialize the packet.
	bool recvWalletInfo(CMsgClientWalletInfoUpdate* body);
	bool recvEmailInfo(CMsgClientEmailAddrInfo* body);
	bool recvPersonaState(CMsgClientPersonaState* body);
}
