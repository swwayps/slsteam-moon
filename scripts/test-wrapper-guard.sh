#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-wrapper-guard.sh - integration test for the Steam wrapper's crash-loop
# fail-safe (setup.sh::create_steam_wrapper).
#
# Why this exists
# ---------------
# In gamescope Game Mode the session supervisor relaunches Steam every time it
# exits. If the injected stack (SLSsteam/CloudRedirect/Lumen) is incompatible
# with a freshly-updated Steam client and stalls the engine, the device loops
# forever and the user can never reach Desktop Mode to update. The wrapper must
# detect repeated STARTUP CRASHES and LATCH into a vanilla launch (no
# LD_AUDIT/LD_PRELOAD/sidecar) so the session comes up, then auto-clear the
# latch once the payload is updated.
#
# Crucially, the trigger is a startup CRASH DUMP, NOT a merely short session:
# switching Game Mode <-> Desktop, a client self-update restart, or a quick quit
# all end Steam fast but are healthy and must NOT latch (regression test below).
#
# The wrapper is GENERATED from setup.sh (not a copy), then driven with a fake
# Steam (via SLSM_STEAM_BIN) and a fake dumps dir.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$REPO_ROOT/setup.sh"

PASS=0; FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

# Heredoc-aware extraction: the generated wrapper embeds shell functions whose
# closing "}" sit at column 0 INSIDE the "<< 'EOF'" heredoc.
FN="$(awk '
	/^create_steam_wrapper\(\)/ { f=1 }
	f && /<< '\''EOF'\''/ { inhd=1 }
	f { print }
	f && inhd && /^EOF$/ { inhd=0; next }
	f && !inhd && /^}/ { exit }
' "$SETUP")"
[ -n "$FN" ] || { echo "could not extract create_steam_wrapper from setup.sh" >&2; exit 1; }

HOME_DIR="$(mktemp -d)"
export HOME="$HOME_DIR"
export XDG_STATE_HOME="$HOME_DIR/.local/state"
SLSDIR="$HOME_DIR/.local/share/SLSsteam"
GUARD_DIR="$XDG_STATE_HOME/slsteam-moon"

mkdir -p "$SLSDIR"
printf 'so'  > "$SLSDIR/SLSsteam.so"
printf 'inj' > "$SLSDIR/library-inject.so"

( log_info() { :; }; log_success() { :; }; log_warn() { :; }; eval "$FN"; SLSDIR="$SLSDIR" create_steam_wrapper )
WRAP="$SLSDIR/path/steam"
[ -x "$WRAP" ] || { echo "wrapper was not generated at $WRAP" >&2; exit 1; }

# Fake Steam: records injected-vs-vanilla, exits immediately.
FAKE_STEAM="$HOME_DIR/steam"
INVOCATIONS="$HOME_DIR/invocations.log"
cat > "$FAKE_STEAM" <<'FS'
#!/bin/sh
if [ -n "${LD_AUDIT:-}" ]; then echo "injected" >> "$INVOCATIONS"; else echo "vanilla" >> "$INVOCATIONS"; fi
exit 0
FS
chmod +x "$FAKE_STEAM"
export SLSM_STEAM_BIN="$FAKE_STEAM" INVOCATIONS

mkdir -p "$HOME_DIR/.steam/steam/appcache"
printf 'spliced' > "$HOME_DIR/.steam/steam/appcache/appinfo.vdf"

export SLSM_GUARD_MAX_FAILS=3
export SLSM_GUARD_STARTUP_SECS=180
DUMPS="$HOME_DIR/dumps"; mkdir -p "$DUMPS"
export SLSM_GUARD_DUMPS_DIR="$DUMPS"

