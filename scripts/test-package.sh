#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-package.sh — packaging self-test.
#
# Verifies that a release zip produced by package.sh is self-contained:
# it MUST bundle the Steamless binary kit so SteamStub DRM removal works
# offline, with no install-time download.  This is the regression guard
# for the "Application load error 6" class of failures, where the kit was
# fetched from GitHub at install time and a silent download failure left
# users with DRM-locked games.
#
# Usage:  scripts/test-package.sh
# Exit:   0 all assertions pass; non-zero on first failure.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

PASS=0
FAIL=0
ok()   { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

echo "== test-package: bundled Steamless kit =="

# ── 1. the kit is committed in the source tree ──────────────────────────
KIT="tools/steamless-bin"
[ -f "$KIT/Steamless.CLI.exe" ] \
    && ok "$KIT/Steamless.CLI.exe present in tree" \
    || bad "$KIT/Steamless.CLI.exe missing from tree"
[ -f "$KIT/Plugins/Steamless.API.dll" ] \
    && ok "$KIT/Plugins/Steamless.API.dll present in tree" \
    || bad "$KIT/Plugins/Steamless.API.dll missing from tree"
# At least one unpacker variant plugin must ship or no DRM can be removed.
if ls "$KIT"/Plugins/Steamless.Unpacker.Variant*.dll >/dev/null 2>&1; then
    ok "unpacker variant plugins present"
else
    bad "no Steamless.Unpacker.Variant*.dll in $KIT/Plugins"
fi

# ── 2. the produced release zip contains the kit ────────────────────────
# Use throwaway stub .so files if a real build isn't present, so the test
# stays fast and build-independent.  Restore the tree afterwards.
STUBBED=()
for f in bin/SLSsteam.so bin/library-inject.so; do
    if [ ! -s "$f" ]; then
        mkdir -p bin
        printf 'stub' > "$f"
        STUBBED+=("$f")
    fi
done

TEST_VERSION="selftest-$$"
cleanup() {
    rm -rf "dist/slsteam-moon-${TEST_VERSION}" \
           "dist/slsteam-moon-linux-${TEST_VERSION}.zip"
    for f in "${STUBBED[@]}"; do rm -f "$f"; done
}
trap cleanup EXIT

if scripts/package.sh --version "$TEST_VERSION" >/dev/null 2>&1; then
    ZIP="dist/slsteam-moon-linux-${TEST_VERSION}.zip"
    if [ -f "$ZIP" ]; then
        listing="$(unzip -l "$ZIP" 2>/dev/null)"
        echo "$listing" | grep -q "steamless-bin/Steamless.CLI.exe" \
            && ok "zip bundles steamless-bin/Steamless.CLI.exe" \
            || bad "zip is missing steamless-bin/Steamless.CLI.exe"
        echo "$listing" | grep -q "steamless-bin/Plugins/Steamless.API.dll" \
            && ok "zip bundles steamless-bin/Plugins/Steamless.API.dll" \
            || bad "zip is missing steamless-bin/Plugins/Steamless.API.dll"
        echo "$listing" | grep -q "tools/desktop-coverage.lib.sh" \
            && ok "zip bundles desktop coverage library" \
            || bad "zip is missing desktop coverage library"
        echo "$listing" | grep -q "tools/desktop-guardian-units.lib.sh" \
            && ok "zip bundles desktop guardian unit library" \
            || bad "zip is missing desktop guardian unit library"
        echo "$listing" | grep -q "ensure-desktop-coverage.sh" \
            && ok "zip bundles desktop coverage CLI" \
            || bad "zip is missing desktop coverage CLI"
    else
        bad "package.sh did not produce $ZIP"
    fi
else
    bad "package.sh exited non-zero"
fi

echo "== test-package: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
