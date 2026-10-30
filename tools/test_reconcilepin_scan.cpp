// Unit test for the drift-tolerant ReconcilePin locators
// (ReconcilePin::detail::findBuilderCallRel32 / findCtxDepotVectorOffset),
// x86-64 forms.
//
// The RE'd offset of the `call rel32` to the target-vector builder inside
// EvaluateConfigChanges drifts when Steam recompiles the function. The locator
// tolerates a small shift but must never hook a guessed address: the exact
// offset wins when it still holds, otherwise a nearby candidate is accepted
// ONLY when it is the unique one in the window, and ambiguity or absence gives
// up (safe disable). On x86-64 the builder's arguments travel in registers, so
// no stack cleanup marks its calls and only a call to the resolved builder
// qualifies.
//
// The content-update context's depot vector moves whenever Steam grows the
// context struct, so its offset is decoded from EvaluateConfigChanges' own
// per-entry loop instead of being pinned.
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

using ReconcilePin::detail::findBuilderCall;
using ReconcilePin::detail::findBuilderCallRel32;
using ReconcilePin::detail::findCtxDepotVectorOffset;

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

// `mov r11,[rbx+disp]; mov r14,r15; shl r14,5; add r14,r11`: the per-entry
// index into the context's DepotEntry vector.
static void putDepotIndex(std::vector<uint8_t>& buf, std::size_t off,
                          uint32_t disp, bool disp32)
{
	static constexpr uint8_t tail[] = {
		0x4D, 0x89, 0xFE, 0x49, 0xC1, 0xE6, 0x05, 0x4D, 0x01, 0xDE};
	buf[off] = 0x4C;
	buf[off + 1] = 0x8B;
	std::size_t at = off + 3;
	if (disp32)
	{
		buf[off + 2] = 0x9B;
		std::memcpy(buf.data() + at, &disp, sizeof(disp));
		at += 4;
	}
	else
	{
		buf[off + 2] = 0x5B;
		buf[at++] = static_cast<uint8_t>(disp);
	}
	std::memcpy(buf.data() + at, tail, sizeof(tail));
}

int main()
{
	constexpr std::size_t EXP = ReconcilePin::detail::kBuilderCallOff;
	constexpr std::size_t WIN = ReconcilePin::detail::kBuilderCallWindow;
	constexpr std::uintptr_t BASE = 0;   // code buffer at address 0 (small rels)
	constexpr std::uintptr_t BUILDER = 0x18500;
	const auto isBuilder = [](std::uintptr_t a) { return a == BUILDER; };

	const std::vector<uint8_t> nop(0x400, 0x90);  // NOP fill, no stray 0xE8

	CHECK(EXP == 0x19e, "the RE'd x86-64 builder call offset is +0x19e");

	// 1. Exact RE'd offset still holds -> used directly.
	{
		auto b = nop; putCall(b, EXP, BASE, BUILDER);
		CHECK(findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, isBuilder)
		          == static_cast<long>(EXP),
		      "exact RE'd offset calling the builder is used");
	}

	// 2. Offset drifted, one nearby call to the builder.
	{
		auto b = nop; b[EXP] = 0xFF; putCall(b, EXP + 9, BASE, BUILDER);
		CHECK(findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, isBuilder)
		          == static_cast<long>(EXP + 9),
		      "a unique drifted call in the window is recovered");
	}

	// 3. Two builder calls in the window -> ambiguous -> safe disable.
	{
		auto b = nop; b[EXP] = 0xFF;
		putCall(b, EXP + 9, BASE, BUILDER);
		putCall(b, EXP + 22, BASE, BUILDER);
		CHECK(findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, isBuilder) == -2,
		      "two candidate calls in the window are rejected as ambiguous");
	}

	// 4. No call at all in the window -> disabled.
	{
		auto b = nop; b[EXP] = 0xFF;
		CHECK(findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, isBuilder) == -1,
		      "no call in the window leaves the fix disabled");
	}

	// 5. The exact offset wins even if another builder call sits nearby.
	{
		auto b = nop; putCall(b, EXP, BASE, BUILDER); putCall(b, EXP + 16, BASE, BUILDER);
		CHECK(findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, isBuilder)
		          == static_cast<long>(EXP),
		      "the exact RE'd offset wins over a nearby call");
	}

	// 6. findBuilderCall: only a call to the resolved builder qualifies.
	{
		auto b = nop; putCall(b, EXP, BASE, 0x18000); putCall(b, EXP + 8, BASE, BUILDER);
		CHECK(findBuilderCall(b.data(), b.size(), BASE, BUILDER) ==
		          static_cast<long>(EXP + 8),
		      "the RE'd offset is accepted only when it calls the builder");
		CHECK(findBuilderCall(b.data(), b.size(), BASE, 0x18900) == -1,
		      "no call to the resolved builder leaves the fix disabled");
		CHECK(findBuilderCall(b.data(), b.size(), BASE, 0) == -1,
		      "without a resolved builder no call is redirected");
	}

	// 7. Context depot vector: disp32 form (x86-64 betas, ctx+0xc0).
	{
		auto b = nop; putDepotIndex(b, 0x318, 0xc0, true);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == 0xc0,
		      "the disp32 depot-vector index decodes the context offset");
	}

	// 8. Context depot vector: disp8 form.
	{
		auto b = nop; putDepotIndex(b, 0x100, 0x78, false);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == 0x78,
		      "the disp8 depot-vector index decodes the context offset");
	}

	// 9. Absent or ambiguous index loads keep the context patch disabled.
	{
		CHECK(findCtxDepotVectorOffset(nop.data(), nop.size()) == -1,
		      "no depot-vector index leaves the context patch disabled");
		auto b = nop;
		putDepotIndex(b, 0x40, 0x78, false);
		putDepotIndex(b, 0x80, 0xc0, true);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == -1,
		      "two depot-vector index loads are rejected as ambiguous");
	}

	// 10. A sequence cut off by the readable range is not decoded.
	{
		auto b = nop; putDepotIndex(b, 0x10, 0xc0, true);
		CHECK(findCtxDepotVectorOffset(b.data(), 0x10 + 12) == -1,
		      "a truncated disp32 index is not decoded");
	}

	// 11. Implausible displacements are rejected.
	{
		auto b = nop; putDepotIndex(b, 0x10, 0xc4, true);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == -1,
		      "a context offset that is not pointer-aligned is rejected");
		auto c = nop; putDepotIndex(c, 0x10, 0x20000, true);
		CHECK(findCtxDepotVectorOffset(c.data(), c.size()) == -1,
		      "an out-of-range context offset is rejected");
	}

	if (g_failures == 0) std::printf("\nall reconcilepin-scan checks passed\n");
	else                 std::printf("\n%d reconcilepin-scan check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
