#include "decompiler.hpp"

#include "ascii.hpp"
#include "log.hpp"
#include "utils.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <string>
#include <vector>


void VFTable::init(const lm_address_t addr, const lm_module_t& mod)
{
	this->moduleBase = mod.base;
	this->address = addr;
	this->typeInfo = reinterpret_cast<TypeInfo*>(addr + sizeof(this->address));
	this->functions = std::vector<lm_address_t>();
}

unsigned int VFTable::analzye()
{
	//*(address + 0) = 0
	//*(address + sizeof(lm_address_t)) = TypeInfo*
	//*(address + sizeof(lm_address_t) * 2) = First VFunc
	
	//Already analysed
	if (functions.size())
	{
		return 0;
	}

	const lm_address_t start = address + sizeof(lm_address_t) * 2;

	for(unsigned int i = 0; ;i++)
	{
		const lm_address_t offset = *(reinterpret_cast<lm_address_t*>(start) + i);
		if (!offset)
		{
			return i;
		}

		this->functions.emplace_back(offset + moduleBase);
	}

	return 0;
}


std::unordered_map<std::string, Elf_Shdr> Decompiler::sections = std::unordered_map<std::string, Elf_Shdr>();
std::unordered_map<lm_address_t, std::string> Decompiler::picThunks = std::unordered_map<lm_address_t, std::string>();
std::unordered_map<lm_address_t, std::string> Decompiler::strings = std::unordered_map<lm_address_t, std::string>();
std::unordered_map<std::string, VFTable> Decompiler::vftables = std::unordered_map<std::string, VFTable>();

unsigned int Decompiler::isString(const lm_address_t addr, std::string* outStr)
{
	const char* pChAddr = reinterpret_cast<const char*>(addr);
	bool nullTerminated = false;
	unsigned int i = 0;

	for(; ; i++)
	{
		const char c = *(pChAddr + i);
		if (c == '\0')
		{
			nullTerminated = true;
			break;
		}

		//Is char?
		if (!Ascii::isPrint(static_cast<unsigned char>(c)))
		{
			break;
		}
	}

	if (i && outStr && nullTerminated)
	{
		*outStr = std::string(pChAddr, i);
}

	//i = offset
	//i + 1 = size/num read
	return i + 1;
}

bool Decompiler::getRelativeTarget(const lm_inst_t& instr, lm_address_t& target)
{
	auto str = std::string(instr.op_str);
	//Numbers start with 0x
	if (str.size() < 3)
	{
		return false;
	}
	str = str.substr(2, str.size() - 2);

	if (std::find_if(str.begin(), str.end(), [](const unsigned char c) { return !Ascii::isHexDigit(c); }) != str.end())
	{
		return false;
	}

	target = std::stoul(str, nullptr, 16);
	return true;
}

bool Decompiler::isPICThunk(const lm_inst_t& callInstr, std::string* targetRegister)
{
	//Shit is slow, so we cache thunks we already found
	if (picThunks.contains(callInstr.address))
	{
		if (targetRegister)
		{
			*targetRegister = picThunks.at(callInstr.address);
		}

		return true;
	}

	if (strcmp(callInstr.mnemonic, "call") != 0)
	{
		return false;
	}

	//g_pLog->debug("Checking %s %s at %p\n", callInstr.mnemonic, callInstr.op_str, callInstr.address);

	lm_address_t target;
	if (!getRelativeTarget(callInstr, target))
	{
		return false;
	}

	//g_pLog->debug("Call target at %p\n", target);

	std::string espTarget;

	lm_inst_t instr;
	for(unsigned int i = 0; i < 2; i++)
	{
		if (!LM_Disassemble(target, &instr))
		{
			g_pLog->debug("Failed to disassemble %p!\n", target);
			return false;
		}

		target += instr.size;

		auto splits = std::vector<std::string>();

		switch(i)
		{
			case 0:
				if (strcmp(instr.mnemonic, "mov") != 0)
				{
					return false;
				}

				splits = Utils::strsplit(instr.op_str, ",");
				espTarget = splits[0];

				//g_pLog->debug("Target %s\n", espTarget.c_str());

				break;

			case 1:
				if (strcmp(instr.mnemonic, "ret") != 0)
				{
					return false;
				}

				break;
		}
	}

	//g_pLog->debug("Found PIC thunk call at %p\n", callInstr.address);
	picThunks[callInstr.address] = espTarget;

	if (targetRegister)
	{
		*targetRegister = espTarget;
	}

	return true;
}

