#include "CNetPacket.hpp"

#include <limits>

bool CNetPacket::deserializeHeader(CMsgProtoBufHeader& header) const
{
	header.Clear();
	if (!isValid())
		return false;

	CNetPacketBody* b = body();
	const uint32_t available = sizeBytes() - sizeof(CNetPacketBody);
	if (b->headerSize > available
	    || b->headerSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
		return false;

	const auto* memory =
		reinterpret_cast<const uint8_t*>(b) + sizeof(CNetPacketBody);
	return header.ParseFromArray(memory, static_cast<int>(b->headerSize));
}

void CNetPacket::free()
{
	CNetPacketBody* b = body();
	if (b)
		Steam::Plat_Free(b);

	setSize(0);
	setBody(nullptr);
	setOriginalBody(nullptr);
}
