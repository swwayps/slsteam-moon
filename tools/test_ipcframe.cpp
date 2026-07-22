// Unit + robustness tests for the structural IClient*::RunIPCFrame resolver
// (src/feats/ipcframe.hpp).
//
// WHY THIS EXISTS
// ---------------
// Each IClient*::RunIPCFrame dispatches over 32-bit message ids with a
// binary-search tree whose tail is a fixed shape:
//
//     call <read msg id>; mov eax,[ebp+disp]; add esp,0x10; cmp eax,<ROOT>
//   = E8 ?? ?? ?? ??  8B 85 ?? ?? ?? ??  83 C4 10  3D <root:u32>
//
// The old slsteam patterns hard-code <ROOT>. That id drifts whenever Steam
// adds/removes a method on the interface, so every such update broke the load
// and needed a manual re-derivation (see
// .kiro/research/slsteam-pattern-refresh-2026-06-23.md). Instead we locate
// EVERY candidate of the generic shape and pick the one whose root is
// numerically NEAREST a per-interface seed, tolerating the small id drift.
//
// THE DECISIVE TEST
// -----------------
// Seed the resolver with the *pre*-2026-06-23 roots and run it against the
// *post*-update steamclient.so. It must still resolve each interface to its
// new function (whose root differs from the seed). That proves the resolver
// would have auto-healed the 2026-06-23 update with zero code change.
//
// Build (from repo root):
//   g++ -std=c++20 -I include tools/test_ipcframe.cpp -o /tmp/test_ipcframe && \
//     /tmp/test_ipcframe [path/to/steamclient.so]
//
// With no argv the test uses $HOME/.steam/steam/ubuntu12_32/steamclient.so;
// it SKIPS the binary-backed checks (still runs the pure unit tests) when the
// file is absent, so it stays green on a build host without Steam installed.

#include "../src/feats/ipcframe.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// ----- minimal ELF32 .text extractor (mirrors .kiro/research/scan.py) -----
struct TextSection { std::vector<uint8_t> bytes; uint32_t va = 0; bool ok = false; };

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

	const uint32_t e_shoff   = rd<uint32_t>(d, 0x20);
	const uint16_t e_shentsz = rd<uint16_t>(d, 0x2e);
	const uint16_t e_shnum   = rd<uint16_t>(d, 0x30);
	const uint16_t e_shstrndx= rd<uint16_t>(d, 0x32);

	auto shName   = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x00); };
	auto shAddr   = [&](int i){ return rd<uint32_t>(d, e_shoff + i*e_shentsz + 0x0c); };
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
			ts.va = shAddr(i);
			ts.ok = true;
			return ts;
		}
	}
	return ts;
}

// ---------------------------------------------------------------------------

struct IfaceCase
{
	const char* name;
	uint32_t seedRoot;
	uint32_t liveRoot;
	bool fingerprintFallback;
};

// Stale seeds and roots in the 2026-07-21 client update. RemoteStorage's
// dispatch-tree root rotated by ~1.9M, outside the deliberately narrow
// numeric band, so it must take the structural-fingerprint fallback.
static const IfaceCase kCases[] = {
	{ "IClientApps",          0xA6889C37, 0xA6889C36, false },
	{ "IClientAppManager",    0x7A0A85B2, 0x7A0A85B7, false },
	{ "IClientRemoteStorage", 0x872FE86C, 0x8712DD4B, true  },
	{ "IClientUGC",           0x71D20C62, 0x71D20C0C, false },
	{ "IClientUserStats",     0x876D658F, 0x876D658E, false },
};

// Three non-root comparisons that identify IClientRemoteStorage's generated
// dispatch tree across the old and new clients. Each id drifted by <= 3 while
// the median/root changed completely.
static constexpr uint32_t kRemoteStorageFingerprint[] = {
	0x5DB4729A, 0x7F3F5645, 0x84692E78,
};

