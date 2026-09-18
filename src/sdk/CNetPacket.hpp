#pragma once

#include "steam.hpp"

#include "protobufs/steammessages_base.pb.h"

#include "../log.hpp"

#include <atomic>
#include <cstddef>
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

// CNetPacket overlays a Steam-owned packet (in the CM/IPC receive hooks) or a
// small SLSsteam-owned scratch buffer (outgoing-frame rewrites).  Steam has
// shipped more than one field layout for it: on the classic layout `body` is at
// +0x4, but a later client (a beta) inserted 8 bytes at the front, moving every
// field +8 (body -> +0xC, size -> +0x10, refs -> +0x14, originalBody -> +0x18).
// Reading `body` at the wrong offset yields a 0xFFFFFFFF sentinel whose deref
// segfaults the whole 32-bit client on the CM receive path.
//
// Rather than hard-code either layout, the field offset shift is detected once
// from a live Steam-owned packet (detectLayout) and applied to every field
// access here.  s_fieldShift defaults to 0 and detection tries shift 0 FIRST,
// so on the stable client it stays 0 and every access is byte-for-byte
// identical to the original fixed-offset struct — the shifted path only ever
// engages on a client whose packets do not validate at 0.
class CNetPacket
{
public:

	constexpr static unsigned int PROTOBUF_TYPE_MASK = 0x80000000;
	constexpr static unsigned int INVALID_MESSAGE_TYPE = 0xFFFFFFFF;
	// A CM packet is at most a few MB in practice; anything larger is a mis-read
	// `size` field, not a real packet.
	constexpr static uint32_t MAX_PLAUSIBLE_SIZE = 100u * 1024 * 1024;

	// Base field offsets on the classic (stable) layout.
	constexpr static std::size_t OFF_BODY = 0x4;
	constexpr static std::size_t OFF_SIZE = 0x8;
	constexpr static std::size_t OFF_ORIGBODY = 0x10;

	// Detected field shift (0 = classic/stable layout).  int, single writer
	// (detectLayout runs on the CM receive thread); benign as a plain global.
	static inline int s_fieldShift = 0;

private:

	// Raw storage.  Fields live at their base offset + s_fieldShift.  0x20 bytes
	// holds every field of both layouts (the highest is originalBody at
	// 0x10 + 8 = 0x18, four bytes, ending at 0x1C); a Steam beta packet is
	// larger still, and this only ever overlays it.
	uint8_t raw_[0x20];

	template <typename T>
	T fieldGet(std::size_t off) const
	{
		T value;
		std::memcpy(&value, raw_ + off + s_fieldShift, sizeof(T));
		return value;
	}
	template <typename T>
	void fieldSet(std::size_t off, T value)
	{
		std::memcpy(raw_ + off + s_fieldShift, &value, sizeof(T));
	}

	static bool plausibleBody(CNetPacketBody* body)
	{
		const auto address = reinterpret_cast<std::uintptr_t>(body);
		return address >= 0x10000u && address < 0xFFFFF000u;
	}

public:

	CNetPacketBody* body() const { return fieldGet<CNetPacketBody*>(OFF_BODY); }
	void setBody(CNetPacketBody* value) { fieldSet<CNetPacketBody*>(OFF_BODY, value); }
	uint32_t sizeBytes() const { return fieldGet<uint32_t>(OFF_SIZE); }
	void setSize(uint32_t value) { fieldSet<uint32_t>(OFF_SIZE, value); }
	void setOriginalBody(CNetPacketBody* value) { fieldSet<CNetPacketBody*>(OFF_ORIGBODY, value); }

