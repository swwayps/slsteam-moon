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
		return getType() != INVALID_MESSAGE_TYPE;
	}

	constexpr EMsg getType() const
	{
		if (!body || size < sizeof(CNetPacketBody))
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

	template<typename T>
	void serialize(const T& message, const CMsgProtoBufHeader* header)
	{
		const uintptr_t headerOffset = sizeof(CNetPacketBody);
		const uintptr_t headerSize = header
			? header->ByteSizeLong()
			: body->headerSize;
		const uintptr_t messageOffset = headerOffset + headerSize;
		const uintptr_t newSize = message.ByteSizeLong() + messageOffset;
		auto* memory = static_cast<uint8_t*>(Steam::Plat_Alloc(newSize));
		if (!memory)
		{
			g_pLog->warn("Failed to allocate packet body with size %zu\n", newSize);
			return;
		}
		auto* newBody = reinterpret_cast<CNetPacketBody*>(memory);
		newBody->type = body->type;
		newBody->headerSize = headerSize;
		if (header)
		{
			if (!header->SerializeToArray(memory + headerOffset, headerSize))
			{
				Steam::Plat_Free(memory);
				return;
			}
		}
		else
		{
			std::memcpy(memory + headerOffset,
				reinterpret_cast<uint8_t*>(body) + headerOffset,
				headerSize);
		}

		if (!message.SerializeToArray(memory + messageOffset, message.ByteSizeLong()))
		{
			Steam::Plat_Free(memory);
			return;
		}

		Steam::Plat_Free(body);
		body = reinterpret_cast<CNetPacketBody*>(memory);
		size = newSize;
		originalBody = body;
	}

	template<typename T>
	void serialize(const T& message)
	{
		serialize(message, nullptr);
	}

	template<typename T>
	T deserializeBody() const
	{
		const uintptr_t messageOffset = body->headerSize + sizeof(CNetPacketBody);
		T message;
		message.ParseFromArray(
			reinterpret_cast<uint8_t*>(body) + messageOffset,
			size - messageOffset);
		return message;
	}

	void free();
}; //0x20

static_assert(sizeof(CNetPacket) == 0x20);
