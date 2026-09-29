#pragma once

#include <cstdint>


class CClientUnifiedServiceTransport
{
public:
	uint32_t sendAndRecvMsg(const char* name, void* send, void* recv);
};