void Decompiler::collectStrings(const lm_module_t& mod, const Elf_Shdr& section)
{
	const lm_address_t start = mod.base + section.sh_addr;
	const lm_address_t end = start + section.sh_size;

	std::string strBuf;

	for(lm_address_t addr = start; addr < end; )
	{
		lm_address_t begin = addr;
		unsigned int read = isString(addr, &strBuf);

		addr += read;

		//Check strBuf.size() because read will be filled for non null terminated strings too
		if (strBuf.size() < MIN_STRING_SIZE)
		{
			continue;
		}

		strings[begin] = strBuf;
		//g_pLog->debug("Found string %s at %p with size %u\n", strBuf.c_str(), begin, strBuf.size());
	}
}

bool Decompiler::collectVFTables(const lm_module_t& mod, const Elf_Shdr& section)
{
	const lm_address_t start = mod.base + section.sh_addr;
	const lm_address_t end = start + section.sh_size;

	auto typeInfos = std::unordered_map<lm_address_t, std::string>();

	//TODO: Iterate the section backwards to do it all in one pass
	//TODO: Improve typeInfo detection, since it also detects arbitrary strings at TypeInfos
	//Potential solutions: Matching the name via regex

	//First pass to collect all typeInfos. Luckily .data.rel.ro seems to be sizeof(lm_address_t) byte aligned
	for(lm_address_t addr = start; addr < end; addr += sizeof(addr))
	{
		const lm_address_t offset = *reinterpret_cast<const lm_address_t*>(addr);
		if (!offset)
		{
			continue;
		}

		const lm_address_t ptr = mod.base + offset;

		if (ptr < mod.base)
		{
			continue;
		}

		if (ptr > mod.end)
		{
			continue;
		}

		if (strings.contains(ptr))
		{
			//TypeName at TypeInfo + sizeof(lm_address_t)
			const lm_address_t typeInfo = addr - sizeof(addr);
			const auto& name = strings.at(ptr);

			//g_pLog->debug("TypeInfo for %s at %p\n", name.c_str(), typeInfo);
			typeInfos[typeInfo] = name;
		}
	}

	//Second pass to find actual VFTables by looking up the TypeInfos
	for(lm_address_t addr = start; addr < end; addr += sizeof(addr))
	{
		const lm_address_t offset = *reinterpret_cast<const lm_address_t*>(addr);
		if (!offset)
		{
			continue;
		}

		const lm_address_t ptr = mod.base + offset;

		if (ptr < mod.base)
		{
			continue;
		}

		if (ptr > mod.end)
		{
			continue;
		}

		if (typeInfos.contains(ptr))
		{
			const lm_address_t vftAddr = addr - sizeof(addr);
			const auto& name = typeInfos.at(ptr);

			auto vft = VFTable();
			vft.init(vftAddr, mod);
			vftables[name] = vft;
			//g_pLog->debug("VFTable %s at %p\n", name.c_str(), vft.address);
		}
	}

	return false;
}

bool Decompiler::parseHeader(const lm_module_t& mod)
{
	//We parse the ELF binary from disk because trying to do so from memory f's up
	g_pLog->debug("Decompiler::parseHeader(%s)\n", mod.name);

	FILE* file = fopen(mod.path, "r");
	if (!file)
	{
		g_pLog->debug("Failed to open file for parsing Elf headers!\n");
		return false;
	}

	Elf_Ehdr hdr;
	if (fread(&hdr, sizeof(hdr), 1, file) < 1)
	{
		g_pLog->debug("Failed to read Elf header!\n");
		return false;
	}

	g_pLog->debug("shsstrndx %u\n", hdr.e_shstrndx);

	if (sizeof(Elf_Shdr) < hdr.e_shentsize)
	{
		g_pLog->debug("hdr.e_shentsize < sizeof(Elf_Shdr)!\n");
		return false;
	}

	auto shdrs = std::vector<Elf_Shdr>();
	shdrs.resize(hdr.e_shnum);

	if (fseek(file, hdr.e_shoff, SEEK_SET) != 0)
	{
		g_pLog->debug("Failed to seek to section headers\n");
		return false;
	}

	if (fread(shdrs.data(), sizeof(Elf_Shdr), shdrs.size(), file) < shdrs.size())
	{
		g_pLog->debug("Failed to read section headers\n");
		return false;
	}

	const Elf_Shdr& strHdr = shdrs[hdr.e_shstrndx];
	auto strSec = std::vector<char>();
	strSec.resize(strHdr.sh_size);

	if (fseek(file, strHdr.sh_offset, SEEK_SET) != 0)
	{
		g_pLog->debug("Failed to seek to strHdr.sh_addr!\n");
		return false;
	}
	
	if (fread(strSec.data(), sizeof(unsigned char), strSec.size(), file) < strSec.size())
	{
		g_pLog->debug("Failed to seek to strHdr.sh_addr!\n");
		return false;
	}

	g_pLog->debug("strHdr name %u address %p\n", strHdr.sh_name, strHdr.sh_offset);

	for(const auto& shdr : shdrs)
	{
		if (!shdr.sh_name)
		{
			g_pLog->debug("Skipping nameless section\n");
			continue;
		}

		const char* name = &strSec[shdr.sh_name];
		g_pLog->debug("Section header name %s, address %p, offset %p\n", name, shdr.sh_addr, shdr.sh_offset);

		auto mapName = std::string(mod.name) + "::" + name;
		sections[mapName] = shdr;
	}

	return true;
}

