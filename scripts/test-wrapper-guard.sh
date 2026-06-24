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
# detect repeated short-lived boots and LATCH into a vanilla launch (no
# LD_AUDIT/LD_PRELOAD/sidecar) so the session comes up, then auto-clear the
# latch once the payload is updated.
#
# The wrapper is GENERATED from setup.sh (not a copy), then driven with a fake
# Steam (via SLSM_STEAM_BIN) that records whether it was launched injected
# (LD_AUDIT set) or vanilla, simulating a fast-crash loop.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SETUP="$REPO_ROOT/setup.sh"

PASS=0; FAIL=0
ok()  { echo "  ok   - $*"; PASS=$((PASS+1)); }
bad() { echo "  FAIL - $*" >&2; FAIL=$((FAIL+1)); }

# Extract create_steam_wrapper verbatim from setup.sh and run it to materialise
# the shipped wrapper into our fake SLSDIR. Heredoc-aware: the generated wrapper
# embeds shell functions whose closing "}" sit at column 0 INSIDE the
# "<< 'EOF'" heredoc, so we must not treat those as the function's end.
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

# Minimal payload so the wrapper's injection path + fingerprint have inputs.
mkdir -p "$SLSDIR"
printf 'so'  > "$SLSDIR/SLSsteam.so"
printf 'inj' > "$SLSDIR/library-inject.so"

# Generate the wrapper from shipped code.
( log_info() { :; }; log_success() { :; }; log_warn() { :; }; eval "$FN"; SLSDIR="$SLSDIR" create_steam_wrapper )
WRAP="$SLSDIR/path/steam"
[ -x "$WRAP" ] || { echo "wrapper was not generated at $WRAP" >&2; exit 1; }

# Fake Steam: records injected-vs-vanilla, exits immediately (a fast crash).
FAKE_BIN_DIR="$HOME_DIR/fakebin"; mkdir -p "$FAKE_BIN_DIR"
FAKE_STEAM="$FAKE_BIN_DIR/steam"
INVOCATIONS="$HOME_DIR/invocations.log"
cat > "$FAKE_STEAM" <<'FS'
#!/bin/sh
if [ -n "${LD_AUDIT:-}" ]; then echo "injected" >> "$INVOCATIONS"; else echo "vanilla" >> "$INVOCATIONS"; fi
exit 0
FS
chmod +x "$FAKE_STEAM"
export SLSM_STEAM_BIN="$FAKE_STEAM"
export INVOCATIONS

# A half-injected appinfo.vdf that the latch must clean up.
mkdir -p "$HOME_DIR/.steam/steam/appcache"
printf 'spliced' > "$HOME_DIR/.steam/steam/appcache/appinfo.vdf"

export SLSM_GUARD_MAX_FAILS=3
export SLSM_GUARD_HEALTHY_SECS=150
DUMPS="$HOME_DIR/dumps"; mkdir -p "$DUMPS"
export SLSM_GUARD_DUMPS_DIR="$DUMPS"

run_wrapper() { sh "$WRAP" >/dev/null 2>&1; }
nth() { sed -n "${1}p" "$INVOCATIONS"; }

echo "== test-wrapper-guard =="

# Simulate a crash loop: each launch exits instantly, so every boot after the
# first is "short-lived" (gap ~0 < HEALTHY_SECS) and counts as a failure.
run_wrapper   # boot 1: no history -> injected
run_wrapper   # boot 2: prev short -> fail 1 -> injected
run_wrapper   # boot 3: fail 2 -> injected
run_wrapper   # boot 4: fail 3 >= max -> LATCH -> vanilla
run_wrapper   # boot 5: latched -> vanilla

[ "$(nth 1)" = "injected" ] && ok "boot 1 injected" || bad "boot 1 not injected: $(nth 1)"
[ "$(nth 2)" = "injected" ] && ok "boot 2 injected" || bad "boot 2 not injected: $(nth 2)"
[ "$(nth 3)" = "injected" ] && ok "boot 3 injected" || bad "boot 3 not injected: $(nth 3)"
[ "$(nth 4)" = "vanilla" ]  && ok "boot 4 falls back to vanilla" || bad "boot 4 not vanilla: $(nth 4)"
[ "$(nth 5)" = "vanilla" ]  && ok "boot 5 stays vanilla (latched)" || bad "boot 5 not vanilla: $(nth 5)"

