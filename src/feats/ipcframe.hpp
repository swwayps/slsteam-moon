#pragma once

// Structural resolver for the IClient*::RunIPCFrame dispatchers.
//
// Each of these methods ends in a fixed-shape binary-search dispatch tail:
//
//     call <read msg id>; mov eax,[ebp+disp]; add esp,0x10; cmp eax,<ROOT>
//   = E8 ?? ?? ?? ??  8B 85 ?? ?? ?? ??  83 C4 10  3D <root:u32>
//
// The 32-bit <ROOT> is the search-tree root message id. It drifts whenever
// Steam adds/removes a method on the interface, which is exactly what broke
// the hard-coded byte signatures on the 2026-06-23 client update. Everything
// else in the tail is stable.
//
// Rather than pin the volatile root, we enumerate EVERY candidate of the
// generic shape and pick the one whose root is numerically NEAREST a stored
// per-interface seed. A small id drift (the common case across updates) still
// resolves to the right function with zero code change; the interfaces' seed
// roots are millions apart, so nearest-root assigns each one unambiguously.
//
// Pure logic only (no libmem / no process state) so it is host-unit-testable
// over a flat code buffer; the live-memory glue lives in patterns.cpp.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace IpcFrame
{
	struct Cand
	{
		size_t   offset;  // byte offset of the `E8` (dispatch tail start)
		uint32_t root;    // the cmp-eax root id at offset+15
	};

	// Byte offset of the u32 root immediate within a matched dispatch tail.
	inline constexpr size_t kRootByteOffset = 15;
	// Total bytes inspected per match (15-byte mask + 4-byte root).
	inline constexpr size_t kMatchSpan = kRootByteOffset + 4;

	// Scan a flat executable buffer for every generic dispatch tail and read
	// its root immediate. Never reads past `size`.
	//
	// The mask's first byte is the `E8` call opcode, so we seek candidate
	// starts with memchr (the call-opcode density in .text is low) instead of
	// testing the mask at every byte — keeps the extra load-time pass cheap.
	inline std::vector<Cand> scan(const uint8_t* code, size_t size)
	{
		// -1 == wildcard byte.
		static const int16_t mask[kRootByteOffset] = {
			0xE8, -1, -1, -1, -1,        // call rel32
			0x8B, 0x85, -1, -1, -1, -1,  // mov eax,[ebp+disp32]
			0x83, 0xC4, 0x10,            // add esp,0x10
			0x3D,                        // cmp eax,imm32 (root follows)
		};

		std::vector<Cand> out;
		if (size < kMatchSpan)
			return out;

		const size_t last = size - kMatchSpan;  // highest valid start offset
		size_t i = 0;
		while (i <= last)
		{
			const void* hit = std::memchr(code + i, 0xE8, last - i + 1);
			if (hit == nullptr)
				break;
			i = static_cast<size_t>(reinterpret_cast<const uint8_t*>(hit) - code);

			bool ok = true;
			for (size_t k = 1; k < kRootByteOffset; ++k)  // k==0 (0xE8) matched
			{
				if (mask[k] != -1 && code[i + k] != mask[k])
				{
					ok = false;
					break;
				}
			}
			if (ok)
			{
				const uint32_t root =
					  static_cast<uint32_t>(code[i + 15])
					| static_cast<uint32_t>(code[i + 16]) << 8
					| static_cast<uint32_t>(code[i + 17]) << 16
					| static_cast<uint32_t>(code[i + 18]) << 24;
				out.push_back({ i, root });
			}
			++i;
		}
		return out;
	}

	// Maximum tolerated drift between a stored seed root and the live root.
	//
	// The five interface seeds are millions of ids apart (the tightest gap is
	// ~4.0M, between IClientRemoteStorage and IClientUserStats), while observed
	// per-update drift is single digits (the message ids are near-sequential,
	// so the dispatch-tree root shifts only slightly when methods are added or
	// removed). This bound is generous enough to absorb a large method-count
	// change yet far below half the inter-seed gap, so it can never let one
	// interface's seed reach a neighbour's function.
	inline constexpr uint32_t kMaxRootDrift = 0x10000;  // 65536

	// Resolve a seed to a candidate ONLY when exactly one candidate lies within
	// `maxDrift` of `seedRoot`. Returns that candidate's index, or SIZE_MAX
	// when there is none (drift too large) or more than one (ambiguous). The
	// caller then keeps the embedded root verbatim, so an uncertain match never
	// silently rewrites a pattern to point at the wrong function — it degrades
	// to the pre-existing behaviour (a loud "pattern not found" if the embedded
	// root has itself drifted) instead of a quiet mis-hook.
	inline size_t resolveConfident(const std::vector<Cand>& cands, uint32_t seedRoot, uint32_t maxDrift)
	{
		size_t hit     = SIZE_MAX;
		size_t inBand   = 0;
		for (size_t i = 0; i < cands.size(); ++i)
		{
			const uint32_t r = cands[i].root;
			const uint64_t d = (r > seedRoot)
				? static_cast<uint64_t>(r) - seedRoot
				: static_cast<uint64_t>(seedRoot) - r;
			if (d <= maxDrift)
			{
				++inBand;
				hit = i;
			}
		}
		return (inBand == 1) ? hit : SIZE_MAX;
	}

	// A RunIPCFrame pattern string ends with "3D b0 b1 b2 b3" — the cmp-eax
	// opcode followed by the root immediate in little-endian byte order. The
	// embedded root doubles as the per-interface seed for resolveConfident, so
	// we read it straight from the pattern (no separate seed table to drift out
	// of sync with the patterns).

	// Read the little-endian u32 from the final four space-separated hex
	// tokens of `pattern`. Returns 0 if there are fewer than four tokens.
	inline uint32_t parseTrailingRoot(const std::string& pattern)
	{
		uint8_t b[4];
		size_t  filled = 0;
		size_t  end    = pattern.size();
		// Walk back over the last four whitespace-delimited tokens.
		while (filled < 4 && end != 0)
		{
			size_t e = pattern.find_last_not_of(' ', end - 1);
			if (e == std::string::npos) break;
			size_t s = pattern.find_last_of(' ', e);
			size_t tokStart = (s == std::string::npos) ? 0 : s + 1;
			b[filled++] = static_cast<uint8_t>(
				std::strtoul(pattern.substr(tokStart, e - tokStart + 1).c_str(), nullptr, 16));
			if (tokStart == 0) break;
			end = tokStart - 1;
		}
		if (filled < 4)
			return 0;
		// b[0] is the LAST token (most-significant byte of the LE root).
		return static_cast<uint32_t>(b[3])
			| static_cast<uint32_t>(b[2]) << 8
			| static_cast<uint32_t>(b[1]) << 16
			| static_cast<uint32_t>(b[0]) << 24;
	}

	// Rewrite the final four hex tokens of `pattern` with `root` in
	// little-endian order (uppercase, two digits each), preserving the prefix.
	inline void setTrailingRoot(std::string& pattern, uint32_t root)
	{
		// Find the start of the 4th-from-last token.
		size_t end   = pattern.size();
		size_t cut   = std::string::npos;
		for (int i = 0; i < 4 && end != 0; ++i)
		{
			size_t e = pattern.find_last_not_of(' ', end - 1);
			if (e == std::string::npos) break;
			size_t s = pattern.find_last_of(' ', e);
			cut = (s == std::string::npos) ? 0 : s + 1;
			if (cut == 0) break;
			end = cut - 1;
		}
		if (cut == std::string::npos)
			return;

		char tail[16];
		std::snprintf(tail, sizeof(tail), "%02X %02X %02X %02X",
			root & 0xFF, (root >> 8) & 0xFF, (root >> 16) & 0xFF, (root >> 24) & 0xFF);
		pattern.replace(cut, std::string::npos, tail);
	}
}
