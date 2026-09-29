#pragma once

#include <elf.h>
#include <libmem/libmem.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>


typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Shdr Elf_Shdr;

struct VFTable
{
	struct TypeInfo
	{
		void* classTypeInfo;
		const char* typeInfoName;
		//Not all TypeInfos have a baseClass, struct is shorter for them
		//void* baseClassType;
	};

	lm_address_t moduleBase;
	lm_address_t address;
	TypeInfo* typeInfo;
	std::vector<lm_address_t> functions;

	void init(const lm_address_t addr, const lm_module_t& mod);
	unsigned int analzye();
};


namespace Decompiler
{
	constexpr int MIN_STRING_SIZE = 5;

	extern std::unordered_map<std::string, Elf_Shdr> sections;
	extern std::unordered_map<lm_address_t, std::string> picThunks;
	extern std::unordered_map<lm_address_t, std::string> strings;
	extern std::unordered_map<std::string, VFTable> vftables;

	unsigned int isString(const lm_address_t addr, std::string* outStr);
	bool isPICThunk(const lm_inst_t& callInstr, std::string* targetRegister);
	bool getRelativeTarget(const lm_inst_t& instr, lm_address_t& target);

	void collectStrings(const lm_module_t& mod, const Elf_Shdr& section);
	bool collectVFTables(const lm_module_t& mod, const Elf_Shdr& section);

	bool parseHeader(const lm_module_t& mod);
	void parseModule(const lm_module_t& mod);

	std::map<std::string, unsigned int> parseInterfaceMapBase(const char* interface);
}
