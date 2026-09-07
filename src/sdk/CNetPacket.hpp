#pragma once

#include "steam.hpp"

#include "protobufs/steammessages_base.pb.h"

#include "../log.hpp"

#include <cstdint>
#include <cstring>
#include <limits>


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
		if (!isValid())
		{
			return;
		}

		constexpr std::size_t headerOffset = sizeof(CNetPacketBody);
		constexpr std::size_t maxPacketSize =
			static_cast<std::size_t>(std::numeric_limits<int>::max());
		const std::size_t headerSize = header
			? header->ByteSizeLong()
			: body->headerSize;
		const std::size_t messageSize = message.ByteSizeLong();
		if (headerSize > maxPacketSize - headerOffset ||
			messageSize > maxPacketSize - headerOffset - headerSize ||
			(!header && headerSize > size - headerOffset))
		{
			return;
		}

		const std::size_t messageOffset = headerOffset + headerSize;
		const std::size_t newSize = messageSize + messageOffset;
		auto* memory = static_cast<uint8_t*>(
			Steam::Plat_Alloc(static_cast<int>(newSize)));
		if (!memory)
		{
			g_pLog->warn("Failed to allocate packet body with size %zu\n", newSize);
			return;
		}
		auto* newBody = reinterpret_cast<CNetPacketBody*>(memory);
		newBody->type = body->type;
		newBody->headerSize = static_cast<uint32_t>(headerSize);
		if (header)
		{
			if (!header->SerializeToArray(
				memory + headerOffset, static_cast<int>(headerSize)))
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

		if (!message.SerializeToArray(
			memory + messageOffset, static_cast<int>(messageSize)))
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
		T message;
		if (!isValid())
		{
			return message;
		}

		constexpr std::size_t headerOffset = sizeof(CNetPacketBody);
		const std::size_t available = size - headerOffset;
		if (body->headerSize > available)
		{
			return message;
		}

		const std::size_t messageOffset = headerOffset + body->headerSize;
		const std::size_t messageSize = size - messageOffset;
		if (messageSize > static_cast<std::size_t>(
				std::numeric_limits<int>::max()))
		{
			return message;
		}

		message.ParseFromArray(reinterpret_cast<uint8_t*>(body) + messageOffset,
			static_cast<int>(messageSize));
		return message;
	}

	void free();
}; //0x20

static_assert(sizeof(CNetPacket) == 0x20);
