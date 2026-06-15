// Regression test for the MemHlp::patternScan out-of-bounds read that crashed
// Steam at startup on some machines (SIGSEGV in patternScan, before the logger
// is even created -> no ~/.SLSsteam.log). Two distinct bugs are covered:
//
//   1. Segment filter: `seg->prot & LM_PROT_XR` is true for ANY readable
//      region (LM_PROT_XR == X|R), so rw-p / r--p DATA segments were scanned.
//      Those border PROT_NONE guard pages whose placement is ASLR/glibc
//      dependent -> crash on some boots only. Fix: require (prot & XR) == XR.
//
//   2. Inner read bound: `byteAddr > itm.second` was off-by-one and let the
//      compare read the byte at itm.second (one past the segment), which is
//      the first byte of the next (possibly unmapped) page. Fix: only scan
//      while `cur + bytes.size() <= itm.second`.
//
// The buggy logic is run in a forked child to prove it really faults at the
// boundary; the fixed logic runs in-process and must complete and match.
//
// Build: g++ -std=c++20 tools/test_patternscan.cpp -o /tmp/t && /tmp/t

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using lm_address_t = uintptr_t;
using lm_byte_t    = uint8_t;

// libmem protection bit values (from include/libmem/libmem.h)
enum { LM_PROT_R = 1 << 0, LM_PROT_W = 1 << 1, LM_PROT_X = 1 << 2,
       LM_PROT_XR = LM_PROT_X | LM_PROT_R };

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); g_fail++; } } while (0)

// patternToBytes: verbatim from memhlp.cpp
static std::vector<int16_t> patternToBytes(const char* pattern)
{
    auto bytes = std::vector<int16_t>();
    char* start = const_cast<char*>(pattern);
    char* end   = start + strlen(pattern);
    while (start < end) {
        if (*start == '?')      bytes.emplace_back(-1);
        else if (*start != ' ') bytes.emplace_back(std::strtoul(start, &start, 16));
        start++;
    }
    return bytes;
}

// OLD (buggy) inner scan — kept only to prove the boundary is dangerous.
static void scan_old(const std::vector<int16_t>& bytes, lm_address_t b, lm_address_t e)
{
    for (lm_address_t cur = b; cur < e; cur++)
        for (unsigned i = 0; i < bytes.size(); i++) {
            if (bytes.at(i) == -1) continue;
            lm_address_t a = cur + i;
            if (a > e) break;                                   // off-by-one
            if (*reinterpret_cast<lm_byte_t*>(a) != bytes.at(i)) break;
        }
}

// NEW (fixed) inner scan — matches src/memhlp.cpp after the fix.
static lm_address_t scan_fixed(const std::vector<int16_t>& bytes, lm_address_t b, lm_address_t e)
{
    lm_address_t addr = (lm_address_t)-1;
    const size_t n = bytes.size();
    if (n == 0 || e - b < n) return addr;
    for (lm_address_t cur = b; cur + n <= e; cur++) {
        bool found = true;
        for (unsigned i = 0; i < n; i++) {
            if (bytes.at(i) == -1) continue;
            if (*reinterpret_cast<lm_byte_t*>(cur + i) != bytes.at(i)) { found = false; break; }
        }
        if (found) addr = cur;
    }
    return addr;
}

// [ readable page ][ PROT_NONE guard page ] — the exact crash shape.
struct Layout { uint8_t* p; size_t pg; lm_address_t base, end; };
static Layout make_layout()
{
    size_t pg = sysconf(_SC_PAGESIZE);
    uint8_t* p = (uint8_t*)mmap(nullptr, 2*pg, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    memset(p, 0xCC, pg);
    if (mprotect(p + pg, pg, PROT_NONE) != 0) { perror("mprotect"); exit(2); }
    return Layout{ p, pg, (lm_address_t)p, (lm_address_t)(p + pg) };
}

template <class F> static int child_signal(F fn)
{
    pid_t pid = fork();
    if (pid == 0) { fn(); _exit(0); }
    int st = 0; waitpid(pid, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}

int main()
{
    printf("[1] segment filter rejects data regions (fix requires XR == X|R)\n");
    CHECK(((LM_PROT_R | LM_PROT_X) & LM_PROT_XR) == LM_PROT_XR, "r-xp (code) is scanned");
    CHECK(((LM_PROT_R | LM_PROT_W) & LM_PROT_XR) != LM_PROT_XR, "rw-p (data) is NOT scanned");
    CHECK((LM_PROT_R & LM_PROT_XR) != LM_PROT_XR,               "r--p (data) is NOT scanned");
    CHECK((0u & LM_PROT_XR) != LM_PROT_XR,                      "---p (guard) is NOT scanned");

    auto L = make_layout();
    L.p[L.pg - 1] = 0xAB;                       // last readable byte = pattern[0]
    std::string pat; for (int i = 0; i < 52; i++) pat += (i ? " AB" : "AB");
    auto bytes = patternToBytes(pat.c_str());

    printf("[2] boundary safety at a PROT_NONE guard page (pattern %zu bytes)\n", bytes.size());
    int sigOld = child_signal([&]{ scan_old(bytes, L.base, L.end); });
    CHECK(sigOld == SIGSEGV, "old logic faults at the boundary (confirms the hazard)");

    int sigNew = child_signal([&]{ scan_fixed(bytes, L.base, L.end); });
    CHECK(sigNew == 0, "fixed logic does NOT read into the guard page");

    printf("[3] fixed scan still finds a pattern fully inside the segment\n");
    uint8_t needle[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    memcpy(L.p + 100, needle, 4);
    auto nb = patternToBytes("DE AD BE EF");
    lm_address_t hit = scan_fixed(nb, L.base, L.end);
    CHECK(hit == L.base + 100, "match found at the expected offset");

    auto wb = patternToBytes("DE ? BE EF");     // wildcard still works
    CHECK(scan_fixed(wb, L.base, L.end) == L.base + 100, "wildcard match works");

    printf(g_fail ? "\nFAILED (%d)\n" : "\nOK\n", g_fail);
    return g_fail ? 1 : 0;
}
