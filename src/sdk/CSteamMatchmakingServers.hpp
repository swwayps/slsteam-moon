#pragma once

#include "steam.hpp"

#include <cstdint>


struct servernetadr_t
{
	uint16_t connectPort;	//0x0
	uint16_t queryPort;		//0x2
	uint32_t ip;			//0x4
}; //0x8

struct gameserverdetails_t
{
public:
	union
	{
		servernetadr_t address;		//0x0
		uint64_t ip64;
	};

	uint8_t __pad_0x0[0x88];	//0x8
	AppId_t appId;
};
