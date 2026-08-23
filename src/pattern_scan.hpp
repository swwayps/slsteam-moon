#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace MemHlp
{
	struct PatternScanRangeResult
	{
		uintptr_t address = std::numeric_limits<uintptr_t>::max();
		std::size_t matches = 0;
	};

	// A signature's verdict over the whole module, accumulated range by range.
	//
	// Resolution requires exactly one match everywhere.  Keeping the first hit
	// out of several silently binds whichever site happens to sit at the lower
	// address: a locator can then be wildcarded just enough to also match a
	// neighbouring function or field, and the wrong one gets hooked with no
	// diagnostic at all.  The offline auditor already refuses that; this is the
	// same rule inside the client.
	struct PatternScanTotal
	{
		uintptr_t address = std::numeric_limits<uintptr_t>::max();
		std::size_t matches = 0;

		bool resolved() const noexcept
		{
			return matches == 1 && address != std::numeric_limits<uintptr_t>::max();
		}
	};

	inline void accumulateScan(PatternScanTotal& total,
		const PatternScanRangeResult& range) noexcept
	{
		if (range.matches == 0)
		{
			return;
		}
		total.matches += range.matches;
		if (total.address == std::numeric_limits<uintptr_t>::max())
		{
			total.address = range.address;
		}
	}

	// Scan one readable range without knowing anything about libmem.  The
	// normal path stops at the first match; the diagnostic path retains the
	// historical last-match result while counting every match.
	inline PatternScanRangeResult scanPatternRange(const std::vector<int16_t>& bytes,
		uintptr_t begin, uintptr_t end, bool countDuplicates)
	{
		PatternScanRangeResult result;
		const std::size_t patternSize = bytes.size();
		if (patternSize == 0 || end < begin || end - begin < patternSize)
		{
			return result;
		}

		const std::size_t candidateCount =
			static_cast<std::size_t>(end - begin - patternSize + 1);
		std::size_t firstLiteral = 0;
		while (firstLiteral < patternSize && bytes[firstLiteral] < 0)
		{
			++firstLiteral;
		}

		const auto matchesAt = [&](const std::uint8_t* candidate)
		{
			for (std::size_t i = 0; i < patternSize; ++i)
			{
				if (bytes[i] >= 0 && candidate[i] != static_cast<std::uint8_t>(bytes[i]))
				{
					return false;
				}
			}
			return true;
		};

		const auto record = [&](const std::uint8_t* candidate)
		{
			++result.matches;
			if (result.matches == 1 || countDuplicates)
			{
				result.address = reinterpret_cast<uintptr_t>(candidate);
			}
		};

		const auto* data = reinterpret_cast<const std::uint8_t*>(begin);
		if (firstLiteral == patternSize)
		{
			for (std::size_t offset = 0; offset < candidateCount; ++offset)
			{
				const auto* candidate = data + offset;
				if (matchesAt(candidate))
				{
					record(candidate);
					if (!countDuplicates)
					{
						return result;
					}
				}
			}
			return result;
		}

		const std::uint8_t needle = static_cast<std::uint8_t>(bytes[firstLiteral]);
		const auto* search = data + firstLiteral;
		const auto* literalEnd = search + candidateCount;
		while (search < literalEnd)
		{
			const auto* literal = reinterpret_cast<const std::uint8_t*>(
				std::memchr(search, needle, static_cast<std::size_t>(literalEnd - search)));
			if (literal == nullptr)
			{
				break;
			}

			const auto* candidate = literal - firstLiteral;
			if (matchesAt(candidate))
			{
				record(candidate);
				if (!countDuplicates)
				{
					return result;
				}
			}
			search = literal + 1;
		}

		return result;
	}
}
