// Unit test for the drift-tolerant target-vector builder call locator
// (ReconcilePin::detail::findBuilderCallRel32).
//
// The RE'd offset of the `call rel32` to the target-vector builder inside
// EvaluateConfigChanges drifts when Steam recompiles the function (e.g. on the
// beta client). The locator tolerates a small shift but must never hook a
// guessed address: it uses the exact offset when it still holds, otherwise
// accepts a nearby call ONLY when it is the unique executable-target call in
// the window, and gives up (safe disable) on ambiguity or absence.
//
// Build: g++ -std=c++20 -I src tools/test_reconcilepin_scan.cpp -o /tmp/test_reconcilepin_scan && /tmp/test_reconcilepin_scan

#include "../src/feats/reconcilepin.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
	do {                                                                     \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }       \
		else         { std::printf("ok:   %s\n", msg); }                     \
	} while (0)

using ReconcilePin::detail::findBuilderCallRel32;

// Encode an `E8 rel32` at buf[off] whose near-call target is `targetAbs`,
// given the buffer's runtime base address `base`.
static void putCall(std::vector<uint8_t>& buf, std::size_t off,
                    std::uintptr_t base, std::uintptr_t targetAbs)
{
	buf[off] = 0xE8;
	const int32_t rel = static_cast<int32_t>(
	    static_cast<intptr_t>(targetAbs) -
	    static_cast<intptr_t>(base + off + 5));
	std::memcpy(buf.data() + off + 1, &rel, sizeof(rel));
}

int main()
{
	constexpr std::size_t EXP = 0x183;   // the RE'd offset
	constexpr std::size_t WIN = 0x40;
	constexpr std::uintptr_t BASE = 0;   // code buffer at address 0 (small rels)
	// Fake "executable" region so decoded targets can be validated.
	const auto execOk = [](std::uintptr_t a) { return a >= 0x10000 && a < 0x20000; };

	const std::vector<uint8_t> nop(0x300, 0x90);  // NOP fill, no stray 0xE8

	// 1. Exact RE'd offset still holds -> used directly.
	{
		auto b = nop; putCall(b, EXP, BASE, 0x18000);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == static_cast<long>(EXP),
		      "exact RE'd offset with executable target is used");
	}

	// 2. Offset drifted (byte there is not a call), one nearby valid call.
	{
		auto b = nop; b[EXP] = 0xFF; putCall(b, EXP + 9, BASE, 0x18500);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == static_cast<long>(EXP + 9),
		      "a unique drifted call in the window is recovered");
	}

	// 3. Two valid calls in the window -> ambiguous -> safe disable.
	{
		auto b = nop; b[EXP] = 0xFF;
		putCall(b, EXP + 9, BASE, 0x18500);
		putCall(b, EXP + 22, BASE, 0x18600);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == -1, "two candidate calls in the window are rejected as ambiguous");
	}

	// 4. A call whose target is not executable is not a candidate.
	{
		auto b = nop; b[EXP] = 0xFF; putCall(b, EXP + 9, BASE, 0x99999);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == -1, "an E8 whose target is not executable is not accepted");
	}

	// 5. No call at all in the window -> disabled.
	{
		auto b = nop; b[EXP] = 0xFF;
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == -1, "no call in the window leaves the fix disabled");
	}

	// 6. The exact offset wins even if another valid call sits nearby.
	{
		auto b = nop; putCall(b, EXP, BASE, 0x18000); putCall(b, EXP + 16, BASE, 0x18700);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == static_cast<long>(EXP),
		      "the exact RE'd offset wins over a nearby call");
	}

	if (g_failures == 0) std::printf("\nall reconcilepin-scan checks passed\n");
	else                 std::printf("\n%d reconcilepin-scan check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