	// Detect the field shift once from a live Steam-owned packet.  Tries the
	// classic layout first so the stable client is unaffected, then the +8
	// layout; a candidate is accepted only when `body` is a plausible pointer,
	// `size` is sane, and the CNetPacketBody header fits inside the packet.  A
	// packet that validates at neither shift leaves the current value untouched
	// and stays undetected so a later packet can settle it.
	static void detectLayout(const CNetPacket* packet)
	{
		static std::atomic<bool> settled{false};
		if (settled.load(std::memory_order_acquire) || packet == nullptr)
			return;
		const auto* base = reinterpret_cast<const uint8_t*>(packet);
		for (const int shift : {0, 8})
		{
			CNetPacketBody* body;
			std::memcpy(&body, base + OFF_BODY + shift, sizeof(body));
			uint32_t size;
			std::memcpy(&size, base + OFF_SIZE + shift, sizeof(size));
			if (!plausibleBody(body)
			    || size < sizeof(CNetPacketBody) || size > MAX_PLAUSIBLE_SIZE)
				continue;
			uint32_t headerSize;
			std::memcpy(&headerSize,
			            reinterpret_cast<const uint8_t*>(body) + offsetof(CNetPacketBody, headerSize),
			            sizeof(headerSize));
			if (headerSize > size - sizeof(CNetPacketBody))
				continue;
			s_fieldShift = shift;
			settled.store(true, std::memory_order_release);
			g_pLog->info("CNetPacket: field layout settled at shift %d\n", shift);
			return;
		}
	}

	bool isValid() const
	{
		return getType() != INVALID_MESSAGE_TYPE;
	}

	// Reports INVALID for an implausible body/size (defense-in-depth: even if
	// the detected layout is somehow wrong, a mis-read pointer is never
	// dereferenced — the client degrades to a safe no-op instead of crashing).
	EMsg getType() const
	{
		CNetPacketBody* b = body();
		if (!plausibleBody(b))
			return INVALID_MESSAGE_TYPE;
		const uint32_t bytes = sizeBytes();
		if (bytes < sizeof(CNetPacketBody) || bytes > MAX_PLAUSIBLE_SIZE)
			return INVALID_MESSAGE_TYPE;
		return b->type;
	}

	bool isProtoBuf() const
	{
		return isValid() && (getType() & PROTOBUF_TYPE_MASK);
	}

	EMsg getProtoBufType() const
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

		CNetPacketBody* oldBody = body();
		const uint32_t oldSize = sizeBytes();
		constexpr std::size_t headerOffset = sizeof(CNetPacketBody);
		constexpr std::size_t maxPacketSize =
			static_cast<std::size_t>(std::numeric_limits<int>::max());
		const std::size_t headerSize = header
			? header->ByteSizeLong()
			: oldBody->headerSize;
		const std::size_t messageSize = message.ByteSizeLong();
		if (headerSize > maxPacketSize - headerOffset ||
			messageSize > maxPacketSize - headerOffset - headerSize ||
			(!header && headerSize > oldSize - headerOffset))
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
		newBody->type = oldBody->type;
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
				reinterpret_cast<uint8_t*>(oldBody) + headerOffset,
				headerSize);
		}

		if (!message.SerializeToArray(
			memory + messageOffset, static_cast<int>(messageSize)))
		{
			Steam::Plat_Free(memory);
			return;
		}

		Steam::Plat_Free(oldBody);
		setBody(reinterpret_cast<CNetPacketBody*>(memory));
		setSize(static_cast<uint32_t>(newSize));
		setOriginalBody(reinterpret_cast<CNetPacketBody*>(memory));
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

		CNetPacketBody* b = body();
		const uint32_t bytes = sizeBytes();
		constexpr std::size_t headerOffset = sizeof(CNetPacketBody);
		const std::size_t available = bytes - headerOffset;
		if (b->headerSize > available)
		{
			return message;
		}

		const std::size_t messageOffset = headerOffset + b->headerSize;
		const std::size_t messageSize = bytes - messageOffset;
		if (messageSize > static_cast<std::size_t>(
				std::numeric_limits<int>::max()))
		{
			return message;
		}

		message.ParseFromArray(reinterpret_cast<uint8_t*>(b) + messageOffset,
			static_cast<int>(messageSize));
		return message;
	}

	void free();
}; //0x20

static_assert(sizeof(CNetPacket) == 0x20);
