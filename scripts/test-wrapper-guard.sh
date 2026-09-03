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

( umask 0002; log_info() { :; }; log_success() { :; }; log_warn() { :; }; eval "$FN"; SLSDIR="$SLSDIR" create_steam_wrapper )
WRAP="$SLSDIR/path/steam"
[ -x "$WRAP" ] || { echo "wrapper was not generated at $WRAP" >&2; exit 1; }

# Fake Steam: records injected-vs-vanilla, exits immediately.
FAKE_STEAM="$HOME_DIR/steam"
INVOCATIONS="$HOME_DIR/invocations.log"
ENV_CAPTURE="$HOME_DIR/environment.log"
STDERR_CAPTURE="$HOME_DIR/stderr.log"
cat > "$FAKE_STEAM" <<'FS'
#!/bin/sh
if [ -n "${LD_AUDIT:-}" ]; then echo "injected" >> "$INVOCATIONS"; else echo "vanilla" >> "$INVOCATIONS"; fi
{
  printf 'LD_AUDIT=%s\n' "${LD_AUDIT:-}"
  printf 'LD_PRELOAD=%s\n' "${LD_PRELOAD:-}"
  printf 'LD_LIBRARY_PATH=%s\n' "${LD_LIBRARY_PATH:-}"
} > "$ENV_CAPTURE"
if [ -n "${SLSM_TEST_EMIT_STDERR:-}" ]; then
  echo "steam-real-error: keep this diagnostic" >&2
fi
[ -n "${PATTERN_ORDER_LOG:-}" ] && echo "steam" >> "$PATTERN_ORDER_LOG"
exit 0
FS
chmod +x "$FAKE_STEAM"
export SLSM_STEAM_BIN="$FAKE_STEAM" INVOCATIONS ENV_CAPTURE

mkdir -p "$HOME_DIR/.steam/steam/appcache"
printf 'spliced' > "$HOME_DIR/.steam/steam/appcache/appinfo.vdf"

export SLSM_GUARD_MAX_FAILS=3
export SLSM_GUARD_STARTUP_SECS=180
DUMPS="$HOME_DIR/dumps"; mkdir -p "$DUMPS"
export SLSM_GUARD_DUMPS_DIR="$DUMPS"
# Every case below simulates a DESKTOP launch, so the harness must look like one
# to the guard: a launch with no display crashes inside libX11 on its own and is
# deliberately not assessed (see the headless case at the end).
export DISPLAY="${DISPLAY:-:0}"
# Recovery state is scoped to the machine's session; pin it so the hand-crafted
# fixtures below are not discarded as belonging to a previous one.
export SLSM_GUARD_SESSION_ID=harness-session