static void test_resolveConfident_pure()
{
	std::printf("[1] resolveConfident (pure)\n");
	CHECK(IpcFrame::resolveConfident({}, 0x1234, 0x100) == SIZE_MAX, "empty -> SIZE_MAX");

	// Exactly one candidate inside the band -> resolved.
	std::vector<IpcFrame::Cand> one = { { 0x10, 0x00001000 }, { 0x20, 0x00009999 } };
	CHECK(IpcFrame::resolveConfident(one, 0x00001005, 0x100) == 0, "single in-band -> its index");

	// Exact match (distance 0) with the other far away -> resolved.
	CHECK(IpcFrame::resolveConfident(one, 0x00001000, 0x100) == 0, "exact match -> index");

	// Two candidates inside the band -> ambiguous -> refuse (keep embedded).
	std::vector<IpcFrame::Cand> two = { { 0x10, 0x00001000 }, { 0x20, 0x00001040 } };
	CHECK(IpcFrame::resolveConfident(two, 0x00001010, 0x100) == SIZE_MAX, "two in-band -> ambiguous -> SIZE_MAX");

	// Nearest exists but is outside the band (drift too large) -> refuse.
	std::vector<IpcFrame::Cand> far = { { 0x10, 0x00009999 } };
	CHECK(IpcFrame::resolveConfident(far, 0x00001000, 0x100) == SIZE_MAX, "out-of-band nearest -> SIZE_MAX");
}

static void test_pattern_root_roundtrip()
{
	std::printf("[1b] parseTrailingRoot / setTrailingRoot (pure)\n");
	// The trailing "3D b0 b1 b2 b3" encodes the root little-endian.
	const std::string apps = "E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D 37 9C 88 A6";
	CHECK(IpcFrame::parseTrailingRoot(apps) == 0xA6889C37, "parse Apps root LE");

	std::string p = apps;
	IpcFrame::setTrailingRoot(p, 0x71D20C62);
	CHECK(IpcFrame::parseTrailingRoot(p) == 0x71D20C62, "set then parse round-trips");
	CHECK(p.size() >= 11 && p.substr(p.size() - 11) == "62 0C D2 71",
	      "trailing bytes rewritten little-endian, uppercase");
	// The fixed prefix (everything up to the root) is untouched.
	CHECK(p.rfind("E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D ", 0) == 0, "prefix preserved");
}

static void test_scan_synthetic()
{
	std::printf("[2] scan (synthetic buffer)\n");
	// One well-formed dispatch tail with root 0xDEADBEEF embedded in noise.
	std::vector<uint8_t> buf = {
		0x90, 0x90,
		0xE8, 0x11, 0x22, 0x33, 0x44,           // call rel32
		0x8B, 0x85, 0x01, 0x02, 0x03, 0x04,      // mov eax,[ebp+disp]
		0x83, 0xC4, 0x10,                        // add esp,0x10
		0x3D, 0xEF, 0xBE, 0xAD, 0xDE,            // cmp eax,0xDEADBEEF
		0x90, 0x90,
	};
	auto cs = IpcFrame::scan(buf.data(), buf.size());
	CHECK(cs.size() == 1, "exactly one candidate found");
	CHECK(cs.size() == 1 && cs[0].offset == 2, "candidate at the call opcode");
	CHECK(cs.size() == 1 && cs[0].root == 0xDEADBEEF, "root read little-endian");

	std::vector<uint8_t> empty(8, 0x90);
	CHECK(IpcFrame::scan(empty.data(), empty.size()).empty(), "no candidate in noise");
}

static void test_fingerprint_synthetic()
{
	std::printf("[2a] dispatch fingerprint (synthetic buffer)\n");
	std::vector<uint8_t> buf(0x80, 0x90);
	size_t at = 0x20;
	for (uint32_t root : kRemoteStorageFingerprint)
	{
		buf[at++] = 0x3D;
		std::memcpy(buf.data() + at, &root, sizeof(root));
		at += sizeof(root) + 3;
	}
	CHECK(IpcFrame::matchesCmpFingerprint(
		buf.data(), buf.size(), kRemoteStorageFingerprint,
		std::size(kRemoteStorageFingerprint), 4),
		"all fingerprint roots -> match");

	// Losing any independent pivot must reject the candidate.
	buf[0x20] = 0x90;
	CHECK(!IpcFrame::matchesCmpFingerprint(
		buf.data(), buf.size(), kRemoteStorageFingerprint,
		std::size(kRemoteStorageFingerprint), 4),
		"missing fingerprint root -> reject");
}

