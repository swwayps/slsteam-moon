#include "misc.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../config.hpp"

#include "fakeappid.hpp"

bool Misc::shouldFakeOffline()
{
	const uint32_t appId = FakeAppIds::getRealAppIdForCurrentPipe();
	if (!appId || !g_config.fakeOffline.get().contains(appId))
	{
		return false;
	}

	g_pLog->infoOnce("Faking offline mode for %u\n", appId);
	return true;
}


bool Misc::recvWalletInfo(CMsgClientWalletInfoUpdate* body)
{
	const int32_t amount = g_config.fakeWalletBalance.get();
	if (!amount || !body)
	{
		return false;
	}

	body->set_has_wallet(true);
	body->set_balance(amount);
	body->set_balance64(amount);
	return true;
}

bool Misc::recvEmailInfo(CMsgClientEmailAddrInfo* body)
{
	const auto email = g_config.fakeEmail.get();
	if (email.size() < 1 || !body)
	{
		return false;
	}

	body->set_email_address(email);
	body->set_email_is_validated(true);
	return true;
}

void Misc::recvMsg(CProtoBufMsgBase *msg)
{
	switch(msg->type)
	{
		case EMSG_WALLET_INFO_UPDATE:
			recvWalletInfo(msg->getBody<CMsgClientWalletInfoUpdate>());
			break;

		case EMSG_EMAIL_ADDRESS_INFO:
			recvEmailInfo(msg->getBody<CMsgClientEmailAddrInfo>());
			break;
	}
}