run_wrapper() {
	if [ "${SLSM_TRACE_WRAPPER:-0}" = 1 ]; then
		sh -x "$WRAP"
	else
		sh "$WRAP" >/dev/null 2>&1
	fi
}
count() { c="$(cat "$GUARD_DIR/boot_fail_count" 2>/dev/null)"; echo "${c:-x}"; }
nth() { sed -n "${1}p" "$INVOCATIONS"; }
reset_state() {
	rm -rf "$GUARD_DIR"; mkdir -p "$GUARD_DIR"
	# Cases that hand-craft last_launch/boot_fail_count mean them as history from
	# the CURRENT session, so stamp the session they belong to.
	printf '%s' "$SLSM_GUARD_SESSION_ID" > "$GUARD_DIR/session_id"
	: > "$INVOCATIONS"; rm -f "$DUMPS"/*.dmp 2>/dev/null
}

echo "== test-wrapper-guard =="

[ "$(stat -c '%a' "$WRAP")" = 755 ] \
  && ok "wrapper mode is trusted even under a group-writable umask" \
  || bad "wrapper mode follows caller umask: $(stat -c '%a' "$WRAP")"

# An old desktop entry can invoke the wrapper with our audit objects already in
# LD_AUDIT.  The wrapper must canonicalize its own entries to one pair and keep
# unrelated auditors intact.
reset_state
export LD_AUDIT="$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so:$SLSDIR/library-inject.so:$HOME_DIR/foreign-auditor.so"
export LD_PRELOAD="/lib/x86_64-linux-gnu/libm.so.6"
export LD_LIBRARY_PATH="$HOME_DIR/foreign-libs"
run_wrapper
expected_audit="$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so:$HOME_DIR/foreign-auditor.so"
actual_audit="$(sed -n 's/^LD_AUDIT=//p' "$ENV_CAPTURE")"
[ "$actual_audit" = "$expected_audit" ] && ok "wrapper canonicalizes inherited SLS audit entries" || bad "audit was not canonicalized: $actual_audit"
[ "$(printf '%s' "$actual_audit" | awk -F: -v a="$SLSDIR/library-inject.so" 'BEGIN{n=0}{for(i=1;i<=NF;i++) if($i==a)n++} END{print n}')" = 1 ] \
  && ok "wrapper keeps one library-inject auditor" || bad "library-inject was duplicated"

# A captured managed shim must never be accepted as an explicit Steam binary.
# The wrapper should discard it and resolve the normal Steam script instead.
MANAGED_PIN="$HOME_DIR/managed-backup.orig"
FALLBACK_STEAM="$HOME_DIR/.steam/steam/steam.sh"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nprintf "managed\\n" >> "$INVOCATIONS"\n' > "$MANAGED_PIN"
printf '#!/bin/sh\nprintf "fallback\\n" >> "$INVOCATIONS"\n' > "$FALLBACK_STEAM"
chmod +x "$MANAGED_PIN" "$FALLBACK_STEAM"
reset_state
unset LD_AUDIT LD_PRELOAD LD_LIBRARY_PATH
export SLSM_STEAM_BIN="$MANAGED_PIN"
run_wrapper
[ "$(nth 1)" = "fallback" ] \
  && ok "wrapper rejects managed explicit backup" \
  || bad "managed explicit backup was launched: $(nth 1)"
export SLSM_STEAM_BIN="$FAKE_STEAM"

# An absolute path containing a symlink must not bypass the pre-bootstrap guard.
# The wrapper receives this form from a managed launcher shim. Canonicalizing it
# before the guard changes the prefix match and can incorrectly enable injection.
SYMLINK_ROOT="$HOME_DIR/symlink-case"
SYMLINK_HOME_REAL="$SYMLINK_ROOT/home-real"
SYMLINK_HOME="$SYMLINK_ROOT/home-link"
mkdir -p "$SYMLINK_HOME_REAL/.local/share/SLSsteam/system-launcher-backup/usr/bin"
ln -s "$SYMLINK_HOME_REAL" "$SYMLINK_HOME"
SYMLINK_SLSDIR="$SYMLINK_HOME/.local/share/SLSsteam"
SYMLINK_ORIG="$SYMLINK_SLSDIR/system-launcher-backup/usr/bin/steam.orig"
SYMLINK_LOG="$SYMLINK_ROOT/invocation.log"
printf 'so' > "$SYMLINK_SLSDIR/SLSsteam.so"
printf 'inj' > "$SYMLINK_SLSDIR/library-inject.so"
(
	export HOME="$SYMLINK_HOME" SLSDIR="$SYMLINK_SLSDIR"
	log_info() { :; }
	log_success() { :; }
	log_warn() { :; }
	eval "$FN"
	create_steam_wrapper
)
cat > "$SYMLINK_ORIG" <<'FS'
#!/bin/sh
if [ -n "${LD_AUDIT:-}" ]; then echo "injected" >> "$SYMLINK_LOG"; else echo "vanilla" >> "$SYMLINK_LOG"; fi
exit 0
FS
chmod +x "$SYMLINK_ORIG"
(
	export HOME="$SYMLINK_HOME" SLSM_STEAM_BIN="$SYMLINK_ORIG" SYMLINK_LOG
	export SLSM_GUARD_DUMPS_DIR="$SYMLINK_ROOT/dumps"
	mkdir -p "$SLSM_GUARD_DUMPS_DIR"
	sh "$SYMLINK_SLSDIR/path/steam" >/dev/null 2>&1
)
[ "$(cat "$SYMLINK_LOG" 2>/dev/null)" = "vanilla" ] \
  && ok "absolute symlink paths preserve the bootstrap guard" \
  || bad "absolute symlink path bypassed the bootstrap guard"

# Safe mode must be genuinely vanilla even when an old desktop entry supplied
# loader variables before the wrapper was reached.  Create a real latch first
# so the fingerprint is the one generated by the wrapper itself.
export SLSM_GUARD_MAX_FAILS=1
reset_state
run_wrapper
bs=$(( $(date +%s) - 30 )); touch -d "@$bs" "$GUARD_DIR/last_launch"; touch -d "@$(( bs + 12 ))" "$DUMPS/crash_safe.dmp"
run_wrapper
[ -f "$GUARD_DIR/safe_mode" ] || bad "could not create safe-mode fixture"
export SLSM_GUARD_MAX_FAILS=3
export LD_AUDIT="$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so"
export LD_PRELOAD="/lib/x86_64-linux-gnu/libm.so.6"
export LD_LIBRARY_PATH="$HOME_DIR/foreign-libs"
run_wrapper
[ "$(sed -n 's/^LD_AUDIT=//p' "$ENV_CAPTURE")" = "" ] \
  && ok "safe mode removes LD_AUDIT" || bad "safe mode inherited LD_AUDIT"
[ "$(sed -n 's/^LD_PRELOAD=//p' "$ENV_CAPTURE")" = "" ] \
  && ok "safe mode removes LD_PRELOAD" || bad "safe mode inherited LD_PRELOAD"
[ "$(sed -n 's/^LD_LIBRARY_PATH=//p' "$ENV_CAPTURE")" = "" ] \
  && ok "safe mode removes LD_LIBRARY_PATH" || bad "safe mode inherited LD_LIBRARY_PATH"

# Steam stderr is forwarded directly (no FIFO filter).
rm -f "$GUARD_DIR/safe_mode" "$GUARD_DIR/safe_mode_fingerprint"
: > "$STDERR_CAPTURE"
unset LD_AUDIT LD_PRELOAD LD_LIBRARY_PATH
export SLSM_TEST_EMIT_STDERR=1
sh "$WRAP" >/dev/null 2>"$STDERR_CAPTURE" || true
for _wait in 1 2 3 4 5 6 7 8 9 10; do
	grep -qF 'steam-real-error: keep this diagnostic' "$STDERR_CAPTURE" && break
	sleep 0.1
done
grep -qF 'steam-real-error: keep this diagnostic' "$STDERR_CAPTURE" \
  && ok "Steam stderr is forwarded" \
  || bad "Steam stderr was lost"
unset SLSM_TEST_EMIT_STDERR LD_AUDIT LD_PRELOAD LD_LIBRARY_PATH

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
		touch -d "@$(( bs + 12 ))" "$DUMPS/crash_$i.dmp"
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
touch -d "@$(( bs + 7000 ))" "$DUMPS/crash_late.dmp"   # ~2h into the session
run_wrapper
[ "$(count)" = "0" ] && ok "late in-game crash dump does not count as a boot failure" || bad "late dump wrongly counted: $(count)"

# --- slow teardown: crash dump in-window even when the gap is huge ------------
reset_state
echo 0 > "$GUARD_DIR/boot_fail_count"
bs=$(( $(date +%s) - 600 ))                              # boot started 10 min ago
touch -d "@$bs" "$GUARD_DIR/last_launch"
touch -d "@$(( bs + 15 ))" "$DUMPS/crash_slow.dmp"      # crashed 15s in
run_wrapper
[ "$(count)" = "1" ] && ok "startup crash counts even with a long teardown gap" || bad "slow-teardown crash missed: $(count)"

# --- a NON-FATAL assert_*.dmp in the startup window must NOT count ------------
# Steam writes assert_*.dmp for non-fatal assertions (e.g. CloudRedirect's
# cloud-save path-resolution asserts) while it keeps running fine. These land in
# /tmp/dumps within the startup window but are NOT crashes - the guard must
# ignore them (only crash_*.dmp = a fatal segfault/abort counts).
reset_state
echo 0 > "$GUARD_DIR/boot_fail_count"
bs=$(( $(date +%s) - 30 ))
touch -d "@$bs" "$GUARD_DIR/last_launch"
touch -d "@$(( bs + 12 ))" "$DUMPS/assert_20260627232123_37.dmp"   # non-fatal assert, 12s in
run_wrapper
[ "$(count)" = "0" ] && ok "non-fatal assert dump does not count as a startup crash" || bad "assert dump wrongly counted: $(count)"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "no latch from a non-fatal assert dump" || bad "wrongly latched on a non-fatal assert dump"

# --- FAST RECOVERY: a startup crash right after steamclient.so changed latches
# on the FIRST crash, not after MAX_FAILS. This is the post-update brick we
# actually care about: making the user sit through 3 crash loops to get Steam
# back makes no sense when the cause (a fresh client) is known. ---------------
CLIENT="$HOME_DIR/.steam/steam/ubuntu12_32/steamclient.so"
mkdir -p "$(dirname "$CLIENT")"

reset_state
printf 'client-v1' > "$CLIENT"
run_wrapper                                              # boot 1: no history -> injects, records the client it ran
touch -d "@$(( $(date +%s) - 300 ))" "$GUARD_DIR/last_launch"   # boot 1 ran clean
run_wrapper                                              # boot 2: boot 1 healthy -> remembers client-v1 as known-good
[ "$(nth 2)" = "injected" ] && ok "fast-recovery baseline: healthy boots inject" || bad "boot 2: $(nth 2)"

sleep 1; printf 'client-v2-newer-bigger' > "$CLIENT"     # Steam self-updated the client
touch -d "@$(( $(date +%s) - 300 ))" "$GUARD_DIR/last_launch"   # boot 2 ran clean
run_wrapper                                              # boot 3: injects the NEW client
[ "$(nth 3)" = "injected" ] && ok "fast-recovery: first boot on the updated client still injects" || bad "boot 3: $(nth 3)"
bs=$(( $(date +%s) - 30 )); touch -d "@$bs" "$GUARD_DIR/last_launch"; touch -d "@$(( bs + 12 ))" "$DUMPS/crash_upd.dmp"
run_wrapper                                              # boot 4: crash + client changed since good -> latch NOW
[ "$(nth 4)" = "vanilla" ] && ok "fast-recovery: latches on the FIRST crash after a client update" || bad "boot 4 not vanilla: $(nth 4)"
[ -f "$GUARD_DIR/safe_mode" ] && ok "fast-recovery: latched after one post-update crash" || bad "did not latch after one post-update crash"
[ "$(count)" = "1" ] && ok "fast-recovery: latched at fail count 1 (not MAX_FAILS)" || bad "unexpected fail count at fast latch: $(count)"

# --- a single crash with an UNCHANGED client must NOT fast-latch (random/one-off
# crash not caused by an update keeps the conservative MAX_FAILS threshold) ----
reset_state
printf 'client-stable' > "$CLIENT"
run_wrapper                                              # boot 1
touch -d "@$(( $(date +%s) - 300 ))" "$GUARD_DIR/last_launch"
run_wrapper                                              # boot 2 healthy -> good = client-stable
bs=$(( $(date +%s) - 30 )); touch -d "@$bs" "$GUARD_DIR/last_launch"; touch -d "@$(( bs + 12 ))" "$DUMPS/crash_rand.dmp"
run_wrapper                                              # boot 3: one crash, client unchanged -> count=1, NO latch
[ "$(nth 3)" = "injected" ] && ok "unchanged client: single crash still injects (no fast latch)" || bad "boot 3 wrongly fell back: $(nth 3)"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "unchanged client: no latch on a single crash" || bad "wrongly latched on one crash with unchanged client"
[ "$(count)" = "1" ] && ok "unchanged client: fail count incremented to 1" || bad "unexpected count: $(count)"

# --- SESSION SCOPE: the recovery latch must not outlive the session -----------
# The latch exists to break a relaunch loop, and a relaunch loop happens entirely
# inside one boot. Keeping it afterwards left the user on a silently unhooked
# Steam that only a payload reinstall could undo: reboot, open Steam from the
# desktop menu, no injection, no plugin UI, no explanation. A new session must
# re-arm injection - if the incompatibility is real the loop simply re-latches
# within seconds and the session still comes up.
export SLSM_GUARD_SESSION_ID=session-A
reset_state
export SLSM_GUARD_MAX_FAILS=1
run_wrapper                                              # launch 1: injects
bs=$(( $(date +%s) - 30 )); touch -d "@$bs" "$GUARD_DIR/last_launch"; touch -d "@$(( bs + 12 ))" "$DUMPS/crash_sess.dmp"
run_wrapper                                              # launch 2: crash -> latch
export SLSM_GUARD_MAX_FAILS=3
[ -f "$GUARD_DIR/safe_mode" ] && ok "session scope: latch created inside the session" || bad "could not create the session latch"
run_wrapper
[ "$(nth 3)" = "vanilla" ] && ok "session scope: latch holds for the rest of the session" || bad "latch did not hold in-session: $(nth 3)"
export SLSM_GUARD_SESSION_ID=session-B
run_wrapper
[ "$(nth 4)" = "injected" ] && ok "session scope: a new session re-arms injection" || bad "new session stayed vanilla: $(nth 4)"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "session scope: latch cleared on the new session" || bad "latch survived into the new session"
[ "$(count)" = "0" ] && ok "session scope: fail count does not carry across sessions" || bad "fail count carried across sessions: $(count)"

# --- UPGRADE: a latch written before session scoping existed must re-arm -------
# Installs upgrading from the previous wrapper carry a latch with a still-valid
# payload fingerprint and no session marker at all. That is the shape that left
# users unhooked across reboots, so it must read as "belongs to a past session".
export SLSM_GUARD_SESSION_ID=session-D
reset_state
export SLSM_GUARD_MAX_FAILS=1
run_wrapper
bs=$(( $(date +%s) - 30 )); touch -d "@$bs" "$GUARD_DIR/last_launch"; touch -d "@$(( bs + 12 ))" "$DUMPS/crash_upgrade.dmp"
run_wrapper                                              # real latch, real fingerprint
export SLSM_GUARD_MAX_FAILS=3
rm -f "$GUARD_DIR/session_id"                            # pre-upgrade state layout
run_wrapper
[ "$(nth 3)" = "injected" ] && ok "upgrade: a latch with no session marker re-arms injection" || bad "pre-upgrade latch survived: $(nth 3)"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "upgrade: pre-upgrade latch cleared" || bad "pre-upgrade latch still present"

# --- a launch with NO DISPLAY says nothing about the injected stack -----------
# Steam started without a display segfaults inside libX11 (XQueryExtension) and
# writes a normal crash_*.dmp - vanilla Steam does it too, so it is not evidence
# that injection broke anything. Counting it let scripted or remote launches
# latch recovery mode for a graphical session that was never even started.
export SLSM_GUARD_SESSION_ID=session-C
reset_state
export SLSM_GUARD_MAX_FAILS=1
saved_display="$DISPLAY"; saved_wl="${WAYLAND_DISPLAY:-}"; unset DISPLAY WAYLAND_DISPLAY
run_wrapper                                              # launch 1: headless
bs=$(( $(date +%s) - 30 )); touch -d "@$bs" "$GUARD_DIR/last_launch"; touch -d "@$(( bs + 12 ))" "$DUMPS/crash_headless.dmp"
export DISPLAY="$saved_display"
[ -n "$saved_wl" ] && export WAYLAND_DISPLAY="$saved_wl"
run_wrapper                                              # launch 2: desktop, assesses launch 1
export SLSM_GUARD_MAX_FAILS=3
[ "$(count)" = "0" ] && ok "crash from a launch with no display is not counted" || bad "headless crash was counted: $(count)"
[ ! -f "$GUARD_DIR/safe_mode" ] && ok "no latch from a headless crash" || bad "latched on a headless crash"
[ "$(nth 2)" = "injected" ] && ok "headless history does not fall back a desktop launch" || bad "desktop launch fell back: $(nth 2)"
export SLSM_GUARD_SESSION_ID=harness-session

# --- signed pattern refresh stays outside the warm launch critical path -------
PATTERN_HELPER="$SLSDIR/pattern-refresh"
PATTERN_ORDER_LOG="$HOME_DIR/pattern-order.log"
export PATTERN_ORDER_LOG
cat > "$PATTERN_HELPER" <<'PH'
#!/bin/sh
case " $* " in
	*" --cache-only "*)
		echo "cache-only" >> "$PATTERN_ORDER_LOG"
		[ "${SLSM_TEST_PATTERN_CACHE:-miss}" = hit ] && exit 0
		exit 3
		;;
	*)
		echo "remote-start" >> "$PATTERN_ORDER_LOG"
		sleep "${SLSM_TEST_PATTERN_DELAY:-0}"
		echo "remote-done" >> "$PATTERN_ORDER_LOG"
		exit 0
		;;
esac
PH
chmod +x "$PATTERN_HELPER"
mkdir -p "$HOME_DIR/.steam/steam/ubuntu12_32"
printf 'client-pattern-fixture' > "$HOME_DIR/.steam/steam/ubuntu12_32/steamclient.so"
printf 'ui-pattern-fixture' > "$HOME_DIR/.steam/steam/ubuntu12_32/steamui.so"

reset_state
: > "$PATTERN_ORDER_LOG"
export SLSM_TEST_PATTERN_CACHE=hit SLSM_TEST_PATTERN_DELAY=2
pattern_started="$(date +%s%N)"
run_wrapper
pattern_elapsed_ms=$(( ($(date +%s%N) - pattern_started) / 1000000 ))
[ "$pattern_elapsed_ms" -lt 1000 ] \
  && ok "warm signed cache does not wait for remote revalidation" \
  || bad "warm signed cache delayed Steam by ${pattern_elapsed_ms}ms"
[ "$(sed -n '1p' "$PATTERN_ORDER_LOG")" = "cache-only" ] \
  && grep -q '^steam$' "$PATTERN_ORDER_LOG" \
  && ! grep -q '^remote-done$' "$PATTERN_ORDER_LOG" \
  && ok "warm cache launches Steam before background revalidation completes" \
  || bad "warm cache launch order was: $(tr '\n' ' ' < "$PATTERN_ORDER_LOG")"
for _wait in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do
	grep -q '^remote-done$' "$PATTERN_ORDER_LOG" && break
	sleep 0.1
done
grep -q '^remote-done$' "$PATTERN_ORDER_LOG" \
  && ok "warm cache revalidates remotely in the background" \
  || bad "background remote revalidation did not finish"

reset_state
: > "$PATTERN_ORDER_LOG"
export SLSM_TEST_PATTERN_CACHE=miss SLSM_TEST_PATTERN_DELAY=0.3
pattern_started="$(date +%s%N)"
run_wrapper
pattern_elapsed_ms=$(( ($(date +%s%N) - pattern_started) / 1000000 ))
[ "$(tr '\n' ' ' < "$PATTERN_ORDER_LOG")" = "cache-only remote-start remote-done steam " ] \
  && ok "cache miss finishes bounded remote refresh before Steam" \
  || bad "cache miss launch order was: $(tr '\n' ' ' < "$PATTERN_ORDER_LOG")"
[ "$pattern_elapsed_ms" -ge 250 ] && [ "$pattern_elapsed_ms" -lt 1500 ] \
  && ok "cache miss waits only for the bounded remote refresh" \
  || bad "cache miss wait was ${pattern_elapsed_ms}ms"
unset SLSM_TEST_PATTERN_CACHE SLSM_TEST_PATTERN_DELAY PATTERN_ORDER_LOG
rm -f "$PATTERN_HELPER"

if [ "${SLSM_KEEP_TEST_TMP:-0}" = 1 ]; then
	echo "test artifacts kept at $HOME_DIR"
else
	rm -rf "$HOME_DIR"
fi

echo "== total: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