// Reference scan: the obvious byte-by-byte algorithm with NO memchr seek.
// IpcFrame::scan must return byte-for-byte identical results; this pins the
// memchr optimisation so a future tweak can never silently change behaviour.
static std::vector<IpcFrame::Cand> scanNaive(const uint8_t* code, size_t size)
{
	static const int16_t mask[15] = {
		0xE8, -1, -1, -1, -1, 0x8B, 0x85, -1, -1, -1, -1, 0x83, 0xC4, 0x10, 0x3D };
	std::vector<IpcFrame::Cand> out;
	if (size < 19) return out;
	for (size_t i = 0; i + 19 <= size; ++i)
	{
		bool ok = true;
		for (size_t k = 0; k < 15; ++k)
			if (mask[k] != -1 && code[i + k] != mask[k]) { ok = false; break; }
		if (!ok) continue;
		uint32_t root = (uint32_t)code[i + 15] | (uint32_t)code[i + 16] << 8
		              | (uint32_t)code[i + 17] << 16 | (uint32_t)code[i + 18] << 24;
		out.push_back({ i, root });
	}
	return out;
}

static bool sameCands(const std::vector<IpcFrame::Cand>& a, const std::vector<IpcFrame::Cand>& b)
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].offset != b[i].offset || a[i].root != b[i].root) return false;
	return true;
}

static void test_scan_equivalence()
{
	std::printf("[2b] scan == naive reference (memchr optimisation is behaviour-preserving)\n");
	// Edge cases: E8 at the last valid start, E8 too late to fit a match,
	// back-to-back E8 bytes, E8 inside an otherwise-matching tail.
	std::vector<std::vector<uint8_t>> cases;
	cases.push_back({});                                   // empty
	cases.push_back(std::vector<uint8_t>(18, 0xE8));       // smaller than a match span
	{
		// valid tail whose call rel32 bytes are themselves 0xE8 (overlap bait)
		std::vector<uint8_t> v = {
			0xE8, 0xE8, 0xE8, 0xE8, 0xE8, 0x8B, 0x85, 0, 0, 0, 0,
			0x83, 0xC4, 0x10, 0x3D, 0x11, 0x22, 0x33, 0x44 };
		cases.push_back(v);
	}
	{
		// trailing 0xE8 with not enough room to complete a match
		std::vector<uint8_t> v(40, 0x90);
		v[39] = 0xE8;
		cases.push_back(v);
	}
	for (size_t n = 0; n < cases.size(); ++n)
	{
		const auto& v = cases[n];
		bool eq = sameCands(IpcFrame::scan(v.data(), v.size()), scanNaive(v.data(), v.size()));
		CHECK(eq, (std::string("synthetic case #") + std::to_string(n) + " matches naive").c_str());
	}
}

