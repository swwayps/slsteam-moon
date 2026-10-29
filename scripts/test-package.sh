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
BACKUP_DIR="$(mktemp -d)"
BACKED_UP=()
for f in bin/SLSsteam.so bin/pattern-refresh; do
    mkdir -p bin
    if [ -e "$f" ]; then
        cp -a "$f" "$BACKUP_DIR/$(basename "$f")"
        BACKED_UP+=("$f")
    fi
    printf 'stub' > "$f"
    [ "$f" != bin/pattern-refresh ] || chmod +x "$f"
    STUBBED+=("$f")
done

ABI_TMP="$(mktemp -d)"
ABI_READELF="$ABI_TMP/readelf"
cat > "$ABI_READELF" <<'READELF'
#!/bin/sh
case "$*" in
    *--version-info*)
        version="${TEST_GLIBC_VERSION:-2.34}"
        case "$*" in
            *bin/pattern-refresh*)
                version="${TEST_PATTERN_GLIBC_VERSION:-$version}"
                ;;
        esac
        printf '  0x0010:   Name: GLIBC_%s  Flags: none  Version: 2\n' \
            "$version"
        ;;
    *-h*)
        printf '  Class:                             %s\n' "${TEST_ELF_CLASS:-ELF64}"
        printf '  Machine:                           Advanced Micro Devices X86-64\n'
        ;;
esac
READELF
chmod +x "$ABI_READELF"
export READELF="$ABI_READELF" TEST_GLIBC_VERSION=2.34

if [ -x scripts/check-abi.sh ]; then
    READELF="$ABI_READELF" TEST_GLIBC_VERSION=2.34 \
        scripts/check-abi.sh bin/pattern-refresh >/dev/null 2>&1 \
        && ok "ABI gate accepts GLIBC_2.34" \
        || bad "ABI gate rejected GLIBC_2.34"
    if READELF="$ABI_READELF" TEST_GLIBC_VERSION=2.35 \
        scripts/check-abi.sh bin/pattern-refresh >/dev/null 2>&1; then
        bad "ABI gate accepted GLIBC_2.35"
    else
        ok "ABI gate rejects symbols newer than GLIBC_2.34"
    fi
    READELF="$ABI_READELF" TEST_GLIBC_VERSION=2.34 \
        scripts/check-abi.sh bin/SLSsteam.so --x86-64 >/dev/null 2>&1 \
        && ok "SLS ABI gate accepts ELF64 GLIBC_2.34" \
        || bad "SLS ABI gate rejected ELF64 GLIBC_2.34"
    if READELF="$ABI_READELF" TEST_GLIBC_VERSION=2.35 \
        scripts/check-abi.sh bin/SLSsteam.so --x86-64 >/dev/null 2>&1; then
        bad "SLS ABI gate accepted GLIBC_2.35"
    else
        ok "SLS ABI gate rejects symbols newer than GLIBC_2.34"
    fi
    if READELF="$ABI_READELF" TEST_ELF_CLASS=ELF32 \
        scripts/check-abi.sh bin/SLSsteam.so --x86-64 >/dev/null 2>&1; then
        bad "SLS ABI gate accepted an ELF32 binary"
    else
        ok "SLS ABI gate rejects an ELF32 binary"
    fi
else
    bad "scripts/check-abi.sh is missing"
fi

STALE_VERSION="selftest-stale-$$"
TEST_VERSION="selftest-$$"
INCOMPATIBLE_VERSION="selftest-incompatible-$$"
cleanup() {
    rm -rf "dist/slsteam-moon-${STALE_VERSION}" \
           "dist/slsteam-moon-linux-${STALE_VERSION}.zip" \
           "dist/slsteam-moon-${TEST_VERSION}" \
           "dist/slsteam-moon-linux-${TEST_VERSION}.zip" \
           "dist/slsteam-moon-${INCOMPATIBLE_VERSION}" \
           "dist/slsteam-moon-linux-${INCOMPATIBLE_VERSION}.zip" "$ABI_TMP"
    for f in "${STUBBED[@]}"; do rm -f "$f"; done
    for f in "${BACKED_UP[@]}"; do cp -a "$BACKUP_DIR/$(basename "$f")" "$f"; done
    rm -rf "$BACKUP_DIR"
}
trap cleanup EXIT

# Non-empty but stale binaries must not be accepted as a release input. This
# catches the ignored-bin failure mode where package.sh silently copies a
# previous build after source files have changed.
touch -d '@1' bin/SLSsteam.so bin/pattern-refresh
if scripts/package.sh --version "$STALE_VERSION" >/dev/null 2>&1; then
    bad "package.sh accepted stale binaries"
else
    ok "package.sh rejects stale binaries"
fi

# Make the test binaries current for the positive packaging assertion below.
touch bin/SLSsteam.so bin/pattern-refresh

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
        echo "$listing" | grep -q "tools/launcher-shim.lib.sh" \
            && ok "zip bundles launcher shim library" \
            || bad "zip is missing launcher shim library"
        echo "$listing" | grep -q "ensure-desktop-coverage.sh" \
            && ok "zip bundles desktop coverage CLI" \
            || bad "zip is missing desktop coverage CLI"
        echo "$listing" | grep -q "bin/pattern-refresh" \
            && ok "zip bundles signed pattern refresh helper" \
            || bad "zip is missing signed pattern refresh helper"
        # SLSsteam must be the only audit module (see setup.sh wrapper).
        echo "$listing" | grep -q "library-inject" \
            && bad "zip still ships library-inject.so" \
            || ok "zip ships no second audit module"
    else
        bad "package.sh did not produce $ZIP"
    fi
else
    bad "package.sh exited non-zero"
fi

# The integrated package path must reject an incompatible SLSsteam ABI before
# staging a release zip, not merely pass the helper's standalone unit check.
if READELF="$ABI_READELF" TEST_GLIBC_VERSION=2.35 \
    TEST_PATTERN_GLIBC_VERSION=2.34 \
    scripts/package.sh --version "$INCOMPATIBLE_VERSION" >/dev/null 2>&1; then
    bad "package.sh accepted incompatible SLSsteam ABI"
elif [ -e "dist/slsteam-moon-${INCOMPATIBLE_VERSION}" ] \
     || [ -e "dist/slsteam-moon-linux-${INCOMPATIBLE_VERSION}.zip" ]; then
    bad "package.sh staged an incompatible SLSsteam release"
else
    ok "package.sh rejects incompatible SLSsteam ABI before staging"
fi

# A file with the target name must not suppress the regression target. The
# target is intentionally phony so a checkout artifact cannot make CI skip it.
PHONY_PROBE="test-depotkey-scope"
PHONY_BACKUP=""
if [ -e "$PHONY_PROBE" ]; then
    PHONY_BACKUP="$(mktemp)"
    cp -a "$PHONY_PROBE" "$PHONY_BACKUP"
fi
touch "$PHONY_PROBE"
if make -n "$PHONY_PROBE" 2>/dev/null | grep -q "tools/test_depotkey_scope.cpp"; then
    ok "test-depotkey-scope remains phony when a file exists"
else
    bad "test-depotkey-scope is suppressed by a same-named file"
fi
rm -f "$PHONY_PROBE"
if [ -n "$PHONY_BACKUP" ]; then
    cp -a "$PHONY_BACKUP" "$PHONY_PROBE"
    rm -f "$PHONY_BACKUP"
fi

echo "== test-package: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
