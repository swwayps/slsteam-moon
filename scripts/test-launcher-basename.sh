#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-launcher-basename.sh - the distro launcher must never be executed under
# a name it does not recognise.
#
# Why this exists
# ---------------
# On Fedora/Nobara (and any distro shipping Valve's launcher directly)
# /usr/bin/steam is a symlink to /usr/lib/steam/bin_steam.sh, and that script
# derives its whole identity from its own argv[0]:
#
#     STEAMPACKAGE="${0##*/}"
#     if [ "$STEAMPACKAGE" = bin_steam.sh ]; then STEAMPACKAGE=steam; fi
#     case "$STEAMPACKAGE" in
#         steam) ... ;; steambeta) ... ;;
#         *) log "Unknown Steam package '$STEAMPACKAGE'"; exit 1 ;;
#     esac
#
# Our launcher shim captures the original as `<mirror>/steam.orig`. Executing
# that captured file under that name aborts the launch with
# "Unknown Steam package 'steam.orig'" BEFORE Steam opens its logger, so the
# user sees Steam simply not starting and no log is produced anywhere.
#
# Two independent layers are covered:
#   1. the generated shim must hand the wrapper (and its own fallback) a path
#      whose basename Valve's launcher accepts;
#   2. the wrapper must normalise ANY name-unsafe launcher it is told to run,
#      so a shim left behind by an older release cannot break the launch.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$REPO_ROOT/setup.sh"
TMP="$(mktemp -d)"
trap 'chmod -R u+w "$TMP" 2>/dev/null || true; rm -rf "$TMP"' EXIT
PASS=0; FAIL=0
check() {
	if [ "$2" = "$3" ]; then
		printf 'ok   - %s\n' "$1"; PASS=$((PASS+1))
	else
		printf 'FAIL - %s (want=[%s] got=[%s])\n' "$1" "$2" "$3" >&2; FAIL=$((FAIL+1))
	fi
}

# A stand-in for Valve's bin_steam.sh: it refuses to run under any other name.
# When VL_LOG is set it also records the name it was invoked under and whether
# the injected loader environment survived the call.
write_valve_launcher() {
	cat > "$1" <<'VL'
#!/bin/sh
case "${0##*/}" in
	steam|steambeta|bin_steam.sh) ;;
	*) printf "bin_steam.sh[$$]: Unknown Steam package '%s'\n" "${0##*/}" >&2; exit 1 ;;
esac
if [ -n "${VL_LOG:-}" ]; then
	printf 'argv0=%s audit=%s\n' "${0##*/}" "${LD_AUDIT:+set}" >> "$VL_LOG"
fi
printf 'VALVE-LAUNCHER %s\n' "$*"
VL
	chmod 0755 "$1"
}

# ---------------------------------------------------------------------------
# Layer 1 - the shim library
# ---------------------------------------------------------------------------
HOME="$TMP/home"
export HOME
BIN_DIR="$TMP/usr/bin"
STEAM_ROOT="$TMP/steam-root"
mkdir -p "$HOME/.local/share/SLSsteam/path" "$HOME/.steam" "$BIN_DIR" "$STEAM_ROOT"
printf '#!/bin/sh\nprintf "DATA-STEAMSH %%s\\n" "$*"\n' > "$STEAM_ROOT/steam.sh"
chmod 0755 "$STEAM_ROOT/steam.sh"
ln -s "$STEAM_ROOT" "$HOME/.steam/steam"
write_valve_launcher "$BIN_DIR/steam"

# Wrapper stub that behaves like the real one: an explicit SLSM_STEAM_BIN wins
# and is exec'd, so argv[0] is exactly what the shim handed over.
cat > "$HOME/.local/share/SLSsteam/path/steam" <<'W'
#!/bin/sh
exec "${SLSM_STEAM_BIN:?no SLSM_STEAM_BIN}" "$@"
W
chmod 0755 "$HOME/.local/share/SLSsteam/path/steam"

LS_SLSDIR="$HOME/.local/share/SLSsteam"
LS_BACKUP_ROOT="$LS_SLSDIR/system-launcher-backup"
LS_LAUNCHER_DIRS=("$BIN_DIR")
LS_STEAM_ROOT="$STEAM_ROOT"
LS_SUDO=""
# shellcheck source=/dev/null
. "$REPO_ROOT/tools/launcher-shim.lib.sh"

check "shims install over a Valve-style launcher" "yes" \
	"$(ls_install_shims && echo yes || echo no)"
check "captured original still refuses a foreign name" "1" \
	"$(sh "$(ls_backup_path "$BIN_DIR/steam")" >/dev/null 2>&1; echo $?)"
check "shim launch through the wrapper reaches the real launcher" "VALVE-LAUNCHER" \
	"$(sh "$BIN_DIR/steam" -silent 2>&1 | cut -d' ' -f1)"
check "shim forwards the launcher arguments" "VALVE-LAUNCHER -silent" \
	"$(sh "$BIN_DIR/steam" -silent 2>&1 | head -n1)"

# Same requirement without the wrapper: the shim's own fallback execs the
# captured original directly.
mv "$HOME/.local/share/SLSsteam/path/steam" "$TMP/wrapper-away"
check "shim fallback without the wrapper reaches the real launcher" "VALVE-LAUNCHER" \
	"$(sh "$BIN_DIR/steam" 2>&1 | cut -d' ' -f1)"
mv "$TMP/wrapper-away" "$HOME/.local/share/SLSsteam/path/steam"

# A missing alias must be recreated by the shim itself: the alias lives in the
# user's own tree, so an older install or a partial restore is self-healing and
# does not need the installer to be run again.
ALIAS="$(ls_backup_alias_path "$BIN_DIR/steam")"
ALIAS_DIR="$(dirname -- "$ALIAS")"
rm -f "$ALIAS"
check "shim heals a missing exec alias" "VALVE-LAUNCHER" \
	"$(sh "$BIN_DIR/steam" 2>&1 | cut -d' ' -f1)"
