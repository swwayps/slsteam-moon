// Unit test for the drift-tolerant ReconcilePin locators
// (ReconcilePin::detail::findBuilderCallRel32 / findCtxDepotVectorOffset).
//
// The RE'd offset of the `call rel32` to the target-vector builder inside
// EvaluateConfigChanges drifts when Steam recompiles the function (e.g. on the
// beta client). The locator tolerates a small shift but must never hook a
// guessed address: a candidate must be a call whose caller then pops the
// builder's eight stack arguments (`add esp,0x20`), the exact offset wins when
// it still holds, otherwise a nearby candidate is accepted ONLY when it is the
// unique one in the window, and ambiguity or absence gives up (safe disable).
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
// given the buffer's runtime base address `base`.  `cleanup` appends the
// caller's `add esp,0x20` that pops the builder's eight stack arguments.
static void putCall(std::vector<uint8_t>& buf, std::size_t off,
                    std::uintptr_t base, std::uintptr_t targetAbs,
                    bool cleanup = true)
{
	buf[off] = 0xE8;
	const int32_t rel = static_cast<int32_t>(
	    static_cast<intptr_t>(targetAbs) -
	    static_cast<intptr_t>(base + off + 5));
	std::memcpy(buf.data() + off + 1, &rel, sizeof(rel));
	if (cleanup)
	{
		buf[off + 5] = 0x83;
		buf[off + 6] = 0xC4;
		buf[off + 7] = 0x20;
	}
}

// `mov edi,[ebp+8]; mov eax,esi; shl eax,5; add eax,[edi+disp]`: the
// per-entry index into the context's DepotEntry vector.
static void putDepotIndex(std::vector<uint8_t>& buf, std::size_t off,
                          uint32_t disp, bool disp32)
{
	const uint8_t head[] = {0x8B, 0x7D, 0x08, 0x89, 0xF0, 0xC1, 0xE0, 0x05, 0x03};
	std::memcpy(buf.data() + off, head, sizeof(head));
	if (disp32)
	{
		buf[off + sizeof(head)] = 0x87;
		std::memcpy(buf.data() + off + sizeof(head) + 1, &disp, sizeof(disp));
	}
	else
	{
		buf[off + sizeof(head)] = 0x47;
		buf[off + sizeof(head) + 1] = static_cast<uint8_t>(disp);
	}
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
		CHECK(r == -2, "two candidate calls in the window are rejected as ambiguous");
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

	// 7. Beta-client layout: the builder call moved to +0x186 and a second
	//    executable call sits at +0x1B6. The resolved builder picks the call.
	{
		auto b = nop; b[EXP] = 0xFF;
		putCall(b, 0x186, BASE, 0x18500);
		putCall(b, 0x1B6, BASE, 0x18600);
		CHECK(findBuilderCall(b.data(), b.size(), BASE, 0x18500, execOk) == 0x186,
		      "the resolved builder disambiguates two calls in the window");
		CHECK(findBuilderCall(b.data(), b.size(), BASE, 0, execOk) == -2,
		      "without a resolved builder two calls stay ambiguous");
	}

	// 8. A call at the RE'd offset to another function is not the builder.
	{
		auto b = nop; putCall(b, EXP, BASE, 0x18000); putCall(b, EXP + 8, BASE, 0x18500);
		CHECK(findBuilderCall(b.data(), b.size(), BASE, 0x18500, execOk) ==
		          static_cast<long>(EXP + 8),
		      "the RE'd offset is accepted only when it calls the builder");
		CHECK(findBuilderCall(b.data(), b.size(), BASE, 0x18900, execOk) == -1,
		      "no call to the resolved builder leaves the fix disabled");
	}
	// 9. The 2026-10 client: the builder call moved by three bytes and the
	//    next call (two stack arguments, no 8-dword cleanup) now sits inside
	//    the window too.  Only the eight-argument call is the builder.
	{
		auto b = nop; b[EXP] = 0xFF;
		putCall(b, EXP + 3, BASE, 0x18500);
		putCall(b, EXP + 0x33, BASE, 0x18600, /*cleanup=*/false);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == static_cast<long>(EXP + 3),
		      "a call without the builder's 8-argument cleanup is not a candidate");
	}

	// 10. A call at the exact offset that does not pop eight arguments is a
	//    different callee and must not be redirected.
	{
		auto b = nop; putCall(b, EXP, BASE, 0x18000, /*cleanup=*/false);
		const long r = findBuilderCallRel32(b.data(), b.size(), BASE, EXP, WIN, execOk);
		CHECK(r == -1, "the exact offset without the 8-argument cleanup is rejected");
	}

	// 11. Context depot vector: disp8 form (client before 2026-10, ctx+0x78).
	{
		auto b = nop; putDepotIndex(b, 0x25b - 5, 0x78, false);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == 0x78,
		      "the disp8 depot-vector index decodes the context offset");
	}

	// 12. Context depot vector: disp32 form (2026-10 client, ctx+0x90).
	{
		auto b = nop; putDepotIndex(b, 0x263 - 5, 0x90, true);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == 0x90,
		      "the disp32 depot-vector index decodes the context offset");
	}

	// 13. Absent or ambiguous index loads keep the context patch disabled.
	{
		CHECK(findCtxDepotVectorOffset(nop.data(), nop.size()) == -1,
		      "no depot-vector index leaves the context patch disabled");
		auto b = nop;
		putDepotIndex(b, 0x40, 0x78, false);
		putDepotIndex(b, 0x80, 0x90, true);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == -1,
		      "two depot-vector index loads are rejected as ambiguous");
	}

	// 14. A sequence cut off by the readable range is not decoded.
	{
		auto b = nop; putDepotIndex(b, 0x10, 0x90, true);
		CHECK(findCtxDepotVectorOffset(b.data(), 0x10 + 11) == -1,
		      "a truncated disp32 index is not decoded");
	}

	// 15. Implausible displacements are rejected.
	{
		auto b = nop; putDepotIndex(b, 0x10, 0x7A, false);
		CHECK(findCtxDepotVectorOffset(b.data(), b.size()) == -1,
		      "a misaligned context offset is rejected");
		auto c = nop; putDepotIndex(c, 0x10, 0x20000, true);
		CHECK(findCtxDepotVectorOffset(c.data(), c.size()) == -1,
		      "an out-of-range context offset is rejected");
	}

	if (g_failures == 0) std::printf("\nall reconcilepin-scan checks passed\n");
	else                 std::printf("\n%d reconcilepin-scan check(s) FAILED\n", g_failures);
	return g_failures == 0 ? 0 : 1;
}
