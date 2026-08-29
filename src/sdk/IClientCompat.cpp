// SPDX-License-Identifier: AGPL-3.0-only

#include "IClientCompat.hpp"

#include "../memhlp.hpp"
#include "../vftableinfo.hpp"

void IClientCompat::specifyCompatTool(
	std::uint32_t appId,
	const char* name,
	const char* config,
	int priority)
{
	MemHlp::callVFunc<void(*)(void*, std::uint32_t, const char*, const char*, int)>(
		VFTIndexes::IClientCompat::SpecifyCompatTool,
		this, appId, name, config, priority);
}

const char* IClientCompat::getCompatToolName(std::uint32_t appId)
{
	return MemHlp::callVFunc<const char*(*)(void*, std::uint32_t)>(
		VFTIndexes::IClientCompat::GetCompatToolName, this, appId);
}
