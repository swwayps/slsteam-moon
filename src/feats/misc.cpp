#include "misc.hpp"

#include "../sdk/CProtoBufMsgBase.hpp"
#include "../config.hpp"
#include "../globals.hpp"

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

bool Misc::recvPersonaState(CMsgClientPersonaState* body)
{
	const auto name = g_config.fakeName.get();
	if (name.empty() || !body)
	{
		return false;
	}

	for (int i = 0; i < body->friends_size(); i++)
	{
		auto* person = body->mutable_friends(i);
		if (person->friendid() != g_currentSteamId.steamId64)
		{
			continue;
		}

		person->set_player_name(name);
		g_pLog->debug("Faked self persona\n");
		return true;
	}
	return false;
}