static void test_binary_robustness(const std::string& path)
{
	std::printf("[3] robustness against the real post-update steamclient.so\n      %s\n", path.c_str());
	TextSection ts = loadText(path);
	if (!ts.ok)
	{
		std::printf("      SKIP: .text not loadable (binary absent on this host)\n");
		return;
	}

	auto cands = IpcFrame::scan(ts.bytes.data(), ts.bytes.size());
	CHECK(cands.size() >= 4, "found the generic RunIPCFrame candidates");

	// The memchr-optimised scan must equal the naive reference over all 33MB
	// of real .text — proves the optimisation changed speed, not behaviour.
	CHECK(sameCands(cands, scanNaive(ts.bytes.data(), ts.bytes.size())),
	      "optimised scan == naive scan over the real .text");

	std::vector<size_t> chosen;
	for (const auto& tc : kCases)
	{
		// Prefer the narrow numeric band. Only RemoteStorage is allowed to use
		// the three-pivot structural fallback, and that fallback must identify
		// exactly one generated dispatcher.
		size_t idx = IpcFrame::resolveConfident(cands, tc.seedRoot, IpcFrame::kMaxRootDrift);
		if (idx == SIZE_MAX && tc.fingerprintFallback)
		{
			size_t hits = 0;
			for (size_t i = 0; i < cands.size(); ++i)
			{
				const auto& cand = cands[i];
				if (IpcFrame::matchesCmpFingerprint(
					ts.bytes.data() + cand.offset, cand.available,
					kRemoteStorageFingerprint, std::size(kRemoteStorageFingerprint), 4))
				{
					idx = i;
					++hits;
				}
			}
			if (hits != 1) idx = SIZE_MAX;
		}
		bool resolved = idx != SIZE_MAX;
		CHECK(resolved, tc.name);
		if (!resolved) continue;

		uint32_t got = cands[idx].root;
		bool right = got == tc.liveRoot;
		if (!right)
			std::printf("      %s: seeded 0x%08X -> got root 0x%08X, expected 0x%08X\n",
			            tc.name, tc.seedRoot, got, tc.liveRoot);
		CHECK(right, (std::string(tc.name) + ": stale seed resolves to the new function").c_str());
		chosen.push_back(cands[idx].offset);
	}

	// All interfaces must map to DISTINCT functions.
	bool distinct = true;
	for (size_t i = 0; i < chosen.size(); ++i)
		for (size_t j = i + 1; j < chosen.size(); ++j)
			if (chosen[i] == chosen[j]) distinct = false;
	CHECK(distinct, "the interfaces resolve to distinct offsets");
}

// ----- general masked-pattern matcher (mirrors MemHlp::patternScan logic) ---
static std::vector<int16_t> parseMasked(const char* p)
{
	std::vector<int16_t> out;
	char* s = const_cast<char*>(p);
	char* e = s + std::strlen(p);
	while (s < e)
	{
		if (*s == '?')      out.push_back(-1);
		else if (*s != ' ') out.push_back(static_cast<int16_t>(std::strtoul(s, &s, 16)));
		++s;
	}
	return out;
}

static size_t countMatches(const uint8_t* code, size_t size, const char* pat)
{
	auto b = parseMasked(pat);
	if (b.empty() || size < b.size()) return 0;
	size_t n = 0;
	for (size_t i = 0; i + b.size() <= size; ++i)
	{
		bool ok = true;
		for (size_t k = 0; k < b.size(); ++k)
			if (b[k] != -1 && code[i + k] != b[k]) { ok = false; break; }
		if (ok) ++n;
	}
	return n;
}

// The two offset-bearing patterns the 2026-06-23 update also broke, in their
// hardened (offset-wildcarded) form. These MUST mirror src/patterns.cpp.
static const char* kRlckWild =
	"75 ? 83 C4 1C 31 C0 5B 5E 5F 5D C3 ? ? ? ? ? 8B 44 24 ? 83 C4 1C 89 F9 89 F2 5B 5E 5F 5D 2D ? ? 00 00";
static const char* kRlckOldExact =  // pre-update: sub eax,0x18d8
	"75 ? 83 C4 1C 31 C0 5B 5E 5F 5D C3 ? ? ? ? ? 8B 44 24 ? 83 C4 1C 89 F9 89 F2 5B 5E 5F 5D 2D D8 18 00 00";
static const char* kNluWild =
	"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC ? ? ? ? 8B 45 08 8B B8 ? ? 00 00 89 9D ? ? FF FF 85 FF";
static const char* kNluOldExact =    // pre-update: mov edi,[eax+0x1b18]
	"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC ? ? ? ? 8B 45 08 8B B8 18 1B 00 00 89 9D ? ? FF FF 85 FF";