check "healed alias points at the captured original" "$(ls_backup_path "$BIN_DIR/steam")" \
	"$(readlink -f "$ALIAS")"

# With no name-safe path obtainable at all, the shim must not execute the
# captured original under a rejected name: fall through to the data-dir
# steam.sh, which ignores its own argv[0].
if [ "$(id -u)" -eq 0 ]; then
	printf 'skip - unobtainable-alias fallback (root can write anything)\n'
else
	rm -f "$ALIAS" "$HOME/.local/share/SLSsteam/path/steam"
	chmod a-w "$ALIAS_DIR"
	check "shim that cannot obtain a name-safe original uses steam.sh" "DATA-STEAMSH" \
		"$(sh "$BIN_DIR/steam" 2>&1 | cut -d' ' -f1)"
	chmod u+w "$ALIAS_DIR"
fi
cat > "$HOME/.local/share/SLSsteam/path/steam" <<'W'
#!/bin/sh
exec "${SLSM_STEAM_BIN:?no SLSM_STEAM_BIN}" "$@"
W
chmod 0755 "$HOME/.local/share/SLSsteam/path/steam"
ls_refresh_backup_alias "$(ls_backup_path "$BIN_DIR/steam")" >/dev/null 2>&1 || true

# Restoration must put the genuine launcher back and clean the alias up.
check "restore puts the genuine launcher back" "yes" \
	"$(ls_restore_shims && echo yes || echo no)"
check "restored launcher runs under its own name" "VALVE-LAUNCHER" \
	"$(sh "$BIN_DIR/steam" 2>&1 | cut -d' ' -f1)"
check "restore removes the exec alias" "no" \
	"$([ -e "$ALIAS" ] && echo yes || echo no)"

# ---------------------------------------------------------------------------
# Layer 2 - the wrapper, driven by a shim from the broken release
# ---------------------------------------------------------------------------
# Generated from setup.sh (not a copy), exactly like test-wrapper-guard.sh.
FN="$(awk '
	/^create_steam_wrapper\(\)/ { f=1 }
	f && /<< '\''EOF'\''/ { inhd=1 }
	f { print }
	f && inhd && /^EOF$/ { inhd=0; next }
	f && !inhd && /^}/ { exit }
' "$SETUP")"
[ -n "$FN" ] || { echo "could not extract create_steam_wrapper from setup.sh" >&2; exit 1; }

W_HOME="$TMP/wrapper-home"
export HOME="$W_HOME"
export XDG_STATE_HOME="$W_HOME/.local/state"
W_SLSDIR="$W_HOME/.local/share/SLSsteam"
W_STEAM_ROOT="$W_HOME/steam-root"
W_BACKUP="$W_SLSDIR/system-launcher-backup/usr/bin/steam.orig"
mkdir -p "$W_SLSDIR" "$W_HOME/.steam" "$W_STEAM_ROOT" "$(dirname "$W_BACKUP")"
printf 'so'  > "$W_SLSDIR/SLSsteam.so"
printf '#!/bin/sh\nprintf "WRAPPER-STEAMSH %%s\\n" "$*"\n' > "$W_STEAM_ROOT/steam.sh"
chmod 0755 "$W_STEAM_ROOT/steam.sh"
ln -s "$W_STEAM_ROOT" "$W_HOME/.steam/steam"
write_valve_launcher "$W_BACKUP"
( log_info() { :; }; log_success() { :; }; log_warn() { :; }; eval "$FN"; SLSDIR="$W_SLSDIR" create_steam_wrapper ) >/dev/null
WRAP="$W_SLSDIR/path/steam"
[ -x "$WRAP" ] || { echo "wrapper was not generated at $WRAP" >&2; exit 1; }

# This is precisely what the shipped (broken) shim exports.
VL_LOG="$W_HOME/valve.log"
export VL_LOG
: > "$VL_LOG"
out="$(SLSM_STEAM_BIN="$W_BACKUP" sh "$WRAP" -silent 2>&1)"
check "wrapper normalises a name-unsafe launcher from a stale shim" "VALVE-LAUNCHER" \
	"$(printf '%s\n' "$out" | grep -o 'VALVE-LAUNCHER' | head -n1)"
check "wrapper preserves the launcher arguments while normalising" "VALVE-LAUNCHER -silent" \
	"$(printf '%s\n' "$out" | grep 'VALVE-LAUNCHER' | head -n1)"
check "normalised launch reaches the launcher as 'steam'" "argv0=steam" \
	"$(grep -o 'argv0=[^ ]*' "$VL_LOG" | head -n1)"
check "wrapper keeps injecting while normalising" "audit=set" \
	"$(grep -o 'audit=[^ ]*' "$VL_LOG" | head -n1)"

# A name-safe target must be handed through untouched (no needless aliasing).
SAFE_DIR="$W_HOME/safe-launcher"
mkdir -p "$SAFE_DIR"
write_valve_launcher "$SAFE_DIR/steam"
out_safe="$(SLSM_STEAM_BIN="$SAFE_DIR/steam" sh "$WRAP" -foo 2>&1)"
check "wrapper runs a name-safe launcher directly" "VALVE-LAUNCHER -foo" \
	"$(printf '%s\n' "$out_safe" | grep 'VALVE-LAUNCHER' | head -n1)"

# The captured original itself must remain the authoritative backup: the alias
# is an extra path, never a replacement (uninstall restores from *.orig).
check "captured original is still present after a normalised launch" "yes" \
	"$([ -f "$W_BACKUP" ] && echo yes || echo no)"

printf '\n%s passed, %s failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
