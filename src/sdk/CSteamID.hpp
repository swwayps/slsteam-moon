#pragma once

#include <cstdint>

class CSteamId
{
public:
	constexpr CSteamId() : steamId64(0) {}

	constexpr CSteamId(uint64_t id) : steamId64(id)
	{
		if (!steamId.accountType)
		{
			steamId64 |= 0x0110000100000000ULL;
		}
	}

	constexpr bool isSet() const { return accountId() != 0; }
	constexpr uint32_t accountId() const { return steamId.accountId; }
	constexpr uint32_t accountType() const { return steamId.accountType; }
	constexpr uint32_t universe() const { return steamId.universe; }

	struct SteamId_t
	{
		uint32_t accountId;
		uint8_t accountType;
		uint8_t __pad0x5[0x2];
		uint8_t universe;
	};

	union
	{
		SteamId_t steamId;
		uint64_t steamId64;
	};
}; //0x8

static_assert(sizeof(CSteamId) == sizeof(uint64_t));
