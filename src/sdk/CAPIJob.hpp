#pragma once

#include <cstdint>


class CProtoBufMsgBase;

class CAPIJob
{
public:
	uint32_t sendAndRecv(CProtoBufMsgBase* send, uint32_t timeOut, CProtoBufMsgBase* recv, uint32_t targetType);
};