[ -f "$GUARD_DIR/safe_mode" ] \
	&& ok "safe-mode latch written" \
	|| bad "safe-mode latch missing"
[ ! -f "$HOME_DIR/.steam/steam/appcache/appinfo.vdf" ] \
	&& ok "half-injected appinfo.vdf cleaned on latch" \
	|| bad "appinfo.vdf not cleaned"

# Updating the payload (new SLSsteam.so) must clear the latch and retry.
sleep 1; printf 'so-v2-bigger' > "$SLSDIR/SLSsteam.so"
run_wrapper   # boot 6: payload changed -> clear latch -> injected
[ "$(nth 6)" = "injected" ] \
	&& ok "payload update clears latch and re-enables injection" \
	|| bad "payload update did not re-enable injection: $(nth 6)"
[ ! -f "$GUARD_DIR/safe_mode" ] \
	&& ok "latch removed after payload update" \
	|| bad "latch still present after payload update"

# A healthy (long-lived) previous boot must reset the fail counter so transient
# restarts never accumulate toward a latch.
: > "$GUARD_DIR/boot_fail_count"; echo 2 > "$GUARD_DIR/boot_fail_count"
touch -d '2 hours ago' "$GUARD_DIR/last_launch" 2>/dev/null \
	|| touch -t "$(date -d '2 hours ago' +%Y%m%d%H%M 2>/dev/null || echo 202001010000)" "$GUARD_DIR/last_launch" 2>/dev/null
run_wrapper   # boot 7: prev boot was 2h -> healthy -> reset -> injected
[ "$(nth 7)" = "injected" ] && ok "healthy gap keeps injection" || bad "boot 7 not injected: $(nth 7)"
[ "$(guard_count="$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"; echo "${guard_count:-x}")" = "0" ] \
	&& ok "fail counter reset after a healthy boot" \
	|| bad "fail counter not reset: $(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"

# Slow-teardown loop: the inter-boot GAP exceeds HEALTHY_SECS (so the timing
# signal alone reads "healthy"), but Steam wrote a startup crash dump during the
# boot. The dump signal must still flag it as a failed boot.
rm -rf "$GUARD_DIR"; mkdir -p "$GUARD_DIR"
echo 0 > "$GUARD_DIR/boot_fail_count"
# Previous boot started 300s ago (gap 300 > 150 => timing says healthy).
boot_start=$(( $(date +%s) - 300 ))
touch -d "@$boot_start" "$GUARD_DIR/last_launch"
# A crash dump written 12s into that boot (within the 150s startup window).
touch -d "@$(( boot_start + 12 ))" "$DUMPS/assert_test.dmp"
: > "$INVOCATIONS"
run_wrapper
[ "$(guard_count="$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"; echo "${guard_count:-x}")" = "1" ] \
	&& ok "startup crash dump flags a failed boot despite a long gap" \
	|| bad "crash-dump signal missed: count=$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"

# A dump OUTSIDE the startup window (e.g. a game crash hours into a healthy
# session) must NOT count as a boot failure.
rm -rf "$GUARD_DIR"; mkdir -p "$GUARD_DIR"; rm -f "$DUMPS"/*.dmp
echo 0 > "$GUARD_DIR/boot_fail_count"
boot_start=$(( $(date +%s) - 8000 ))
touch -d "@$boot_start" "$GUARD_DIR/last_launch"
touch -d "@$(( boot_start + 7000 ))" "$DUMPS/assert_late.dmp"   # ~2h in
: > "$INVOCATIONS"
run_wrapper
[ "$(guard_count="$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"; echo "${guard_count:-x}")" = "0" ] \
	&& ok "late (in-game) crash dump does not count as a boot failure" \
	|| bad "late dump wrongly counted: count=$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"

rm -rf "$HOME_DIR"

echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