static void test_offset_patterns(const std::string& newPath, const std::string& oldPath)
{
	std::printf("[4] offset patterns self-heal via wildcarded offset (RequiresLegacyCDKey, NotifyLicensesUpdated)\n");
	TextSection nw = loadText(newPath);
	TextSection od = loadText(oldPath);
	if (!nw.ok || !od.ok)
	{
		std::printf("      SKIP: need both pre- and post-update steamclient.so\n"
		            "      NEW=%s (%s)  OLD=%s (%s)\n",
		            newPath.c_str(), nw.ok ? "ok" : "absent",
		            oldPath.c_str(), od.ok ? "ok" : "absent");
		return;
	}

	// Wildcarded form resolves uniquely on BOTH builds -> drift-tolerant.
	CHECK(countMatches(nw.bytes.data(), nw.bytes.size(), kRlckWild) == 1, "RLCK wildcard: 1 hit on new");
	CHECK(countMatches(od.bytes.data(), od.bytes.size(), kRlckWild) == 1, "RLCK wildcard: 1 hit on old");
	CHECK(countMatches(nw.bytes.data(), nw.bytes.size(), kNluWild) == 1, "NLU wildcard: 1 hit on new");
	CHECK(countMatches(od.bytes.data(), od.bytes.size(), kNluWild) == 1, "NLU wildcard: 1 hit on old");

	// The pre-update exact offset proves the wildcard is NECESSARY: it matched
	// the old build but the offset drift makes it miss the new one.
	CHECK(countMatches(od.bytes.data(), od.bytes.size(), kRlckOldExact) == 1, "RLCK old-exact: matched old");
	CHECK(countMatches(nw.bytes.data(), nw.bytes.size(), kRlckOldExact) == 0, "RLCK old-exact: misses new (drift)");
	CHECK(countMatches(od.bytes.data(), od.bytes.size(), kNluOldExact) == 1, "NLU old-exact: matched old");
	CHECK(countMatches(nw.bytes.data(), nw.bytes.size(), kNluOldExact) == 0, "NLU old-exact: misses new (drift)");
}

// Required patterns refreshed for the 2026-07-21 client. These mirror
// src/patterns.cpp. RequestInternetServerList masks the allocation size that
// changed 0x350 -> 0x354; AppManager uses its near-entry dispatch tail instead
// of a marker more than 64 KiB into the function.
static const char* kRequestInternetServerList =
	"C7 04 24 ? ? 00 00 E8 ? ? ? ? 5A 89 45 ? 59 FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? FF B6 ? ? ? ? 6A 01";
static const char* kAppManagerDispatch =
	"E8 ? ? ? ? 8B 85 ? ? ? ? 83 C4 10 3D B7 85 0A 7A";

static void test_required_patterns(const std::string& newPath, const std::string& oldPath)
{
	std::printf("[5] refreshed required patterns\n");
	TextSection nw = loadText(newPath);
	TextSection od = loadText(oldPath);
	if (!nw.ok)
	{
		std::printf("      SKIP: current steamclient.so unavailable\n");
		return;
	}

	CHECK(countMatches(nw.bytes.data(), nw.bytes.size(), kRequestInternetServerList) == 1,
	      "RequestInternetServerList: 1 hit on current client");
	CHECK(countMatches(nw.bytes.data(), nw.bytes.size(), kAppManagerDispatch) == 1,
	      "AppManager dispatch tail: 1 hit on current client");
	if (od.ok)
	{
		CHECK(countMatches(od.bytes.data(), od.bytes.size(), kRequestInternetServerList) == 1,
		      "RequestInternetServerList: allocation-size wildcard matches old client");
	}
}

int main(int argc, char** argv)
{
	test_resolveConfident_pure();
	test_pattern_root_roundtrip();
	test_scan_synthetic();
	test_fingerprint_synthetic();
	test_scan_equivalence();

	std::string path;
	if (argc > 1) path = argv[1];
	else { const char* home = std::getenv("HOME");
	       path = std::string(home ? home : "") + "/.steam/steam/ubuntu12_32/steamclient.so"; }
	test_binary_robustness(path);

	// Optional pre-update binary for the offset-pattern differential test.
	// Path via argv[2] or $SLSSTEAM_OLD_STEAMCLIENT; the test skips if absent.
	const char* oldEnv = std::getenv("SLSSTEAM_OLD_STEAMCLIENT");
	std::string oldPath = (argc > 2) ? argv[2] : (oldEnv ? oldEnv : "");
	test_offset_patterns(path, oldPath);
	test_required_patterns(path, oldPath);

	if (g_failures == 0) { std::printf("\ntest_ipcframe: ALL PASS (%d checks)\n", g_checks); return 0; }
	std::printf("\ntest_ipcframe: %d/%d CHECK(S) FAILED\n", g_failures, g_checks);
	return 1;
}
