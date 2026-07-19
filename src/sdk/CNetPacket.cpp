#include "CNetPacket.hpp"


CMsgProtoBufHeader CNetPacket::deserializeHeader() const
{
	const uintptr_t headerOffset = sizeof(CNetPacketBody);
	uint8_t* mem = reinterpret_cast<uint8_t*>(body) + headerOffset;

	CMsgProtoBufHeader header;
	if (!header.ParseFromArray(mem, body->headerSize))
	{
		g_pLog->debug("Failed to parse header!\n");
	}

	return header;
}
