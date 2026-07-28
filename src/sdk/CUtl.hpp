#pragma once

#include <cstdint>


template<typename T>
class CUtlMemory
{
public:

	T* base;
	uint32_t alloc;
	uint32_t growSize;
};

template<typename T>
class CUtlVector
{
public:

	CUtlMemory<T> memory;
	uint32_t size;

	constexpr T* at(uint32_t index)
	{
		if (index >= size)
		{
			return nullptr;
		}

		return &memory.base[index];
	}
};

class CUtlBuffer
{
public:

	CUtlMemory<uint8_t> mem;	//0x0
	int32_t get;				//0xC
	int32_t put;				//0x10
	int32_t offset;				//0x14
	uint32_t flags;				//0x1A
	uint8_t __pad0x1B[0x8];		//0x1B
}; //0x24
