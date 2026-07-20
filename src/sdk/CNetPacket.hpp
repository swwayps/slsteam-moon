#pragma once

#include "steam.hpp"

#include "protobufs/steammessages_base.pb.h"

#include "../log.hpp"

#include <cstdint>
#include <cstring>


//Helper class to make calculations more legible
class CNetPacketBody
{
public:

	EMsg type;
	uint32_t headerSize;
	//Header[headerSize]
	//Body[CNetPacket->size - headerSize - sizeof(CNetPacketBody)]
};

constexpr static unsigned int MAX_PACKET_SIZE = 8192;
constexpr static unsigned int MAX_PACKETS = 64;

static uint8_t PACKETS_ARRAY[MAX_PACKET_SIZE * MAX_PACKETS] { };
static uint32_t PACKETS_ARRAY_INDEX = 0;

class CNetPacket
{
public:

	constexpr static unsigned int PROTOBUF_TYPE_MASK = 0x80000000;
	constexpr static unsigned int INVALID_MESSAGE_TYPE = 0xFFFFFFFF;

	uint8_t __pad0x0[0x4];			//0x0
	CNetPacketBody* body;			//0x4
	uint32_t size;					//0x8
	uint8_t __pad_0xC[0x4];			//0xC
	CNetPacketBody* originalBody;	//0x10

	constexpr bool isValid() const
	{
		return body && size >= 8 && body->type != INVALID_MESSAGE_TYPE;
	}

	constexpr EMsg getType() const
	{
		if (!body)
		{
			return 0;
		}

		return body->type;
	}

	constexpr bool isProtoBuf() const
	{
		return getType() & PROTOBUF_TYPE_MASK;
	}

	constexpr EMsg getProtoBufType() const
	{
		return getType() & ~PROTOBUF_TYPE_MASK;
	}

	CMsgProtoBufHeader deserializeHeader() const;

	template<typename T>
	constexpr void serializeBody(const T& msg)
	{
		const uintptr_t msgOffset = body->headerSize + sizeof(CNetPacketBody);
		const uintptr_t newSize = msg.ByteSizeLong() + msgOffset;
		//uint8_t* mem = reinterpret_cast<uint8_t*>(malloc(newSize));
		uint8_t* mem = &PACKETS_ARRAY[PACKETS_ARRAY_INDEX * MAX_PACKET_SIZE];

		if (newSize >= MAX_PACKET_SIZE)
		{
			g_pLog->debug("Failed to serialize %p! Buffer to small (needed %u, has %u)\n", getType(), newSize, MAX_PACKET_SIZE);
			return;
		}

		memcpy(mem, body, msgOffset);
		if (!msg.SerializeToArray(mem + msgOffset, msg.ByteSizeLong()))
		{
			g_pLog->debug("Failed to serialize %p!\n", getType());
			return;
		}

		body = reinterpret_cast<CNetPacketBody*>(mem);
		size = newSize;
		//If I understand correctly Steam cleans up for us, that's why we crash when we free the oldBody ourself
		//However the body we allocate doesn't get freed, so we just reuse a buffer for it

		g_pLog->debug("Serialized %p into PACKETS_ARRAY at %u with size %u\n", getType(), PACKETS_ARRAY_INDEX, newSize);

		PACKETS_ARRAY_INDEX++;
		if (PACKETS_ARRAY_INDEX >= MAX_PACKETS)
		{
			PACKETS_ARRAY_INDEX = 0;
		}
	}

	template<typename T>
	constexpr T deserializeBody() const
	{
		const uintptr_t msgOffset = body->headerSize + sizeof(CNetPacketBody);
		auto msg = T();

		msg.ParseFromArray(reinterpret_cast<uint8_t*>(body) + msgOffset, size - msgOffset);

		return msg;
	}

	constexpr void clearBody()
	{
		//Hide body and call original function so steam uses it's own free
		//We can also free the body ourself if we don't call the original function
		size = body->headerSize + sizeof(CNetPacketBody);
	}
}; //0x14
