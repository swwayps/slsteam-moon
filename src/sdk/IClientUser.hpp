#pragma once

#include <cstdint>


class IClientUser
{
public:

	uint32_t getAppOwnershipTicketExtendeData
	(
		uint32_t appId,
		void* pTicket,
		uint32_t ticketSize,
		uint32_t* pOffAppId,
		uint32_t* pOffSteamId,
		uint32_t* pOffSig,
		uint32_t* pSigSize
	);
};

extern IClientUser* g_pClientUser;
