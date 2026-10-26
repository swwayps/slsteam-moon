// SPDX-License-Identifier: AGPL-3.0-only
//
// SHA-1 (FIPS 180-4) for appinfo.vdf's binary_hash and the cache pair digest.
// Computed here so no digest depends on which libcrypto, if any, the Steam
// process has loaded: a missing SHA1 symbol used to zero every digest, and a
// zero digest matched whatever bytes it was meant to protect.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Sha1
{
inline void digest(const void* data, std::size_t size, std::uint8_t out[20])
{
	std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
	const auto rotl = [](std::uint32_t value, int bits)
	{
		return (value << bits) | (value >> (32 - bits));
	};
	const auto block = [&](const std::uint8_t* p)
	{
		std::uint32_t w[80];
		for (int i = 0; i < 16; ++i)
		{
			w[i] = std::uint32_t{p[4 * i]} << 24 | std::uint32_t{p[4 * i + 1]} << 16 |
			       std::uint32_t{p[4 * i + 2]} << 8 | std::uint32_t{p[4 * i + 3]};
		}
		for (int i = 16; i < 80; ++i)
			w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

		std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; ++i)
		{
			std::uint32_t f = 0, k = 0;
			if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
			else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
			else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
			const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rotl(b, 30);
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	};

	const auto* bytes = static_cast<const std::uint8_t*>(data);
	const std::size_t whole = size - size % 64;
	for (std::size_t at = 0; at < whole; at += 64)
		block(bytes + at);

	// The last partial block, the 0x80 marker and the 64-bit bit length take
	// one or two more blocks.
	std::uint8_t tail[128] = {};
	const std::size_t rest = size - whole;
	if (rest != 0)
		std::memcpy(tail, bytes + whole, rest);
	tail[rest] = 0x80;
	const std::size_t tailSize = rest < 56 ? 64 : 128;
	const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
	for (int i = 0; i < 8; ++i)
		tail[tailSize - 1 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
	block(tail);
	if (tailSize == 128)
		block(tail + 64);

	for (int i = 0; i < 5; ++i)
	{
		out[4 * i] = static_cast<std::uint8_t>(h[i] >> 24);
		out[4 * i + 1] = static_cast<std::uint8_t>(h[i] >> 16);
		out[4 * i + 2] = static_cast<std::uint8_t>(h[i] >> 8);
		out[4 * i + 3] = static_cast<std::uint8_t>(h[i]);
	}
}
} // namespace Sha1
