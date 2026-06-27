#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-steamstub-detect.sh — unit test for run-steamless.sh's SteamStub
# marker detection (step 2).
#
# Regression guard for the Fedora "rc=127 / could not remove DRM" bug:
# the detector used to shell out to `xxd`, which is absent from a base
# Fedora install.  Under `set -euo pipefail` the missing tool aborted
# the whole helper with 127 before any signature was even examined, so
# DRM removal silently failed for every SteamStub title on such distros.
#
# These tests assert the detector (1) depends only on tooling present on
# a minimal install (no `xxd`), and (2) still classifies v2 / v3 / plain
# binaries correctly.  We drive the real shipped script, not a copy.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HELPER="$REPO_ROOT/tools/steamstub-bypass/run-steamless.sh"

PASS=0; FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

[ -f "$HELPER" ] || { echo "helper not found: $HELPER" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ── craft synthetic binaries ────────────────────────────────────────────
# A minimal but well-formed-enough PE so the .bind section walker runs.
#   kind=plain  -> .text/.data sections, no VLV magic    (expect: skip/2)
#   kind=v3     -> a section literally named ".bind"      (expect: proceed)
#   kind=v2     -> 'VLV\0' magic at file offset 0x40      (expect: proceed)
python3 - "$WORK" <<'PY'
import struct, sys, os
work = sys.argv[1]

def make_pe(sections):
    e_lfanew = 0x80
    buf = bytearray(0x400)
    buf[0:2] = b'MZ'
    struct.pack_into('<I', buf, 0x3c, e_lfanew)
    buf[e_lfanew:e_lfanew+4] = b'PE\x00\x00'
    nsec = len(sections)
    optsize = 0
    struct.pack_into('<H', buf, e_lfanew + 6, nsec)       # NumberOfSections
    struct.pack_into('<H', buf, e_lfanew + 0x14, optsize) # SizeOfOptionalHeader
    sec_off = e_lfanew + 0x18 + optsize
    for i, name in enumerate(sections):
        nm = name.encode().ljust(8, b'\x00')
        buf[sec_off + i*40 : sec_off + i*40 + 8] = nm
    return bytes(buf)

plain = make_pe(['.text', '.data'])
v3    = make_pe(['.text', '.bind'])
v2    = bytearray(make_pe(['.text', '.data']))
v2[0x40:0x44] = b'VLV\x00'   # SteamStub v2 magic at 0x40

for name, data in (('plain.exe', plain), ('v3.exe', v3), ('v2.exe', bytes(v2))):
    with open(os.path.join(work, name), 'wb') as f:
        f.write(data)
PY

# ── poison `xxd` so any lingering use is caught, and run with QUIET ──────
# A fake xxd that fails loudly sits first on PATH.  If the detector still
# works, it provably does not depend on xxd.
POISON="$WORK/poison"
mkdir -p "$POISON"
cat > "$POISON/xxd" <<'SH'
#!/usr/bin/env bash
echo "POISONED-XXD-CALLED" >&2
exit 99
SH
chmod +x "$POISON/xxd"

# Detection happens before Steamless.CLI.exe is resolved; pointing
# STEAMLESS_HOME at an empty dir makes a *detected* stub die with rc=4
# (CLI not found) right after detection, with no Wine involved.  A
# *non*-stub exits 2 during detection.  So:
#   plain -> 2   (detector said "nothing to do")
#   v2/v3 -> 4   (detector proceeded, then CLI-not-found)
#   never -> 127 (the old xxd-missing failure)
run_detect() {
    local exe="$1"
    PATH="$POISON:$PATH" \
    STEAMLESS_HOME="$WORK/no-such-steamless-home" \
    QUIET=1 \
        bash "$HELPER" "$exe" >/dev/null 2>"$WORK/err.log"
    echo $?
}

echo "== test-steamstub-detect =="

# 1. static guard: the shipped detector must not invoke xxd at all.
# Strip full-line comments first so the historical note documenting the
# removed `xxd` dependency doesn't trip the check.
if grep -vE '^[[:space:]]*#' "$HELPER" | grep -qw xxd; then
    bad "run-steamless.sh still references xxd (reintroduces the Fedora rc=127 bug)"
else
    ok "detector does not depend on xxd"
fi

# 2. plain PE -> nothing to do (exit 2), no 127.
rc="$(run_detect "$WORK/plain.exe")"
[ "$rc" = "2" ] && ok "plain PE classified as not-wrapped (exit 2)" \
                || bad "plain PE expected exit 2, got $rc"

# 3. v3 (.bind) -> detection proceeds (exit 4 = CLI missing, not 2/127).
rc="$(run_detect "$WORK/v3.exe")"
[ "$rc" = "4" ] && ok "v3 (.bind) detected, proceeds past detection (exit 4)" \
                || bad "v3 expected exit 4 (detected, CLI missing), got $rc"

# 4. v2 (VLV) -> detection proceeds (exit 4), independent of xxd.
rc="$(run_detect "$WORK/v2.exe")"
[ "$rc" = "4" ] && ok "v2 (VLV) detected, proceeds past detection (exit 4)" \
                || bad "v2 expected exit 4 (detected, CLI missing), got $rc"

# 5. the poison xxd must never have been called.
if grep -q "POISONED-XXD-CALLED" "$WORK/err.log" 2>/dev/null; then
    bad "detector invoked xxd (poison shim was hit)"
else
    ok "xxd shim never invoked across all cases"
fi

echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
