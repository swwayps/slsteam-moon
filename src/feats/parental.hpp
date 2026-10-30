// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "protowire.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Parental
{

inline bool looksLikeSettingsReceiver(const uint8_t* code, std::size_t len)
{
	// x86-64 (instance, settings, len, signature, signatureLen, a6, a7): the
	// argument registers are saved in this order before the frame is set up.
	static constexpr int expected[] = {
		0x41, 0x57, 0x41, 0x56, 0x45, 0x89, 0xc6,
		0x41, 0x55, 0x49, 0x89, 0xf5,
		0x41, 0x54, 0x41, 0x89, 0xd4,
		0x55, 0x48, 0x89, 0xfd, 0x53, 0x48, 0x89, 0xcb,
		0x48, 0x81, 0xec,   -1,   -1, 0x00, 0x00,
	};
	constexpr std::size_t expectedLen = sizeof(expected) / sizeof(expected[0]);
	if (!code || len < expectedLen) return false;
	for (std::size_t i = 0; i < expectedLen; ++i)
	{
		if (expected[i] >= 0 && code[i] != expected[i]) return false;
	}
	return true;
}

namespace Wire = ::ProtoWire;

inline std::optional<std::vector<uint8_t>>
rewriteSettings(const uint8_t* data, std::size_t len)
{
	std::vector<uint8_t> out;
	bool parentalEnabled = false;
	const bool valid = Wire::walk(data, len, [&](const Wire::Field& field) {
		switch (field.number)
		{
		case 9: // is_enabled
		{
			if (field.wireType == 0)
			{
				std::size_t pos = field.valueOff;
				uint64_t value = 0;
				if (Wire::readVarint(data, len, pos, value))
				{
					parentalEnabled = value != 0;
				}
			}
			return;
		}
		case 10: // enabled_features
		case 13: // temporary_enabled_features
		case 15: // playtime_restrictions
		case 16: // temporary_playtime_restrictions
			return;
		default:
			out.insert(out.end(), data + field.start, data + field.end);
			return;
		}
	});
	if (!valid) return std::nullopt;
	if (!parentalEnabled) return std::vector<uint8_t>(data, data + len);

	// Keep parental mode active while allowing every locally unlockable feature.
	// Disabling parental mode makes Steam normalize these masks to all bits,
	// including an unnamed feature value that produces startup assertions.
	constexpr uint64_t unlockableFeatures = 0x7fffULL;
	Wire::putVarintField(out, 9, 1);
	Wire::putVarintField(out, 10, unlockableFeatures);
	Wire::putVarintField(out, 13, unlockableFeatures);
	return out;
}

void setup();
void remove();

} // namespace Parental