void Decompiler::parseModule(const lm_module_t &mod)
{
	if (!parseHeader(mod))
	{
		return;
	}

	const Elf_Shdr& shText = sections[std::string(mod.name) + "::.text"];
	const Elf_Shdr& shROData = sections[std::string(mod.name) + "::.rodata"];
	const Elf_Shdr& shDataRelRO = sections[std::string(mod.name) + "::.data.rel.ro"];

	//Collect strings to cross-reference
	collectStrings(mod, shROData);
	//Use collected strings to identify typeInfos, then cross reference those to find VFTables
	collectVFTables(mod, shDataRelRO);
}

std::map<std::string, unsigned int> Decompiler::parseInterfaceMapBase(const char* interface)
{
	auto functionMap = std::map<std::string, unsigned int>();
	if(!vftables.contains(interface))
	{
		return functionMap;
	}

	auto& vft = vftables[interface];
	vft.analzye();

	g_pLog->debug("Disassembling %s's functions\n", interface);

	lm_inst_t instr;
	lm_address_t leaOffset = 0;
	std::string thunkRegister;

	unsigned int index = 0;

	for(const auto& fn : vft.functions)
	{
		lm_address_t addr = fn;
		unsigned int stage = 0;

		for(unsigned int i = 0; i < 1000; i++)
		{
			if (!LM_Disassemble(addr, &instr))
			{
				g_pLog->debug("Failed to disassemble vft function %p at %p\n", fn, instr.address);
			}

			addr += instr.size;

			auto split = std::vector<std::string>();

			switch(stage)
			{
				//Check PIC thunk
				case 0:
					if (!isPICThunk(instr, &thunkRegister))
					{
						continue;
					}
					//Thunk moves the return address into our target register

					leaOffset = instr.address + instr.size;
					//g_pLog->debug("Found thunk with %s target\n", thunkRegister.c_str());
					stage++;
					break;

				//Thunk is followed by 'add thunkReg, num'
				case 1:
					if (strcmp(instr.mnemonic, "add") != 0)
					{
						continue;
					}

					//g_pLog->debug("Found %s %s\n", instr.mnemonic, instr.op_str);
					split = Utils::strsplit(instr.op_str, ",");
					if (split[0] != thunkRegister)
					{
						continue;
					}
					
					split[1] = split[1].substr(3, split[1].size() - 3);
					leaOffset += std::stoul(split[1], nullptr, 16);

					stage++;
					break;

				case 2:
					if (strcmp(instr.mnemonic, "lea") != 0)
					{
						continue;
					}

					if (!strstr(instr.op_str, thunkRegister.c_str()) || !strstr(instr.op_str, " - "))
					{
						continue;
					}

					split = Utils::strsplit(instr.op_str, "-");
					auto hexnumStr = split[1].substr(3, split[1].size() - 4);
					const lm_address_t targetAddr = leaOffset - std::stoul(hexnumStr, nullptr, 16);

					if (!strings.contains(targetAddr))
					{
						continue;
					}

					const auto& str = strings.at(targetAddr);
					if (strstr(interface, str.c_str()))
					{
						continue;
					}

					//g_pLog->debug("Found string ref to %s at %p for vft[%u]\n", str.c_str(), targetAddr, index);
					functionMap[str] = index;

					stage++;
					break;
			}

			if (stage > 2)
			{
				break;
			}
		}

		index++;
	}

	return functionMap;
}
