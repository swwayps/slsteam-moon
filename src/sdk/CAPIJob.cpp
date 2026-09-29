#include "CAPIJob.hpp"

#include "../hooks.hpp"


uint32_t CAPIJob::sendAndRecv(CProtoBufMsgBase* send, uint32_t timeOut, CProtoBufMsgBase* recv, uint32_t targetType)
{
	return Hooks::CAPIJob_SendAndRecv.tramp.fn(this, send, 1, timeOut, recv, targetType);
}
