#include "CSteamEngine.hpp"

#include "IClientCompat.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"
#include "../vftableinfo.hpp"
#include "../feats/compatlive.hpp"

#include "libmem/libmem.h"

#include <array>

namespace
{
bool executableAddress(lm_address_t address)
{
	lm_segment_t segment{};
	return address != 0 && address != LM_ADDRESS_BAD &&
		LM_FindSegment(address, &segment) &&
		(segment.prot & LM_PROT_XR) == LM_PROT_XR &&
		address >= segment.base && address < segment.end;
}
}


CUser* CSteamEngine::getUser(uint32_t index)
{
	const static auto offset = *reinterpret_cast<lm_address_t*>(Patterns::CSteamEngine::Offset_User.address + 0x2);
	const auto ppUserMap = *reinterpret_cast<uint8_t**>(this + offset);

	// The user map is populated asynchronously during early bootstrap.
	// Hooks that fire before login (e.g. LoadPackage for package 0 on a
	// cold cache) can reach getUser(0) while the map pointer is still
	// null; indexing it would deref ~address 4 and segfault.  Bail out
	// so callers fall back to the CheckAppOwnership-captured user.
	if (ppUserMap == nullptr)
	{
		return nullptr;
	}

	const auto ppUser = ppUserMap + index * 8;

	return *reinterpret_cast<CUser**>(ppUser + 4);
}

void CSteamEngine::setAppIdForCurrentPipe(uint32_t appId)
{
	//Last argument needs to be 0, otherwise steam crashes.
	//Might be only 1 when steam first sets it, then 0
	Hooks::CSteamEngine_SetAppIdForCurrentPipe.tramp.fn(this, appId, 0);
}

CSteamEngine* g_pSteamEngine = nullptr;
CUser* g_pLocalUser = nullptr;

CUser* getLocalUser()
{
	// Prefer the engine-resolved user when the Init hook caught the
	// engine pointer.  Otherwise fall back to the user captured from
	// CheckAppOwnership.  Both point at the same pipe-0 CUser.
	if (g_pSteamEngine != nullptr)
	{
		CUser* user = g_pSteamEngine->getUser(0);
		if (user != nullptr)
		{
			return user;
		}
	}

	return g_pLocalUser;
}

IClientCompat* getLocalClientCompat()
{
	CUser* const user = getLocalUser();
	const lm_address_t instruction =
		Patterns::CUser::Offset_CompatManager.address;
	if (user == nullptr || instruction == 0 || instruction == LM_ADDRESS_BAD)
		return nullptr;

	std::array<std::uint8_t, 6> bytes{};
	if (LM_ReadMemory(instruction, bytes.data(), bytes.size()) != bytes.size())
		return nullptr;
	const auto offset = CompatLive::decodeManagerOffset(bytes);
	if (!offset.has_value()) return nullptr;

	auto* const compat = reinterpret_cast<IClientCompat*>(
		reinterpret_cast<std::uint8_t*>(user) + *offset);
	lm_address_t vtable = 0;
	if (LM_ReadMemory(
		reinterpret_cast<lm_address_t>(compat),
		reinterpret_cast<lm_byte_t*>(&vtable), sizeof(vtable)) !=
		sizeof(vtable) || vtable == 0 || vtable == LM_ADDRESS_BAD)
	{
		return nullptr;
	}

	for (const int index : {
		VFTIndexes::IClientCompat::SpecifyCompatTool,
		VFTIndexes::IClientCompat::GetCompatToolName})
	{
		lm_address_t target = 0;
		const lm_address_t slot = vtable +
			static_cast<lm_address_t>(index) * sizeof(lm_address_t);
		if (LM_ReadMemory(slot, reinterpret_cast<lm_byte_t*>(&target),
			sizeof(target)) != sizeof(target) ||
			!executableAddress(target))
		{
			return nullptr;
		}
	}
	return compat;
}
