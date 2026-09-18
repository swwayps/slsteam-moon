// Regression tests for the high-24-bit dispatch-pivot resolver used to locate
// IClientRemoteStorage::RunIPCFrame and IClientUserStats::RunIPCFrame when their
// binary-search median jumps beyond the nearest-root band.  Runs against two
// 32-bit steamclient.so modules (an OLD and a NEW build) supplied as argv[1]
// and argv[2].
//
// WHY THIS EXISTS
// ---------------
// These two interfaces have each moved their dispatch median by ~1.9M between
// builds, far outside the deliberately narrow nearest-root band, so a single
// root literal cannot identify them across builds.  The runtime resolves them
// structurally instead: several of each interface's dispatch message ids keep
// their top 24 bits stable across builds while only the low byte drifts, so a
// set of high-24 pivots identifies exactly one dispatcher on every build with
// no cross-interface collision (the interfaces live in different high-byte
// regions).  This test proves both pivot sets each resolve to EXACTLY ONE, and
// DISTINCT, candidate on both supplied builds.
//
// The pivot literals below MUST mirror src/patterns.cpp
// (autoResolveIpcFrameRoots).
//
// Build + run (from repo root):
//   g++ -std=c++20 -I include tools/test_autorepair_cmpfingerprint.cpp -o /tmp/tacf \
//     && /tmp/tacf /path/to/OLD/steamclient.so /path/to/NEW/steamclient.so

#include "../src/feats/ipcframe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                      \
	do {                                                                      \
		++g_checks;                                                           \
		if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; }        \
	} while (0)

struct TextSection
{
	std::vector<uint8_t> bytes;
	bool ok = false;
};

template <class T> static T rd(const std::vector<uint8_t>& d, size_t off)
{
	T v{}; std::memcpy(&v, d.data() + off, sizeof(T)); return v;
}

static TextSection loadText(const std::string& path)
{
	TextSection ts;
	std::ifstream f(path, std::ios::binary);
	if (!f) return ts;
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
	if (d.size() < 0x34 || std::memcmp(d.data(), "\x7f""ELF", 4) != 0) return ts;

	const uint32_t e_shoff    = rd<uint32_t>(d, 0x20);
	const uint16_t e_shentsz  = rd<uint16_t>(d, 0x2e);
	const uint16_t e_shnum    = rd<uint16_t>(d, 0x30);
	const uint16_t e_shstrndx = rd<uint16_t>(d, 0x32);

	auto shName   = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x00); };
	auto shOffset = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x10); };
	auto shSize   = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x14); };

	const uint32_t strOff = shOffset(e_shstrndx);
	for (int i = 0; i < e_shnum; ++i)
	{
		const char* nm = reinterpret_cast<const char*>(d.data() + strOff + shName(i));
		if (std::strcmp(nm, ".text") == 0)
		{
			const uint32_t off = shOffset(i), sz = shSize(i);
			ts.bytes.assign(d.begin() + off, d.begin() + off + sz);
			ts.ok = true;
			return ts;
		}
	}
	return ts;
}

// High-24 dispatch pivots; MUST mirror src/patterns.cpp.
static constexpr uint32_t kRemoteStoragePivots[] = {
	0x396376, 0x5DB472, 0x84692E, 0x8694E9, 0x8712DD,
	0xC0F5C7, 0xDB2410, 0xE82BD7, 0xF5841E, 0xFD3CA9,
};
static constexpr uint32_t kUserStatsPivots[] = {
	0x84EDDC, 0x85DE33, 0x85E5D6, 0xF7452C,
	0xF991AC, 0xF9B9E3, 0xFF7DFB, 0xFF94F2,
};
static constexpr size_t kPivotMinMatches = 4;

// Resolve a pivot set to a single candidate offset (SIZE_MAX if none/ambiguous).
static size_t resolvePivots(
	const std::vector<IpcFrame::Cand>& cands,
	const uint8_t* text,
	const uint32_t* pivots,
	size_t pivotCount,
	size_t* qualifyingOut)
{
	size_t hit = SIZE_MAX, qualifying = 0;
	for (const auto& cand : cands)
	{
		if (IpcFrame::countHighPivots(
			text + cand.offset, cand.available, pivots, pivotCount) >= kPivotMinMatches)
		{
			hit = &cand - cands.data();
			++qualifying;
		}
	}
	if (qualifyingOut) *qualifyingOut = qualifying;
	return (qualifying == 1) ? hit : SIZE_MAX;
}

static void run(const std::string& label, const TextSection& ts)
{
	if (!ts.ok)
	{
		std::printf("%s: SKIP (.text not loadable)\n", label.c_str());
		return;
	}
	auto cands = IpcFrame::scan(ts.bytes.data(), ts.bytes.size());
	CHECK(cands.size() >= 4, (label + ": generic RunIPCFrame tails found").c_str());

	size_t rsQual = 0, usQual = 0;
	size_t rs = resolvePivots(cands, ts.bytes.data(), kRemoteStoragePivots,
	                          std::size(kRemoteStoragePivots), &rsQual);
	size_t us = resolvePivots(cands, ts.bytes.data(), kUserStatsPivots,
	                          std::size(kUserStatsPivots), &usQual);

	CHECK(rsQual == 1, (label + ": RemoteStorage pivots resolve exactly one candidate").c_str());
	CHECK(usQual == 1, (label + ": UserStats pivots resolve exactly one candidate").c_str());
	if (rs != SIZE_MAX && us != SIZE_MAX)
		CHECK(cands[rs].offset != cands[us].offset,
		      (label + ": RemoteStorage and UserStats resolve distinct functions").c_str());

	if (rs != SIZE_MAX)
		std::printf("%s: RemoteStorage root=0x%08X (qual=%zu)\n",
		            label.c_str(), cands[rs].root, rsQual);
	if (us != SIZE_MAX)
		std::printf("%s: UserStats     root=0x%08X (qual=%zu)\n",
		            label.c_str(), cands[us].root, usQual);
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: %s <old steamclient.so> <new steamclient.so>\n", argv[0]);
		return 1;
	}

	TextSection oldTs = loadText(argv[1]);
	TextSection newTs = loadText(argv[2]);
	CHECK(oldTs.ok && newTs.ok, "both supplied steamclient.so load");

	run("OLD", oldTs);
	run("NEW", newTs);

	if (g_failures == 0)
	{
		std::printf("\ntest_autorepair_cmpfingerprint: ALL PASS (%d checks)\n", g_checks);
		return 0;
	}
	std::printf("\ntest_autorepair_cmpfingerprint: %d/%d CHECK(S) FAILED\n", g_failures, g_checks);
	return 1;
}
