#pragma once

#include <cstddef>
#include <cstdint>

template<typename T>
class CUtlMemory
{
public:
	T* base;			//0x0
	uint32_t alloc;		//0x8
	uint32_t growSize;	//0xC
}; //0x10

template<typename T>
class CUtlVector
{
public:
	CUtlMemory<T> memory;	//0x0
	uint32_t size;			//0x10

	constexpr T* at(uint32_t index)
	{
		if (index >= size)
		{
			return nullptr;
		}

		return &memory.base[index];
	}

	constexpr bool swap(uint32_t index, uint32_t index2)
	{
		if (index >= size || index2 >= size)
		{
			return false;
		}

		T buf = *at(index2);
		*at(index2) = *at(index);
		*at(index) = buf;
		return true;
	}
};

class CUtlBuffer
{
	typedef int(*CUtlBuffer_Function1_t)(void*);
	typedef bool(*CUtlBuffer_Resize_t)(void*, int32_t);

public:
	CUtlMemory<uint8_t> mem;			//0x0
	int32_t get;						//0x10
	int32_t put;						//0x14
	int32_t offset;						//0x18
	uint32_t flags;						//0x1C
	CUtlBuffer_Function1_t fn0x20;		//0x20
	void* __pad0x28;					//0x28
	CUtlBuffer_Resize_t resizeFn;		//0x30
	void* __pad0x38;					//0x38

	constexpr bool hasBytes(const std::size_t count) const
	{
		return mem.base && put >= 0 &&
			static_cast<std::size_t>(put) >= count;
	}
}; //0x40

static_assert(sizeof(CUtlMemory<uint32_t>) == 0x10);
static_assert(sizeof(CUtlBuffer) == 0x40);
