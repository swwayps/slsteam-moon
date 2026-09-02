#include "CNetPacket.hpp"

#include <limits>

bool CNetPacket::deserializeHeader(CMsgProtoBufHeader& header) const
{
	header.Clear();
	if (!isValid())
		return false;

	const uint32_t available = size - sizeof(CNetPacketBody);
	if (body->headerSize > available
	    || body->headerSize > static_cast<uint32_t>(std::numeric_limits<int>::max()))
		return false;

	const auto* memory =
		reinterpret_cast<const uint8_t*>(body) + sizeof(CNetPacketBody);
	return header.ParseFromArray(memory, static_cast<int>(body->headerSize));
}

void CNetPacket::free()
{
	if (body)
		Steam::Plat_Free(body);

	size = 0;
	body = nullptr;
	originalBody = nullptr;
}
