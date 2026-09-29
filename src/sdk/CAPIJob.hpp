#pragma once

#include "protobufs/enums_clientserver.pb.h"

#include <cstdint>


class CProtoBufMsgBase;

class CAPIJob
{
public:
	uint32_t sendAndRecv(CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType);
};
