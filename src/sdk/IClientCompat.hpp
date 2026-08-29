// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

class IClientCompat
{
public:
	void specifyCompatTool(
		std::uint32_t appId,
		const char* name,
		const char* config,
		int priority);
	const char* getCompatToolName(std::uint32_t appId);
};
