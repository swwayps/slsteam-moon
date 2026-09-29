#include "IClientUser.hpp"

#include "../hooks.hpp"


uint32_t IClientUser::getAppOwnershipTicketExtendeData
(
	const AppId_t appId,
	void* pTicket,
	const uint32_t ticketSize,
	uint32_t* pOffAppId,
	uint32_t* pOffSteamId,
	uint32_t* pOffSig,
	uint32_t* pSigSize
)
{
	return Hooks::IClientUser_GetAppOwnershipTicketExtendedData.originalFn.fn(this, appId, pTicket, ticketSize, pOffAppId, pOffSteamId, pOffSig, pSigSize);
}
