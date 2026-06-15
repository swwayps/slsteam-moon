#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-setup-steamless.sh — unit test for setup.sh::install_steamstub.
#
# Asserts the installer (1) prefers the verbatim-bundled Steamless kit
# over a network download, and (2) warns LOUDLY when no kit ends up
# installed (the silent-failure regression behind "load error 6").
#
# The real function is extracted from setup.sh and run against stub
# log_* helpers and a fake repo tree, so we test shipped code, not a copy.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$REPO_ROOT/setup.sh"

PASS=0; FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

# Extract the install_steamstub function body verbatim from setup.sh.
FN="$(awk '/^install_steamstub\(\)/{f=1} f{print} f&&/^}/{exit}' "$SETUP")"
[ -n "$FN" ] || { echo "could not extract install_steamstub from setup.sh" >&2; exit 1; }

# Build a harness that defines stub helpers, the extracted function, then
# invokes it.  WARN lines are written to $WARN_LOG so we can assert on them.
run_case() {
    local fakeroot="$1" target="$2"
    (
        cd "$fakeroot"
        log_info()    { echo "[info] $*"; }
        log_warn()    { echo "[warn] $*" >> "$WARN_LOG"; echo "[warn] $*"; }
        log_success() { echo "[ok] $*"; }
        eval "$FN"
        install_steamstub "$target"
    )
}

make_fakeroot() {
    local root; root="$(mktemp -d)"
    mkdir -p "$root/tools/steamstub-bypass"
    for s in run-steamless.sh install-steamless.sh scan-all.sh; do
        # install-steamless.sh stub must NOT touch the network; if the
        # fallback runs it, it just no-ops (leaving the kit absent).
        echo '#!/usr/bin/env bash' > "$root/tools/steamstub-bypass/$s"
        echo 'exit 0' >> "$root/tools/steamstub-bypass/$s"
        chmod +x "$root/tools/steamstub-bypass/$s"
    done
    echo "$root"
}

echo "== test-setup-steamless =="

# Case 1: bundled kit present -> it is copied into the target, no warn.
R1="$(make_fakeroot)"
mkdir -p "$R1/tools/steamless-bin/Plugins"
printf 'cli'  > "$R1/tools/steamless-bin/Steamless.CLI.exe"
printf 'api'  > "$R1/tools/steamless-bin/Plugins/Steamless.API.dll"
T1="$(mktemp -d)"; WARN_LOG="$(mktemp)"
run_case "$R1" "$T1" >/dev/null 2>&1
[ -f "$T1/steamless-bin/Steamless.CLI.exe" ] \
    && ok "bundled CLI copied into target" \
    || bad "bundled CLI not copied into target"
[ -f "$T1/steamless-bin/Plugins/Steamless.API.dll" ] \
    && ok "bundled Plugins copied into target" \
    || bad "bundled Plugins not copied into target"
[ ! -s "$WARN_LOG" ] \
    && ok "no warning when kit is present" \
    || bad "unexpected warning when kit present: $(cat "$WARN_LOG")"
rm -rf "$R1" "$T1" "$WARN_LOG"

# Case 2: no bundle, download no-ops -> kit absent -> loud warning.
R2="$(make_fakeroot)"   # no tools/steamless-bin
T2="$(mktemp -d)"; WARN_LOG="$(mktemp)"
run_case "$R2" "$T2" >/dev/null 2>&1
[ ! -f "$T2/steamless-bin/Steamless.CLI.exe" ] \
    && ok "no CLI installed when kit unavailable (precondition)" \
    || bad "unexpected CLI present"
if grep -qi "load error 6" "$WARN_LOG"; then
    ok "warns loudly when Steamless kit is missing"
else
    bad "no loud warning emitted when kit missing: $(cat "$WARN_LOG")"
fi
rm -rf "$R2" "$T2" "$WARN_LOG"

echo "== test-setup-steamless: install_steamstub checks done =="

echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
