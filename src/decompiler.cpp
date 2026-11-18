#include "decompiler.hpp"

#include "ascii.hpp"
#include "log.hpp"
#include "utils.hpp"

#include "libmem/libmem.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>


void VFTable_t::init(const lm_address_t addr, const lm_module_t& mod)
{
	this->moduleBase = mod.base;
	this->address = addr;
	this->typeInfo = reinterpret_cast<TypeInfo*>(addr + sizeof(this->address));
	this->functions = std::vector<lm_address_t>();
	this->subclasses = std::map<unsigned int, VFTable_t>();
}

unsigned int VFTable_t::analyze()
{
	//*(address + 0) = 0
	//*(address + sizeof(lm_address_t)) = TypeInfo*
	//*(address + sizeof(lm_address_t) * 2) = First VFunc
	
	//Already analysed
	if (functions.size())
	{
		return functions.size();
	}

	const lm_address_t start = address + sizeof(lm_address_t) * 2;

	for (unsigned int i = 0; ;i++)
	{
		const lm_address_t offset = *(reinterpret_cast<lm_address_t*>(start) + i);
		if (!offset)
		{
			break;
		}

		//TODO:
		//Proper way would be to cross reference the typeInfos, but I couldn't get the calculations for that
		//right. It worked on the first 4 vftables on CUser but then started failing
		constexpr lm_address_t NEGATIVE_OFFSET = 0xFFFF0000;
		if ((offset & NEGATIVE_OFFSET) == NEGATIVE_OFFSET)
		{
			break;
		}

		this->functions.emplace_back(offset + moduleBase);
	}

	for (auto& sub : subclasses)
	{
		sub.second.analyze();
	}

	return functions.size();
}


std::unordered_map<std::string, Elf_Shdr> Decompiler::sections = std::unordered_map<std::string, Elf_Shdr>();
std::unordered_map<lm_address_t, std::string> Decompiler::picThunks = std::unordered_map<lm_address_t, std::string>();
std::unordered_map<lm_address_t, std::string> Decompiler::strings = std::unordered_map<lm_address_t, std::string>();
std::unordered_map<lm_address_t, std::string> Decompiler::typeInfos = std::unordered_map<lm_address_t, std::string>();
std::unordered_map<std::string, VFTable_t> Decompiler::vftables = std::unordered_map<std::string, VFTable_t>();

unsigned int Decompiler::getString(const lm_address_t addr, std::string* outStr)
{
	const char* pChAddr = reinterpret_cast<const char*>(addr);
	bool nullTerminated = false;
	unsigned int i = 0;

	for (; ; i++)
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

	if (outStr)
	{
		if (i && nullTerminated)
		{
			*outStr = std::string(pChAddr, i);
		}
		else
		{
			outStr->clear();
		}
	}

	//i = offset
	//i + 1 = size/num read
	return i + 1;
}

