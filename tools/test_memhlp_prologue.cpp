// TDD regression test for the lower-bound guard in backward prologue scans.

#include "memhlp.hpp"

#include <cstdio>

namespace
{
	int failures = 0;

	void check(bool condition, const char* message)
	{
		if (!condition)
		{
			std::fprintf(stderr, "FAIL: %s\n", message);
			++failures;
		}
	}
}

int main()
{
	check(MemHlp::prologueByteMatches(-1, 0x42),
	      "wildcard prologue byte matches any value");
	check(MemHlp::prologueByteMatches(0x42, 0x42),
	      "literal prologue byte matches itself");
	check(!MemHlp::prologueByteMatches(0x42, 0x43),
	      "literal prologue byte rejects another value");
	check(MemHlp::prologueWindowWithin(0x1100, 0x1000, 0, 4),
	      "window at address remains inside lower bound");
	check(MemHlp::prologueWindowWithin(0x1100, 0x1000, 0xfd, 4),
	      "last window ending at lower bound is allowed");
	check(!MemHlp::prologueWindowWithin(0x1100, 0x1000, 0xfe, 4),
	      "window crossing lower bound is rejected");
	check(!MemHlp::prologueWindowWithin(0x0fff, 0x1000, 0, 1),
	      "address below lower bound is rejected");
	check(!MemHlp::prologueWindowWithin(0x1100, 0x1000, 0, 0),
	      "empty prologue is rejected");
	check(MemHlp::prologueLowerBound(0x1000, 0x2000) == 0x2000,
	      "prologue scan uses the matched segment lower bound");
	check(MemHlp::prologueLowerBound(0x2000, 0x1000) == 0x2000,
	      "prologue scan never lowers the module boundary");
	check(MemHlp::prologueLowerBound(0x1000, LM_ADDRESS_BAD) == 0x1000,
	      "prologue scan falls back to module bound without a segment");

	if (failures != 0)
	{
		std::fprintf(stderr, "test_memhlp_prologue: %d failure(s)\n", failures);
		return 1;
	}
	std::puts("memhlp prologue tests passed");
	return 0;
}
