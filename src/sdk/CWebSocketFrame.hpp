
#pragma once

#include <cstdint>


constexpr uint32_t kMsgHdrProtoFlag = 0x80000000u;

struct MsgHdr
{
	uint32_t eMsg;          // raw value: actual eMsg OR kMsgHdrProtoFlag
	uint32_t headerLength;  // length of the CMsgProtoBufHeader that follows
};


enum EWebSocketOpCode : uint32_t
{
	k_eWebSocketOpCode_Continuation = 0,
	k_eWebSocketOpCode_Text         = 1,
	k_eWebSocketOpCode_Binary       = 2,
	k_eWebSocketOpCode_Close        = 8,
	k_eWebSocketOpCode_Ping         = 9,
	k_eWebSocketOpCode_Pong         = 10,
};

constexpr uint32_t kEMsgServiceMethodCallFromClient = 151;
