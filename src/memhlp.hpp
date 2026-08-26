#pragma once

#include "libmem/libmem.h"
#include "log.hpp"
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <limits>
#include <string_view>
#include <system_error>
#include <vector>


namespace MemHlp
{
	// Parse the direct hexadecimal operand emitted for a relative call/jump.
	// Capstone also reports register and memory operands here; those are not
	// absolute targets and must fail closed instead of throwing from stoul.
	inline lm_address_t parseJumpTargetOperand(const char* operand)
	{
		if (operand == nullptr)
			return LM_ADDRESS_BAD;

		std::string_view text(operand);
		while (!text.empty()
		       && (text.front() == ' ' || text.front() == '\t'
		           || text.front() == '\r' || text.front() == '\n'))
		{
			text.remove_prefix(1);
		}
		while (!text.empty()
		       && (text.back() == ' ' || text.back() == '\t'
		           || text.back() == '\r' || text.back() == '\n'))
		{
			text.remove_suffix(1);
		}
		if (text.empty())
			return LM_ADDRESS_BAD;

		if (text.size() >= 2 && text[0] == '0'
		    && (text[1] == 'x' || text[1] == 'X'))
		{
			text.remove_prefix(2);
		}
		if (text.empty())
			return LM_ADDRESS_BAD;

		for (const char c : text)
		{
			if (!std::isxdigit(static_cast<unsigned char>(c)))
				return LM_ADDRESS_BAD;
		}

		lm_address_t value = 0;
		const auto parsed = std::from_chars(
			text.data(), text.data() + text.size(), value, 16);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
			return LM_ADDRESS_BAD;
		return value;
	}

	// Return whether a backward candidate may read `prologueSize` bytes
	// without crossing the mapped module/segment lower bound.
	inline bool prologueWindowWithin(lm_address_t address,
	                                 lm_address_t lowerBound,
	                                 lm_size_t offset,
	                                 lm_size_t prologueSize) noexcept
	{
		if (prologueSize == 0 || address < lowerBound)
			return false;
		const lm_size_t available = address - lowerBound;
		return offset <= available
		    && prologueSize - 1 <= available - offset;
	}

	inline lm_address_t prologueLowerBound(lm_address_t moduleBase,
	                                        lm_address_t segmentBase) noexcept
	{
		if (segmentBase == LM_ADDRESS_BAD || segmentBase < moduleBase)
			return moduleBase;
		return segmentBase;
	}
	inline bool formatPICThunkInstruction(char* output, std::size_t outputSize,
	                                      const char* mnemonic,
	                                      const char* operand,
	                                      lm_address_t returnAddress) noexcept
	{
		if (output == nullptr || outputSize == 0 || mnemonic == nullptr
		    || operand == nullptr || mnemonic[0] == '\0' || operand[0] == '\0')
			return false;
		const int written = std::snprintf(
			output, outputSize, "%s %s, %p", mnemonic, operand,
			reinterpret_cast<void*>(returnAddress));
		return written >= 0
		    && static_cast<std::size_t>(written) < outputSize;
	}

	enum class SigFollowMode
	{
		None,
		Relative,
		PrologueUpwards
	};

	///Summary:
	///Write assembly code to address and increase address by bytes written
	template<typename ...Args>
	bool assembleCodeAt(lm_address_t& address, const char* fmt, Args... args)
	{
		if (address == LM_ADDRESS_BAD)
		{
			g_pLog->debug("Can't write to LM_ADDRESS_BAD!\n");
			return false;
		}

		size_t size = snprintf(nullptr, 0, fmt, args...) + 1;
		char* code = reinterpret_cast<char*>(malloc(size));
		snprintf(code, size, fmt, args...);

		static lm_inst_t inst;
		//TODO: Potentially replace with LM_AssembleEx and only allocate memory as needed
		bool success = false;

		if (!LM_Assemble(code, &inst))
		{
			g_pLog->debug("Failed to assemble %s!\n", code);
		}
		else if (!LM_WriteMemory(address, inst.bytes, inst.size))
		{
			g_pLog->debug("Failed to write %s to %p!\n", code, address);
		}
		else
		{
			g_pLog->debug("Wrote %s to %p with %i bytes\n", code, address, inst.size);
			address += inst.size;
			success = true;
		}

		free(code);
		return success;
	}

	std::vector<int16_t> patternToBytes(const char* pattern);
	lm_address_t patternScan(const char* pattern, lm_module_t module);
	// Reports the module-wide match count, optionally every match address (up
	// to MemHlp::kMaxConvergenceCandidates), and returns LM_ADDRESS_BAD unless
	// exactly one match was found.  A caller that can prove convergence uses
	// the address list instead of the return value.
	lm_address_t patternScan(const char* pattern, lm_module_t module,
		std::size_t* matchesOut, std::vector<uintptr_t>* allMatches);

	struct SignatureSearchResult
	{
		lm_address_t match = LM_ADDRESS_BAD;
		lm_address_t target = LM_ADDRESS_BAD;
		// Module-wide match count.  Anything but 1 leaves both addresses bad;
		// the count is retained so the caller can say why.
		std::size_t matches = 0;
	};

	SignatureSearchResult searchSignatureDetailed(
		const char* name,
		const char* signature,
		lm_module_t module,
		SigFollowMode mode,
		void* extraData,
		size_t extraDataSize
	);
	lm_address_t searchSignature(const char* name, const char* signature, lm_module_t module, SigFollowMode mode, void* extraData, size_t extraDataSize);
	lm_address_t searchSignature(const char* name, const char* signature, lm_module_t module, SigFollowMode mode);
	lm_address_t searchSignature(const char* name, const char* signature, lm_module_t module);

	lm_address_t getJmpTarget(lm_address_t address);
	lm_address_t findPrologue(lm_address_t address, lm_address_t lowerBound,
	                          const lm_byte_t* prologueBytes, lm_size_t prologueSize);

	//TODO: Create hooking wrapper that calls this automatically
	bool fixPICThunkCall(const char* name, lm_address_t fn, lm_address_t tramp);

	const char* getTypeName(void* pClass);
	
	template<typename tFN, typename ...Args>
	constexpr auto callVFunc(unsigned int index, void* thisPtr, Args... args)
	{
		const auto fn = reinterpret_cast<tFN>(*(*reinterpret_cast<lm_address_t***>(thisPtr) + index));
		return fn(thisPtr, args...);
	}
}
