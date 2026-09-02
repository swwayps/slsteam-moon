#pragma once

#include "steam.hpp"

#include "protobufs/steammessages_base.pb.h"

#include <cstdint>


//Helper class to make calculations more legible
class CNetPacketBody
{
public:

	EMsg type;
	uint32_t headerSize;
	//Header[headerSize]
	// Body[CNetPacket->size - headerSize - sizeof(CNetPacketBody)]
};

class CNetPacket
{
public:

	constexpr static unsigned int PROTOBUF_TYPE_MASK = 0x80000000;
	constexpr static unsigned int INVALID_MESSAGE_TYPE = 0xFFFFFFFF;

	uint8_t __pad0x0[0x4];			//0x0
	CNetPacketBody* body;			//0x4
	uint32_t size;					//0x8
	int32_t refs;					//0xC
	CNetPacketBody* originalBody;	//0x10
	uint8_t __pad0x10[0xC];			//0x14

	constexpr bool isValid() const
	{
		return body
		    && size >= sizeof(CNetPacketBody)
		    && body->type != INVALID_MESSAGE_TYPE;
	}

	constexpr EMsg getType() const
	{
		if (!body)
		{
			return INVALID_MESSAGE_TYPE;
		}

		return body->type;
	}

	constexpr bool isProtoBuf() const
	{
		return isValid() && (getType() & PROTOBUF_TYPE_MASK);
	}

	constexpr EMsg getProtoBufType() const
	{
		return getType() & ~PROTOBUF_TYPE_MASK;
	}

	bool deserializeHeader(CMsgProtoBufHeader& header) const;

	void free();
}; //0x20

static_assert(sizeof(CNetPacket) == 0x20);