lm_address_t Decompiler::extractHexNum(const std::string& str)
{
	const auto pos = str.find("0x");
	if (pos == std::string::npos)
	{
		return LM_ADDRESS_BAD;
	}

	const char* first = str.data() + pos + 2;
	const char* last = str.data() + str.size();
	lm_address_t value = 0;
	const auto [ptr, ec] = std::from_chars(first, last, value, 16);
	if (ec != std::errc{} || ptr == first)
	{
		return LM_ADDRESS_BAD;
	}

	return value;
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

lm_address_t Decompiler::getLeaOffset(lm_inst_t& callInstr)
{
	lm_address_t offset = LM_ADDRESS_BAD;
	std::string thunkReg;

	if (!isPICThunk(callInstr, &thunkReg))
	{
		return LM_ADDRESS_BAD;
	}

	//Thunk moves the return address into our target register
	offset = callInstr.address + callInstr.size;
	//LOG_DEBUG("Found thunk with %s target\n", thunkRegister.c_str());

	lm_inst_t nextInstr;
	if (!LM_Disassemble(offset, &nextInstr))
	{
		LOG_DEBUG("Failed to disassemble next in getLeaOffset at 0x%lx\n", offset);
		return LM_ADDRESS_BAD;
	}

	//Thunk is followed by 'add thunkReg, num'
	if (strcmp(nextInstr.mnemonic, "add") != 0)
	{
		LOG_DEBUG("Failed to get lea offset at 0x%lx, next instruction is not an add instruction!\n", offset);
		return LM_ADDRESS_BAD;
	}

	//LOG_DEBUG("Found %s %s\n", instr.mnemonic, instr.op_str);
	auto split = Utils::strsplit(nextInstr.op_str, ",");
	if (split[0] != thunkReg)
	{
		LOG_DEBUG("Failed to get lea offset at 0x%lx, split[0] != thunkReg\n", callInstr.address);
		return LM_ADDRESS_BAD;
	}

	split[1] = split[1].substr(3, split[1].size() - 3);
	offset += std::stoul(split[1], nullptr, 16);

	return offset;
}

Elf_Shdr* Decompiler::getSection(const lm_module_t& mod, const char* name)
{
	std::ostringstream ss;
	ss << mod.name << "::" << name;

	const auto secName = ss.str();
	if (!sections.contains(secName))
	{
		return nullptr;
	}

	return &sections.at(secName);
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

	//LOG_DEBUG("Checking %s %s at 0x%x\n", callInstr.mnemonic, callInstr.op_str, callInstr.address);

	lm_address_t target;
	if (!getRelativeTarget(callInstr, target))
	{
		return false;
	}

	//LOG_DEBUG("Call target at 0x%x\n", target);

	std::string espTarget;

	lm_inst_t instr;
	for (unsigned int i = 0; i < 2; i++)
	{
		if (!LM_Disassemble(target, &instr))
		{
			LOG_DEBUG("Failed to disassemble 0x%lx!\n", target);
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

				//LOG_DEBUG("Target %s\n", espTarget.c_str());

				break;

			case 1:
				if (strcmp(instr.mnemonic, "ret") != 0)
				{
					return false;
				}

				break;
		}
	}

	//LOG_DEBUG("Found PIC thunk call at 0x%x\n", callInstr.address);
	picThunks[callInstr.address] = espTarget;

	if (targetRegister)
	{
		*targetRegister = espTarget;
	}

	return true;
}

void Decompiler::collectStrings(const lm_module_t& mod, const Elf_Shdr& section)
{
	const lm_address_t start = mod.base + section.rva;
	const lm_address_t end = start + section.size;

	std::string strBuf;

	for (lm_address_t addr = start; addr < end; )
	{
		lm_address_t begin = addr;
		// getString leaves the buffer untouched for padding and binary
		// data; clear it so those bytes are not recorded as the previous
		// string.
		strBuf.clear();
		unsigned int read = getString(addr, &strBuf);

		addr += read;

		//Check strBuf.size() because read will be filled for non null terminated strings too
		if (strBuf.size() < MIN_STRING_SIZE)
		{
			continue;
		}

		strings[begin] = strBuf;
		//LOG_DEBUG("Found string %s at 0x%x with size %u\n", strBuf.c_str(), begin, strBuf.size());
	}
}

void Decompiler::collectTypeInfos(const lm_module_t& mod, const Elf_Shdr& section)
{
	const lm_address_t start = mod.base + section.rva;
	const lm_address_t end = start + section.size;

	//First pass to collect all typeInfos. Luckily .data.rel.ro seems to be sizeof(lm_address_t) byte aligned
	for (lm_address_t addr = start; addr < end; addr += sizeof(addr))
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

			//LOG_DEBUG("TypeInfo for %s at 0x%lx\n", name.c_str(), typeInfo);
			typeInfos[typeInfo] = name;
		}
	}
}

bool Decompiler::collectVFTables(const lm_module_t& mod, const Elf_Shdr& section)
{
	const lm_address_t start = mod.base + section.rva;
	const lm_address_t end = start + section.size;

	//Second pass to find actual VFTables by looking up the TypeInfos
	for (lm_address_t addr = start; addr < end; addr += sizeof(addr))
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

			auto vft = VFTable_t();
			vft.init(vftAddr, mod);

			if (vftables.contains(name))
			{
				auto& parent = vftables[name];
				parent.subclasses[parent.subclasses.size()] = vft;
				continue;
			}

			vftables[name] = vft;
			//LOG_DEBUG("VFTable %s at 0x%lx\n", name.c_str(), vft.address);
		}
	}

	return false;
}

