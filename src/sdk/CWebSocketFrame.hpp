
#pragma once

#include <cstdint>


constexpr uint32_t kMsgHdrProtoFlag = 0x80000000u;

struct MsgHdr
{
	uint32_t eMsg;          // raw value: actual eMsg OR kMsgHdrProtoFlag
	uint32_t headerLength;  // length of the CMsgProtoBufHeader that follows
};

struct CRemoteClientPacket
{
	uint32_t   m_hConnection;        // HCONNECTION
	uint8_t*   m_pubData;
	uint32_t   m_cubData;
	int32_t    m_cRef;
	uint8_t*   m_pubNetworkBuffer;
	CRemoteClientPacket* m_pNext;
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
constexpr uint32_t kEMsgServiceMethodResponse       = 147;
