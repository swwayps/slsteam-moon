// Proves the process-wide one-shot guard used by load(): two independent
// SLSsteam.so instances live in the same Steam process (one per LD_AUDIT
// link-map namespace), each with its own `static` flag, so a per-.so static
// cannot coordinate them. The guard claims a per-pid advisory file lock; the
// kernel fd/inode lock IS shared across both instances (the fd table is
// per-process), so exactly one instance wins.
//
// This test simulates the two instances as two independent open()+flock()
// sequences in one process (exactly what the two .so copies do) and asserts
// only the first acquires; it also checks the lock is reusable after release
// (no stale-lock hazard on pid reuse).
//
// Build: g++ -std=c++20 tools/test_loadlock.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c,m) do{ if(c){printf("  PASS: %s\n",m);} else {printf("  FAIL: %s\n",m); g_fail++;} }while(0)

// Mirrors the guard in src/main.cpp::load(). Returns the held fd (>=0) if this
// caller won the one-shot, or -1 if another holder already claimed it.
static int claim_oneshot()
{
    char path[64];
    std::snprintf(path, sizeof(path), "/tmp/.slssteam.load.%d", getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) return -2;                       // open failure -> caller falls through
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return -1; } // already held
    return fd;                                   // won; keep fd open to hold the lock
}

int main()
{
    char path[64];
    std::snprintf(path, sizeof(path), "/tmp/.slssteam.load.%d", getpid());
    unlink(path); // clean start

    printf("[1] two instances in one process: only the first hooks\n");
    int a = claim_oneshot();   // instance A (e.g. audit-namespace copy)
    int b = claim_oneshot();   // instance B (e.g. base-namespace copy)
    CHECK(a >= 0,  "instance A claims the one-shot (would run hooking)");
    CHECK(b == -1, "instance B is blocked (would skip -> no re-scan of hooked code)");

    printf("[2] lock auto-releases (no stale-lock / pid-reuse hazard)\n");
    if (a >= 0) close(a);      // simulate process exit releasing the lock
    int c = claim_oneshot();   // a later process with the same pid
    CHECK(c >= 0, "after release, a fresh process can claim again");
    if (c >= 0) close(c);

    unlink(path);
    printf(g_fail ? "\nFAILED (%d)\n" : "\nOK — cross-instance one-shot holds\n", g_fail);
    return g_fail ? 1 : 0;
}
