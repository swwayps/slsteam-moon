#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-setup-desktop.sh — unit test for setup.sh::patch_desktop_file.
#
# Guards the .desktop launcher rewrite against the regressions that left Steam
# launching uninjected:
#   1. Path coverage: the launcher token is rewritten to our wrapper for ANY
#      launcher path (not just the three hard-coded /usr/*/steam), and the
#      patched-marker is stamped ONLY when an Exec= was actually rewritten (a
#      stamped-but-unpatched entry is skipped by is_patched_desktop forever).
#   2. env-prefixed and Desktop Action Exec= lines are handled, not mangled.
#   3. SYMLINK survival: the patched entry MUST end up a regular file, never a
#      symlink into Steam's own copy. The Debian/Mint /usr/games/steam launcher
#      regenerates the user entry and re-points it at the vanilla launcher
#      whenever it is a symlink or missing, but leaves a regular file untouched
#      — so a regular file is what makes the patch survive Steam restarts and
#      self-updates. Patching must also NOT write through the symlink and
#      clobber Steam's upstream copy.
#
# The real function is extracted from setup.sh and run against stub helpers, so
# we test shipped code, not a copy.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$REPO_ROOT/setup.sh"

PASS=0; FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

extract_fn() { awk "/^$1\(\)/{f=1} f{print} f&&/^}/{exit}" "$SETUP"; }
FN_PATCH="$(extract_fn patch_desktop_file)"
[ -n "$FN_PATCH" ] || { echo "could not extract patch_desktop_file from setup.sh" >&2; exit 1; }

SLSM_TAG="X-SLSteamMoon-Patched=true"
WRAPPER_REL=".local/share/SLSsteam/path/steam"

# Run patch_desktop_file($file) with a known $HOME so the wrapper path
# ("$SLSDIR/path/steam") is deterministic.
run_patch() {
	local file="$1" fakehome="$2"
	(
		export HOME="$fakehome"
		SLSDIR="$HOME/.local/share/SLSsteam"
		SLSM_TAG="X-SLSteamMoon-Patched=true"
		log_info() { :; }; log_warn() { :; }; log_success() { :; }
		eval "$FN_PATCH"
		patch_desktop_file "$file"
	)
}

make_desktop() {
	local f="$1"; shift
	{ echo "[Desktop Entry]"; echo "Name=Steam"; echo "Type=Application"
	  local e; for e in "$@"; do echo "$e"; done; } > "$f"
}
primary_exec() { grep -m1 '^Exec=' "$1"; }
tag_count()    { grep -c "^$SLSM_TAG\$" "$1"; }

echo "== test-setup-desktop =="
FH="$(mktemp -d)"; WRAP="$FH/$WRAPPER_REL"

# Case 1: standard /usr/bin/steam (regression baseline)
D="$(mktemp)"; make_desktop "$D" "Exec=/usr/bin/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
	&& ok "standard /usr/bin/steam rewritten to wrapper" \
	|| bad "standard path not rewritten: $(primary_exec "$D")"
[ "$(tag_count "$D")" = "1" ] && ok "standard: tag present once" || bad "standard: tag count $(tag_count "$D")"
rm -f "$D" "$D.slssteam-backup"

# Case 2: non-standard /opt/steam/steam (the originally reported path gap)
D="$(mktemp)"; make_desktop "$D" "Exec=/opt/steam/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
	&& ok "/opt/steam/steam rewritten to wrapper" \
	|| bad "/opt/steam/steam NOT rewritten: $(primary_exec "$D")"
[ "$(tag_count "$D")" = "1" ] && ok "/opt: tag present once" || bad "/opt: tag count $(tag_count "$D")"
rm -f "$D" "$D.slssteam-backup"

# Case 3: bare 'steam' token
D="$(mktemp)"; make_desktop "$D" "Exec=steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
	&& ok "bare 'steam' rewritten to wrapper" \
	|| bad "bare 'steam' NOT rewritten: $(primary_exec "$D")"
rm -f "$D" "$D.slssteam-backup"

# Case 4: env-prefixed Exec (launcher token, not 'env'/'VAR=v', replaced)
D="$(mktemp)"; make_desktop "$D" "Exec=env GSETTINGS_BACKEND=memory /usr/bin/steam %U"
run_patch "$D" "$FH" >/dev/null
[ "$(primary_exec "$D")" = "Exec=env GSETTINGS_BACKEND=memory $WRAP %U" ] \
	&& ok "env-prefixed Exec keeps env, rewrites launcher" \
	|| bad "env-prefixed Exec wrong: $(primary_exec "$D")"
rm -f "$D" "$D.slssteam-backup"

# Case 5: Desktop Action lines are rewritten cleanly, not mangled
D="$(mktemp)"; make_desktop "$D" "Exec=/usr/bin/steam %U"
{ echo ""; echo "[Desktop Action Store]"; echo "Name=Store"; echo "Exec=steam steam://store"; } >> "$D"
run_patch "$D" "$FH" >/dev/null
grep -q "^Exec=$WRAP steam://store\$" "$D" \
	&& ok "Desktop Action rewritten cleanly to '<wrapper> steam://store'" \
	|| bad "Desktop Action mangled: $(grep -m1 'steam://store' "$D")"
rm -f "$D" "$D.slssteam-backup"

# Case 6: a SYMLINK entry must become a REGULAR file and not clobber upstream
D="$(mktemp)"; UP="$(mktemp)"
make_desktop "$UP" "Exec=/usr/games/steam %U"   # Steam's upstream (vanilla) copy
rm -f "$D"; ln -s "$UP" "$D"                      # user entry is a symlink -> upstream
run_patch "$D" "$FH" >/dev/null
[ -L "$D" ] \
	&& bad "patched entry is still a SYMLINK (Steam will re-clobber it on update)" \
	|| ok "symlinked entry materialised as a regular file (survives Steam regen)"
[ "$(primary_exec "$D")" = "Exec=$WRAP %U" ] \
	&& ok "symlink case: Exec rewritten to wrapper" \
	|| bad "symlink case: Exec wrong: $(primary_exec "$D")"
[ "$(primary_exec "$UP")" = "Exec=/usr/games/steam %U" ] \
	&& ok "symlink case: Steam's upstream copy left untouched" \
	|| bad "symlink case: clobbered Steam's upstream copy: $(primary_exec "$UP")"
rm -f "$D" "$D.slssteam-backup" "$UP"

# Case 7: never tag a file with no rewritable Exec
D="$(mktemp)"
{ echo "[Desktop Entry]"; echo "Name=Steam"; echo "Type=Application"; } > "$D"  # no Exec=
run_patch "$D" "$FH" >/dev/null
[ "$(tag_count "$D")" = "0" ] \
	&& ok "no Exec= line -> file is NOT tagged" \
	|| bad "tagged a file with no rewritable Exec (would lock out future runs)"
rm -f "$D" "$D.slssteam-backup"

rm -rf "$FH"
echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
