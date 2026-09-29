#include "IClientUser.hpp"

#include "../hooks.hpp"


uint32_t IClientUser::getAppOwnershipTicketExtendeData
(
	uint32_t appId,
	void* pTicket,
	uint32_t ticketSize,
	uint32_t* pOffAppId,
	uint32_t* pOffSteamId,
	uint32_t* pOffSig,
	uint32_t* pSigSize
)
{
	return Hooks::IClientUser_GetAppOwnershipTicketExtendedData.tramp.fn(this, appId, pTicket, ticketSize, pOffAppId, pOffSteamId, pOffSig, pSigSize);
}


IClientUser* g_pClientUser = nullptr;