run_wrapper() { sh "$WRAP" >/dev/null 2>&1; }
count() { c="$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"; echo "${c:-x}"; }
nth() { sed -n "${1}p" "$INVOCATIONS"; }
reset_state() { rm -rf "$GUARD_DIR"; mkdir -p "$GUARD_DIR"; : > "$INVOCATIONS"; rm -f "$DUMPS"/*.dmp 2>/dev/null; }

echo "== test-wrapper-guard =="

# --- REGRESSION: short but CLEAN boots must NOT latch (no crash dumps) --------
# This is the Game Mode <-> Desktop switching / quick-quit case that previously
# false-latched on a bare timing heuristic.
reset_state
for i in 1 2 3 4 5; do
	# Each "session" started seconds ago (very short) but left no crash dump.
	[ -f "$GUARD_DIR/last_launch" ] && touch -d "@$(( $(date +%s) - 5 ))" "$GUARD_DIR/last_launch"
	run_wrapper
done
allinj=1; for i in 1 2 3 4 5; do [ "$(nth $i)" = "injected" ] || allinj=0; done
[ "$allinj" = 1 ] && ok "short clean boots stay injected (no false latch)" || bad "short clean boots wrongly fell back: $(cat "$INVOCATIONS" | tr '\n' ' ')"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "no latch from short clean boots" || bad "latched on short clean boots"

# --- crash loop: each boot leaves a startup crash dump -> latch -> vanilla ----
reset_state
for i in 1 2 3 4; do
	# Simulate: previous boot started ~30s ago and crashed ~12s in.
	if [ -f "$GUARD_DIR/last_launch" ]; then
		bs=$(( $(date +%s) - 30 ))
		touch -d "@$bs" "$GUARD_DIR/last_launch"
		touch -d "@$(( bs + 12 ))" "$DUMPS/assert_$i.dmp"
	fi
	run_wrapper
done
[ "$(nth 1)" = "injected" ] && ok "crash-loop boot 1 injected" || bad "boot 1: $(nth 1)"
[ "$(nth 4)" = "vanilla" ]  && ok "crash-loop boot 4 falls back to vanilla" || bad "boot 4: $(nth 4)"
[ -f "$GUARD_DIR/safe_mode" ] && ok "latched after 3 startup crashes" || bad "did not latch after 3 crashes"
[ ! -f "$HOME_DIR/.steam/steam/appcache/appinfo.vdf" ] && ok "appinfo.vdf cleaned on latch" || bad "appinfo.vdf not cleaned"

# stays latched on a subsequent boot
run_wrapper
[ "$(nth 5)" = "vanilla" ] && ok "stays vanilla while latched" || bad "boot 5: $(nth 5)"

# --- payload update clears the latch ------------------------------------------
sleep 1; printf 'so-v2-bigger' > "$SLSDIR/SLSsteam.so"
: > "$INVOCATIONS"
run_wrapper
[ "$(nth 1)" = "injected" ] && ok "payload update clears latch, re-enables injection" || bad "after update: $(nth 1)"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "latch removed after payload update" || bad "latch still present"

# --- a LATE (in-game) crash dump must NOT count as a boot failure -------------
reset_state
echo 0 > "$GUARD_DIR/boot_fail_count"
bs=$(( $(date +%s) - 8000 ))
touch -d "@$bs" "$GUARD_DIR/last_launch"
touch -d "@$(( bs + 7000 ))" "$DUMPS/assert_late.dmp"   # ~2h into the session
run_wrapper
[ "$(count)" = "0" ] && ok "late in-game crash dump does not count as a boot failure" || bad "late dump wrongly counted: $(count)"

# --- slow teardown: crash dump in-window even when the gap is huge ------------
reset_state
echo 0 > "$GUARD_DIR/boot_fail_count"
bs=$(( $(date +%s) - 600 ))                              # boot started 10 min ago
touch -d "@$bs" "$GUARD_DIR/last_launch"
touch -d "@$(( bs + 15 ))" "$DUMPS/assert_slow.dmp"      # crashed 15s in
run_wrapper
[ "$(count)" = "1" ] && ok "startup crash counts even with a long teardown gap" || bad "slow-teardown crash missed: $(count)"

rm -rf "$HOME_DIR"

echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
