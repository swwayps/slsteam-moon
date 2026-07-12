#pragma once

template<typename T>
struct CUtlMemory
{
	T* base;
	int alloc;
	int growSize;
};

template<typename T>
struct CUtlVector
{
	CUtlMemory<T> memory;
	int size;
};

class CUtlBuffer
{
public:

	CUtlMemory<uint8_t> mem;	//0x0
	int32_t get;				//0xC
	int32_t put;				//0x10
	int32_t offset;				//0x14
	uint8_t __pad0x18[0x2];		//0x18
	uint8_t flags;				//0x1A
	uint8_t __pad0x1B[0x9];		//0x1B
}; //0x24
