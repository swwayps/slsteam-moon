#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-setup-desktop.sh — unit test for setup.sh::patch_desktop_file.
#
# Guards the .desktop launcher rewrite. The original implementation only
# rewrote three hard-coded launcher paths (/usr/games|bin|local/bin/steam)
# yet TAGGED the file as patched whenever is_real_steam_desktop matched a
# broader set of Exec= forms. Result on non-standard installs (e.g. Steam at
# /opt/steam/steam or /usr/lib/steam/steam): the file was stamped
# "X-SLSteamMoon-Patched" but its Exec= still pointed at the distro launcher,
# so launching from the menu bypassed our wrapper — and because the tag was
# present, every re-run skipped the file forever ("redirects to steam default,
# console works"). This test asserts:
#   1. the launcher token is rewritten to our wrapper for ANY launcher path,
#   2. env-prefixed and Desktop Action Exec= lines are handled, not mangled,
#   3. the patched-marker is only stamped when an Exec= was actually rewritten.
#
# The real functions are extracted from setup.sh and run against stub helpers,
# so we test shipped code, not a copy.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$REPO_ROOT/setup.sh"

PASS=0; FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

# Extract the functions under test verbatim from setup.sh.
extract_fn() { awk "/^$1\(\)/{f=1} f{print} f&&/^}/{exit}" "$SETUP"; }
FN_PATCH="$(extract_fn patch_desktop_file)"
FN_REAL="$(extract_fn is_real_steam_desktop)"
FN_PATCHED="$(extract_fn is_patched_desktop)"
[ -n "$FN_PATCH" ] || { echo "could not extract patch_desktop_file from setup.sh" >&2; exit 1; }

SLSM_TAG="X-SLSteamMoon-Patched=true"
WRAPPER_REL=".local/share/SLSsteam/path/steam"

# Run patch_desktop_file($file) inside a harness with a known $HOME so the
# wrapper path ("$SLSDIR/path/steam") is deterministic. Echoes the function's
# exit code on the last line as "RC=<n>".
run_patch() {
    local file="$1" fakehome="$2"
    (
        export HOME="$fakehome"
        SLSDIR="$HOME/.local/share/SLSsteam"
        SLSM_TAG="X-SLSteamMoon-Patched=true"
        log_info()    { :; }
        log_warn()    { :; }
        log_success() { :; }
        eval "$FN_REAL"
        eval "$FN_PATCHED"
        eval "$FN_PATCH"
        patch_desktop_file "$file"
        echo "RC=$?"
    )
}

make_desktop() {
    # $1 = target file, remaining args = Exec= lines (in order)
    local f="$1"; shift
    {
        echo "[Desktop Entry]"
        echo "Name=Steam"
        echo "Type=Application"
        local e
        for e in "$@"; do echo "$e"; done
    } > "$f"
}

primary_exec() { grep -m1 '^Exec=' "$1"; }
tag_count()    { grep -c "^$SLSM_TAG\$" "$1"; }

echo "== test-setup-desktop =="

FH="$(mktemp -d)"; WRAP="$FH/$WRAPPER_REL"

# --- Case 1: standard /usr/bin/steam (regression baseline) ------------------
D="$(mktemp)"; make_desktop "$D" "Exec=/usr/bin/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
    && ok "standard /usr/bin/steam rewritten to wrapper" \
    || bad "standard path not rewritten: $(primary_exec "$D")"
[ "$(tag_count "$D")" = "1" ] && ok "standard: tag present once" || bad "standard: tag count $(tag_count "$D")"
rm -f "$D" "$D.slssteam-backup"

# --- Case 2: non-standard /opt/steam/steam (the reported gap) ---------------
D="$(mktemp)"; make_desktop "$D" "Exec=/opt/steam/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
    && ok "/opt/steam/steam rewritten to wrapper" \
    || bad "/opt/steam/steam NOT rewritten: $(primary_exec "$D")"
[ "$(tag_count "$D")" = "1" ] && ok "/opt: tag present once" || bad "/opt: tag count $(tag_count "$D")"
rm -f "$D" "$D.slssteam-backup"

# --- Case 3: /usr/lib/steam/steam ------------------------------------------
D="$(mktemp)"; make_desktop "$D" "Exec=/usr/lib/steam/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
    && ok "/usr/lib/steam/steam rewritten to wrapper" \
    || bad "/usr/lib/steam/steam NOT rewritten: $(primary_exec "$D")"
rm -f "$D" "$D.slssteam-backup"

# --- Case 4: bare 'steam' token --------------------------------------------
D="$(mktemp)"; make_desktop "$D" "Exec=steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
    && ok "bare 'steam' rewritten to wrapper" \
    || bad "bare 'steam' NOT rewritten: $(primary_exec "$D")"
rm -f "$D" "$D.slssteam-backup"

# --- Case 5: env-prefixed Exec (launcher token, not 'env', replaced) --------
D="$(mktemp)"; make_desktop "$D" "Exec=env GSETTINGS_BACKEND=memory /usr/bin/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=env GSETTINGS_BACKEND=memory $WRAP %U" ] \
    && ok "env-prefixed Exec keeps env, rewrites launcher" \
    || bad "env-prefixed Exec wrong: $(primary_exec "$D")"
rm -f "$D" "$D.slssteam-backup"

# --- Case 6: Desktop Action lines are not mangled ---------------------------
D="$(mktemp)"
make_desktop "$D" "Exec=/usr/bin/steam %U"
{ echo ""; echo "[Desktop Action Store]"; echo "Name=Store"; echo "Exec=steam steam://store"; } >> "$D"
run_patch "$D" "$FH" >/dev/null
if grep -q "^Exec=$WRAP steam://store\$" "$D"; then
    ok "Desktop Action rewritten cleanly to '<wrapper> steam://store'"
else
    bad "Desktop Action mangled: $(grep -m1 'steam://store' "$D")"
fi
rm -f "$D" "$D.slssteam-backup"

# --- Case 7: never tag a file with no rewritable Exec -----------------------
D="$(mktemp)"
{ echo "[Desktop Entry]"; echo "Name=Steam"; echo "Type=Application"; } > "$D"  # no Exec=
run_patch "$D" "$FH" >/dev/null
rc_line="$(run_patch "$D" "$FH" | tail -n1)"
[ "$(tag_count "$D")" = "0" ] \
    && ok "no Exec= line -> file is NOT tagged" \
    || bad "tagged a file with no rewritable Exec (would lock out future runs)"
rm -f "$D" "$D.slssteam-backup"

rm -rf "$FH"
echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
