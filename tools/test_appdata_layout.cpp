#include "../src/feats/appdata_layout.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>

namespace
{
using AppDataLayout::Layout;
using AppDataLayout::derive;

int failures = 0;

void check(bool condition, std::string_view message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

void checkLayout(std::optional<Layout> actual, Layout expected, std::string_view message)
{
	check(actual.has_value() && *actual == expected, message);
}
}

int main()
{
	constexpr std::array<std::uint8_t, 7> skipInsn{
		0x80, 0xB8, 0x10, 0x00, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 6> shaInsn{
		0x8D, 0x90, 0x1C, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 7> wrongOpcode{
		0x81, 0xB8, 0x10, 0x00, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 7> largeSkip{
		0x80, 0xB8, 0x00, 0x10, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 7> overlapSkip{
		0x80, 0xB8, 0x20, 0x00, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 6> overlapSha{
		0x8D, 0x90, 0x18, 0x00, 0x00, 0x00
	};
	checkLayout(derive(skipInsn, shaInsn), Layout{0x10, 0x1C},
	             "validated sites derive synthetic layout");
	check(!derive(wrongOpcode, shaInsn), "wrong opcode is rejected");
	check(!derive(skipInsn, std::span(shaInsn).first(4)),
	      "truncation is rejected");
	check(!derive(largeSkip, shaInsn), "offset is bounded");
	check(!derive(overlapSkip, overlapSha), "fields cannot overlap");

	constexpr std::array<std::uint8_t, 4> actualSkipInsn{
		0x80, 0x7E, 0x10, 0x00
	};
	constexpr std::array<std::uint8_t, 3> actualShaInsn{
		0x8D, 0x40, 0x1C
	};
	checkLayout(derive(actualSkipInsn, actualShaInsn), Layout{0x10, 0x1C},
	             "current i386 sites derive the live layout");
	// x86-64 beta: movzx eax,byte [rax+0x14] and cmp [rax+0x20],rsi.
	constexpr std::array<std::uint8_t, 4> x64SkipInsn{0x0F, 0xB6, 0x40, 0x14};
	constexpr std::array<std::uint8_t, 4> x64ShaInsn{0x48, 0x39, 0x70, 0x20};
	checkLayout(derive(x64SkipInsn, x64ShaInsn), Layout{0x14, 0x20},
	             "x86-64 sites derive the live layout");
	constexpr std::array<std::uint8_t, 3> x64ShaNoRexW{0x39, 0x70, 0x20};
	check(!derive(x64SkipInsn, x64ShaNoRexW),
	      "a 32-bit compare is not taken for the SHA site");
	constexpr std::array<std::uint8_t, 8> x64SkipDisp32{
		0x41, 0x0F, 0xB6, 0x86, 0x14, 0x01, 0x00, 0x00
	};
	checkLayout(derive(x64SkipDisp32, x64ShaInsn), Layout{0x114, 0x20},
	             "an x86-64 disp32 skip site with REX is decoded");

	constexpr std::array<std::uint8_t, 7> negativeSkip{
		0x80, 0xB8, 0xF0, 0xFF, 0xFF, 0xFF, 0x00
	};
	constexpr std::array<std::uint8_t, 4> negativeDisp8Skip{
		0x80, 0x7E, 0xF0, 0x00
	};
	check(!derive(negativeSkip, shaInsn),
	      "negative 32-bit displacement is rejected");
	check(!derive(negativeDisp8Skip, actualShaInsn),
	      "negative 8-bit displacement is rejected");

	constexpr std::array<std::uint8_t, 7> maximumSkip{
		0x80, 0xB8, 0xFF, 0x0F, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 6> maximumSha{
		0x8D, 0x90, 0xEC, 0x0F, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 6> overBoundarySha{
		0x8D, 0x90, 0xED, 0x0F, 0x00, 0x00
	};
	checkLayout(derive(maximumSkip, shaInsn), Layout{0x0FFF, 0x1C},
	             "maximum accepted skip boundary is safe");
	checkLayout(derive(actualSkipInsn, maximumSha), Layout{0x10, 0x0FEC},
             "maximum accepted SHA boundary is safe");
	check(!derive(actualSkipInsn, overBoundarySha),
	      "field end beyond the boundary is rejected");

	constexpr std::array<std::uint8_t, 7> wrongSkipModrm{
		0x80, 0xB0, 0x10, 0x00, 0x00, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 6> wrongShaSib{
		0x8D, 0x94, 0x90, 0x1C, 0x00, 0x00
	};
	constexpr std::array<std::uint8_t, 2> wrongShaNoDisplacement{
		0x8D, 0x00
	};
	check(!derive(wrongSkipModrm, shaInsn),
	      "wrong CMP ModRM extension is rejected");
	check(!derive(actualSkipInsn, wrongShaSib),
	      "SIB-based SHA reference is rejected");
	check(!derive(actualSkipInsn, wrongShaNoDisplacement),
	      "register-only SHA reference is rejected");

	return failures == 0 ? 0 : 1;
}