bool Decompiler::parseHeader(const lm_module_t& mod)
{
	auto file = CELFExecutableFile();
	if (!file.load(mod.path))
	{
		return false;
	}

	for (const auto& shdr : file.sections)
	{
		LOG_DEBUG("Section header name %s, address 0x%lx, offset 0x%lx\n", shdr.name.c_str(), shdr.rva, shdr.offset);

		auto mapName = std::string(mod.name) + "::" + shdr.name;
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

	const Elf_Shdr* shROData = getSection(mod, ".rodata");
	if (shROData)
	{
		//Collect strings to cross-reference
		LOG_DEBUG("Scanning .rodata for strings in %s\n", mod.name);
		collectStrings(mod, *shROData);
	}

	// The x86-64 client keeps its literals in merged string sections
	// (.rodata.str1.1, .rodata.str1.8, ...) rather than in .rodata.
	const std::string strPrefix = std::string(mod.name) + "::.rodata.str";
	for (const auto& [secName, shdr] : sections)
	{
		if (!secName.starts_with(strPrefix))
		{
			continue;
		}
		LOG_DEBUG("Scanning %s for strings\n", secName.c_str());
		collectStrings(mod, shdr);
	}

	const Elf_Shdr* shDataRelRO = getSection(mod, ".data.rel.ro");
	const Elf_Shdr* shDataRelROLocal = getSection(mod, ".data.rel.ro.local");
	if (shDataRelRO)
	{
		collectTypeInfos(mod, *shDataRelRO);
	}
	if (shDataRelROLocal)
	{
		collectTypeInfos(mod, *shDataRelROLocal);
	}
	if (shDataRelRO)
	{
		LOG_DEBUG("Scanning .data.rel.ro for VFTables in %s\n", mod.name);
		collectVFTables(mod, *shDataRelRO);
	}
	if (shDataRelROLocal)
	{
		LOG_DEBUG("Scanning .data.rel.ro.local for VFTables in %s\n", mod.name);
		collectVFTables(mod, *shDataRelROLocal);
	}
}

// LM_Disassemble opens and closes a capstone handle for every instruction
// (about 6.5 us each), which made up 1.39 s of the 1.41 s registry scan at
// load. The walk reads short sequential runs (5 instructions on average), so
// it decodes up to kRunLength ahead in one call and serves the run from here.
struct Decompiler::DecodedRun
{
	static constexpr lm_size_t kRunLength = 16;

	lm_inst_t* insts = nullptr;
	lm_size_t count = 0;
	lm_address_t segmentBase = 0;
	lm_address_t segmentEnd = 0;

	DecodedRun() = default;
	DecodedRun(const DecodedRun&) = delete;
	DecodedRun& operator=(const DecodedRun&) = delete;
	~DecodedRun() { LM_FreeInstructions(insts); }

	bool decode(lm_address_t address, lm_inst_t* out)
	{
		for (lm_size_t i = 0; i < count; i++)
		{
			if (insts[i].address == address)
			{
				*out = insts[i];
				return true;
			}
		}

		// Never read past the mapped segment that holds the address; an
		// address no segment holds (a bogus slot or branch) ends the walk.
		if (address < segmentBase || address >= segmentEnd)
		{
			lm_segment_t segment {};
			if (!LM_FindSegment(address, &segment))
				return false;
			segmentBase = segment.base;
			segmentEnd = segment.end;
		}

		LM_FreeInstructions(insts);
		insts = nullptr;
		count = LM_DisassembleEx(address, LM_GetArchitecture(),
			std::min<lm_size_t>(segmentEnd - address, kRunLength * LM_INST_MAX),
			kRunLength, address, &insts);
		if (count == 0)
			return false;
		*out = insts[0];
		return true;
	}
};

namespace
{
	// One run across the functions of a walk keeps the segment bounds it
	// found, so the maps lookup happens once per segment, not once per slot.
	void parseFunctionWith(const lm_address_t begin,
	                       std::unordered_map<lm_address_t, unsigned int>& references,
	                       Decompiler::DecodedRun& run)
	{
		auto branchesTaken = std::unordered_set<lm_address_t>();
		Decompiler::__parseFunction(begin, references, branchesTaken, run);
	}
}

void Decompiler::parseFunction(const lm_address_t begin, std::unordered_map<lm_address_t, unsigned int>& references)
{
	DecodedRun run;
	parseFunctionWith(begin, references, run);
}

void Decompiler::__parseFunction
(
	const lm_address_t begin,
	std::unordered_map<lm_address_t, unsigned int>& references,
	std::unordered_set<lm_address_t>& branchesTaken,
	DecodedRun& run
)
{
	lm_address_t addr = begin;
	lm_inst_t instr;

	unsigned int numCC = 0;

	for (;;)
	{
		if (!run.decode(addr, &instr))
		{
			LOG_WARN("Failed to disassemble function 0x%lx at 0x%lx!\n", begin, addr);
			return;
		}

		//LOG_DEBUG("0x%lx: %s %s\n", addr, instr.mnemonic, instr.op_str);

		addr += instr.size;

		//Shitty way to detect function boundaries
		//TODO: Investigate wheter there is a better way
		if (strcmp(instr.mnemonic, "int3") == 0)
		{
			numCC++;

			if (numCC >= 3)
			{
				return;
			}
		}
		else
		{
			numCC = 0;
		}

		if (instr.mnemonic[0] == 'j')
		{
			lm_address_t branch;

			if (!getRelativeTarget(instr, branch))
			{
				// An indirect jump (switch table) ends this path; callers
				// report what they then fail to find.
				LOG_DEBUG("Failed to follow %s %s at 0x%lx!\n", instr.mnemonic, instr.op_str, instr.address);
				return;
			}

			const bool taken = branchesTaken.contains(branch);

			//LOG_DEBUG("%s branch %s %s at 0x%lx to 0x%lx\n", (taken ? "Taking" : "Skipping"), instr.mnemonic, instr.op_str, instr.address, branch);
			branchesTaken.emplace(branch);

			if (strcmp(instr.mnemonic, "jmp") == 0)
			{
				//Explored already, so we abort
				if (taken)
				{
					return;
				}

				addr = branch;
			}
			else
			{
				if (!taken)
				{
					__parseFunction(branch, references, branchesTaken, run);
				}
			}

			continue;
		}
		//Checking the log it seems like capstone is turning all ret instructions into just ret.
		//But just in case we check for all of them
		else if (strcmp(instr.mnemonic, "ret") == 0 || strcmp(instr.mnemonic, "retn") == 0 || strcmp(instr.mnemonic, "retf") == 0)
		{
			//LOG_DEBUG("Hit %s instruction at 0x%x, stopping\n", instr.mnemonic, instr.address);
			return;
		}

		if (strcmp(instr.mnemonic, "lea") != 0)
		{
			continue;
		}

		// RIP-relative string load: "reg, [rip - 0x<disp>]" (the same operands
		// upstream matches).
		if (!strstr(instr.op_str, ", [rip - 0x"))
		{
			continue;
		}

		const lm_address_t targetAddr = (instr.address + instr.size) - extractHexNum(instr.op_str);

		//LOG_DEBUG("Target addr for op 0x%lx\n", targetAddr);

		if (!strings.contains(targetAddr))
		{
			continue;
		}

		//const auto& str = strings.at(targetAddr);
		//LOG_DEBUG("String reference to %s at 0x%x\n", str.c_str(), instr.address);
		references[targetAddr]++;
	}
}

std::map<std::string, unsigned int> Decompiler::parseInterfaceMapBase(const char* interface)
{
	auto functionMap = std::map<std::string, unsigned int>();
	if (!vftables.contains(interface))
	{
		return functionMap;
	}

	auto& vft = vftables[interface];

	LOG_DEBUG("Disassembling %s's functions\n", interface);

	DecodedRun run;
	for (unsigned int i = 0; i < vft.functions.size(); i++)
	{
		//LOG_DEBUG("Decompiling %u\n", i);
		const lm_address_t fn = vft.functions[i];

		auto refs = std::unordered_map<lm_address_t, unsigned int>();
		parseFunctionWith(fn, refs, run);

		for (const auto& ref : refs)
		{
			auto str = strings[ref.first];
			if (strstr(interface, str.c_str()))
			{
				continue;
			}

			if (functionMap.contains(str))
			{
				//Some functions are overloaded, so we append a 2, 3, etc
				unsigned int idx = 2;
				for (;;)
				{
					auto indexedStr = str + std::to_string(idx);
					if (!functionMap.contains(indexedStr))
					{
						str = indexedStr;
						break;
					}

					idx++;
				}
			}

			//I would love to add a break statement after this.
			//But some functions do reference multiple strings
			//Since we can't know which one is the right one without applying possibly wrong heuristics
			//We just index all of them
			functionMap[str] = i;
			//LOG_DEBUG("%s::%s at %u (%u times)\n", interface, str.c_str(), i, ref.second);
		}
	}

	return functionMap;
}

void Decompiler::cleanUp()
{
	sections.clear();
	picThunks.clear();
	strings.clear();
	typeInfos.clear();
	vftables.clear();
}
