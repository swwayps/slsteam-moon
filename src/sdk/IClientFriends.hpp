#pragma once

#include "steam.hpp"
#include <cstdint>


struct GamePlayed_t
{
	AppId_t appId;			//0x0
	uint8_t pad[0x14];		//0x4
}; //0x18
