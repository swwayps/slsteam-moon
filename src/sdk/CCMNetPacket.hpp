#pragma once

#include "steam.hpp"

#include "protobufs/steammessages_base.pb.h"

#include <cstdint>

struct CCMNetPacketBody
{
	uint32_t type;
	uint32_t headerSize;
};

struct CCMNetPacket
{
	static constexpr uint32_t INVALID_TYPE = 0xFFFFFFFFu;
	static constexpr uint32_t PROTOBUF_TYPE_MASK = 0x80000000u;

	uint8_t pad0[4];
	CCMNetPacketBody* body;
	uint32_t size;
	int32_t refs;
	CCMNetPacketBody* originalBody;

	bool isValid() const noexcept
	{
		return body && size >= sizeof(CCMNetPacketBody) && body->type != INVALID_TYPE;
	}

	bool isProtoBuf() const noexcept
	{
		return isValid() && (body->type & PROTOBUF_TYPE_MASK) != 0;
	}

	uint32_t getProtoBufType() const noexcept
	{
		return body->type & ~PROTOBUF_TYPE_MASK;
	}

	bool deserializeHeader(CMsgProtoBufHeader& header) const
	{
		if (!isValid() || body->headerSize > size - sizeof(CCMNetPacketBody))
		{
			return false;
		}

		const auto* data = reinterpret_cast<const uint8_t*>(body)
			+ sizeof(CCMNetPacketBody);
		return header.ParseFromArray(data, body->headerSize);
	}

	void release()
	{
		Steam::free(body);
		size = 0;
		body = nullptr;
		originalBody = nullptr;
	}
};
