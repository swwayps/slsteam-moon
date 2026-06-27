#!/bin/bash

SLSDIR="$HOME/.local/share/SLSsteam"
SLSLIB="$SLSDIR/SLSsteam.so"

# User-local applications dir (XDG override always wins over system-wide).
USER_APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
USER_DESKTOP="$USER_APPS/steam.desktop"
SYS_DESKTOP="/usr/share/applications/steam.desktop"

# XDG autostart: some images (SteamOS/Bazzite) auto-launch Steam on desktop
# login via /etc/xdg/autostart/steam.desktop (which calls the distro launcher,
# bypassing our wrapper). A user-level entry of the same basename overrides it.
USER_AUTOSTART="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/steam.desktop"
SYS_AUTOSTART="/etc/xdg/autostart/steam.desktop"

# Tag we drop into patched .desktop files so we can detect/undo them later.
SLSM_TAG="X-SLSteamMoon-Patched=true"

# ============================================================================
# Pretty output (colors + box-drawing). Falls back to plain text when stdout
# is not a TTY or the terminal does not advertise colour support.
#
# Palette: "moonlit night" — cool blues for structure, silver-white for the
# moon glyph, standard semantic colours for status. Uses 256-colour escapes
# when the terminal supports them, otherwise degrades to 8-colour ANSI.
# ============================================================================

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != "dumb" ]; then
	# Detect 256-colour support. tput is the reliable check; default to true
	# if tput is unavailable but COLORTERM looks modern.
	if command -v tput >/dev/null 2>&1 && [ "$(tput colors 2>/dev/null || echo 0)" -ge 256 ]; then
		HAS_256=1
	elif [ "${COLORTERM:-}" = "truecolor" ] || [ "${COLORTERM:-}" = "24bit" ]; then
		HAS_256=1
	else
		HAS_256=0
	fi

	BOLD=$'\033[1m'
	DIM=$'\033[2m'
	NC=$'\033[0m'

	if [ "$HAS_256" = 1 ]; then
		MOON=$'\033[38;5;153m'
		NIGHT=$'\033[38;5;75m'
		HALO=$'\033[38;5;231m'
		MUTED=$'\033[38;5;110m'
		GREEN=$'\033[38;5;114m'
		YELLOW=$'\033[38;5;221m'
		RED=$'\033[38;5;203m'
	else
		MOON=$'\033[1;34m'
		NIGHT=$'\033[0;36m'
		HALO=$'\033[1;37m'
		MUTED=$'\033[0;34m'
		GREEN=$'\033[0;32m'
		YELLOW=$'\033[0;33m'
		RED=$'\033[0;31m'
	fi
else
	BOLD=""; DIM=""; NC=""
	MOON=""; NIGHT=""; HALO=""; MUTED=""
	GREEN=""; YELLOW=""; RED=""
fi

print_banner() {
	echo ""
	echo -e "${MOON}${BOLD}"
	echo "┌─────────────────────────────────────────────────────────┐"
	printf "│             ${HALO}${BOLD}◯${NC}${MOON}${BOLD}  slsteam-moon installer                   │\n"
	echo "└─────────────────────────────────────────────────────────┘"
	echo -e "${NC}"
}

print_section() {
	echo ""
	echo -e "${NIGHT}─────────────────────────────────────────────────────────${NC}"
	echo -e "${NIGHT}${BOLD}❯ $1${NC}"
	echo -e "${NIGHT}─────────────────────────────────────────────────────────${NC}"
}

log_info()    { echo -e "${NIGHT}→${NC} $1"; }
log_success() { echo -e "${GREEN}✓${NC} $1"; }
log_warn()    { echo -e "${YELLOW}⚠${NC} $1"; }
log_error()   { echo -e "${RED}✗${NC} $1"; }
log_step()    { echo -e "${MOON}•${NC} $1"; }

print_install_complete() {
	echo ""
	echo -e "${GREEN}${BOLD}"
	echo "┌─────────────────────────────────────────────────────────┐"
	echo "│        ✓ slsteam-moon Installation Completed!           │"
	echo "└─────────────────────────────────────────────────────────┘"
	echo -e "${NC}"
}

print_uninstall_complete() {
	echo ""
	echo -e "${GREEN}${BOLD}"
	echo "┌─────────────────────────────────────────────────────────┐"
	echo "│               ✓ Uninstall Complete!                     │"
	echo "└─────────────────────────────────────────────────────────┘"
	echo -e "${NC}"
	echo ""
	echo -e "   Restart your terminal and Steam for changes to take effect."
	echo ""
}

# ============================================================================
# Detection helpers
# ============================================================================

# Find the real Steam binary. Distros vary:
#   /usr/games/steam        Debian, Ubuntu, Mint (steam-installer)
#   /usr/bin/steam          Arch, Fedora, openSUSE, Manjaro, Pop!_OS
#   /usr/local/bin/steam    manual installs
detect_steam_binary() {
	local c
	for c in /usr/games/steam /usr/bin/steam /usr/local/bin/steam; do
		if [ -x "$c" ]; then
			echo "$c"
			return 0
		fi
	done
	command -v steam 2>/dev/null
}

# Tell the Mint/Debian "steam-installer" stub apart from a real Steam launcher.
# The stub has Name=Install Steam.
is_real_steam_desktop() {
	local f="$1"
	[ -f "$f" ] || return 1
	# Already patched by us — treat as real (don't recurse).
	grep -q "$SLSM_TAG" "$f" 2>/dev/null && return 0
	# Stub installer, skip it.
	grep -q "^Name=Install Steam" "$f" 2>/dev/null && return 1
	# Heuristic: any Exec= line that runs steam directly.
	grep -qE "^Exec=.*((^| |\")steam( |\$|%)|/steam( |\$|%)|/games/steam|/bin/steam)" "$f" 2>/dev/null
}

# Already patched to use our wrapper?
is_patched_desktop() {
	[ -f "$1" ] && grep -q "$SLSM_TAG" "$1" 2>/dev/null
}

# Locate a "donor" .desktop to seed the user-local override from when the user
# doesn't already have one. Tries, in order: existing user-local (real), the
# system-wide entry (real), and the bundle Steam itself ships under
# ~/.steam/.../steam-launcher/.
find_donor_desktop() {
	local c
	if is_real_steam_desktop "$USER_DESKTOP"; then
		echo "$USER_DESKTOP"; return 0
	fi
	if is_real_steam_desktop "$SYS_DESKTOP"; then
		echo "$SYS_DESKTOP"; return 0
	fi
	for c in \
		"$HOME/.steam/steam/steam-launcher/steam.desktop" \
		"$HOME/.steam/debian-installation/deb-installer/steam-launcher/steam.desktop" \
		"$HOME/.steam/debian-installation/deb-installer/steam.desktop"; do
		if is_real_steam_desktop "$c"; then
			echo "$c"; return 0
		fi
	done
	return 1
}

# ============================================================================
# Steam process management
# ============================================================================

# Stop any running Steam so the wrapper / desktop entry takes effect on next
# launch. Tries graceful shutdown first, falls back to SIGTERM then SIGKILL.
kill_steam() {
	if ! pgrep -x steam >/dev/null 2>&1 \
	   && ! pgrep -f '/steam$|/steam ' >/dev/null 2>&1 \
	   && ! pgrep -f 'steamwebhelper' >/dev/null 2>&1; then
		log_success "No running Steam process detected"
		return 0
	fi

	log_info "Stopping running Steam processes"

	if command -v steam >/dev/null 2>&1; then
		steam -shutdown >/dev/null 2>&1 || true
	fi

	for _ in 1 2 3 4 5; do
		if ! pgrep -x steam >/dev/null 2>&1 \
		   && ! pgrep -f 'steamwebhelper' >/dev/null 2>&1; then
			log_success "Steam stopped"
			return 0
		fi
		sleep 1
	done

	pkill -TERM -x steam 2>/dev/null || true
	pkill -TERM -f 'steamwebhelper' 2>/dev/null || true
	pkill -TERM -f '/steam$|/steam ' 2>/dev/null || true
	sleep 2

	if pgrep -x steam >/dev/null 2>&1 \
	   || pgrep -f 'steamwebhelper' >/dev/null 2>&1 \
	   || pgrep -f '/steam$|/steam ' >/dev/null 2>&1; then
		log_warn "Steam still running — sending SIGKILL"
		pkill -KILL -x steam 2>/dev/null || true
		pkill -KILL -f 'steamwebhelper' 2>/dev/null || true
		pkill -KILL -f '/steam$|/steam ' 2>/dev/null || true
		sleep 1
	fi

	log_success "Steam stopped"
}

# ============================================================================
# Install steps
# ============================================================================

install_slssteam()
{
	LIB="./bin/SLSsteam.so"

	if [ ! -f "$LIB" ]; then
		log_error "$LIB not found"
		echo ""
		echo "   If you're a developer, build it first:"
		echo -e "     ${GREEN}./build-docker.sh${NC}  ${MUTED}# For releases (requires Podman/Docker)${NC}"
		echo -e "     ${GREEN}make${NC}               ${MUTED}# For local testing${NC}"
		echo ""
		exit 1
	fi

	log_info "Installing SLSsteam libraries"
	mkdir -p "$SLSDIR" || exit 1
	cp -v ./bin/* "$SLSDIR/" | sed "s|^|   ${MUTED}${NC}|"
	log_success "Libraries installed at $SLSDIR"
	echo ""
}

create_steam_wrapper()
{
	log_info "Creating Steam wrapper with SLSsteam injection"

	mkdir -p "$SLSDIR/path"

	# The wrapper resolves the real Steam binary at runtime so the install is
	# portable across distros (and survives moves between, e.g., a Debian-style
	# /usr/games/steam and an Arch-style /usr/bin/steam).
	cat > "$SLSDIR/path/steam" << 'EOF'
#!/bin/sh
# slsteam-moon wrapper. Injects SLSsteam via LD_AUDIT (rtld-audit) so the audit
# namespace can't interpose on Steam's own copies of protobuf / yaml-cpp /
# libstdc++. library-inject.so redirects libcurl to the system copy and must
# come first. CloudRedirect (cloud saves) is injected separately via LD_PRELOAD
# (see its block below).
SLSDIR="$HOME/.local/share/SLSsteam"

# Resolve the real Steam binary, skipping our own wrapper.
SELF="$(readlink -f "$0" 2>/dev/null || echo "$0")"
STEAM_BIN=""
# Override hook: an explicit, executable path wins. Lets power users pin a
# specific Steam binary and lets the wrapper guard's unit test inject a fake
# Steam; a no-op when unset.
if [ -n "${SLSM_STEAM_BIN:-}" ] && [ -x "${SLSM_STEAM_BIN:-}" ] && \
   [ "$(readlink -f "$SLSM_STEAM_BIN" 2>/dev/null || echo "$SLSM_STEAM_BIN")" != "$SELF" ]; then
	STEAM_BIN="$SLSM_STEAM_BIN"
fi
if [ -z "$STEAM_BIN" ]; then
	for c in /usr/games/steam /usr/bin/steam /usr/local/bin/steam; do
		if [ -x "$c" ] && [ "$(readlink -f "$c" 2>/dev/null || echo "$c")" != "$SELF" ]; then
			STEAM_BIN="$c"
			break
		fi
	done
fi
if [ -z "$STEAM_BIN" ]; then
	# Fall back to PATH lookup, but skip ourselves.
	IFS=:
	for d in $PATH; do
		c="$d/steam"
		if [ -x "$c" ] && [ "$(readlink -f "$c" 2>/dev/null || echo "$c")" != "$SELF" ]; then
			STEAM_BIN="$c"
			break
		fi
	done
	unset IFS
fi
if [ -z "$STEAM_BIN" ]; then
	echo "slsteam-moon: could not find the real Steam binary" >&2
	exit 127
fi

# ---------------------------------------------------------------------------
# Crash-loop fail-safe (Game Mode boot protection).
#
# In a gamescope "Game Mode" session the supervisor relaunches Steam every time
# it exits. If our injected stack (SLSsteam via LD_AUDIT / CloudRedirect via
# LD_PRELOAD / the Lumen sidecar) goes incompatible with a freshly-updated Steam
# client and stalls the engine, the device loops on the splash forever and the
# user can never reach Desktop Mode to update. This guard counts boots that
# CRASHED AT STARTUP (Steam wrote an assert/crash minidump within the boot's
# first few minutes) and, past a threshold, LATCHES into a safe mode that starts
# Steam completely vanilla (no LD_AUDIT, no LD_PRELOAD, no sidecar) so the
# session comes up. We deliberately key off the crash dump and NOT merely a
# short-lived session: switching Game Mode <-> Desktop, a client self-update
# restart, or a quick manual quit all end Steam fast but are NOT failures, and a
# clean kill never writes a dump. As a fast path, a startup crash whose
# steamclient.so differs from the last cleanly-booted one latches on the FIRST
# crash (a fresh client is the near-certain cause), so the user is not made to
# loop MAX_FAILS times for a known-cause break; a crash on an UNCHANGED client
# keeps the conservative MAX_FAILS threshold. Session/distro-agnostic: ChimeraOS/Bazzite
# (sessions.d STEAMCMD), SteamOS (steam-launcher PATH drop-in) and Desktop Mode
# all funnel through this one wrapper. Every step is best-effort and can never
# itself block the launch.
GUARD_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/slsteam-moon"
mkdir -p "$GUARD_DIR" 2>/dev/null || true
GUARD_LAST="$GUARD_DIR/last_launch"          # mtime = start of the most recent boot
GUARD_COUNT="$GUARD_DIR/boot_fail_count"
GUARD_SAFE="$GUARD_DIR/safe_mode"            # present => stay vanilla
GUARD_FP="$GUARD_DIR/safe_mode_fingerprint"  # payload id captured when latched
GUARD_CLIENT_LAST="$GUARD_DIR/last_client"   # steamclient.so id the most recent boot ran
GUARD_CLIENT_GOOD="$GUARD_DIR/good_client"   # steamclient.so id of the last boot that started cleanly
GUARD_LOG="$GUARD_DIR/guard.log"

# Tunables (overridable for testing).
[ -n "${SLSM_GUARD_MAX_FAILS:-}" ] || SLSM_GUARD_MAX_FAILS=3
[ -n "${SLSM_GUARD_STARTUP_SECS:-}" ] || SLSM_GUARD_STARTUP_SECS=180
[ -n "${SLSM_GUARD_DUMPS_DIR:-}" ] || SLSM_GUARD_DUMPS_DIR="/tmp/dumps"

guard_log() {
	printf '%s %s\n' "$(date '+%F %T' 2>/dev/null)" "$1" >> "$GUARD_LOG" 2>/dev/null || true
}
# Non-blocking, auto-dismissing notice (10s, normal urgency). NOT critical:
# the freedesktop spec lets the shell pin critical notifications on screen with
# no timeout (KDE does), which is why an earlier critical recovery notice never
# went away. Best-effort; never blocks the launch.
guard_notify() {
	if command -v notify-send >/dev/null 2>&1; then
		notify-send -u normal -t 10000 "Steam recovery mode" "$1" >/dev/null 2>&1 || true
	fi
}
guard_read_int() {
	_v="$(cat "$1" 2>/dev/null)"
	case "$_v" in ''|*[!0-9]*) printf 0 ;; *) printf '%s' "$_v" ;; esac
}
# Identify the Steam client library (size:mtime of the 32-bit steamclient.so we
# hook). A client self-update rewrites it, changing this id - the signal that a
# startup crash is a compatibility break rather than a one-off. Empty when no
# client is found yet (fresh install): callers then stay conservative.
guard_client_fp() {
	for _r in "$HOME/.steam/steam" "$HOME/.steam/debian-installation" "$HOME/.local/share/Steam"; do
		_c="$_r/ubuntu12_32/steamclient.so"
		if [ -e "$_c" ]; then
			stat -c '%s:%Y' "$_c" 2>/dev/null || stat -f '%z:%m' "$_c" 2>/dev/null || printf '?'
			return 0
		fi
	done
	printf ''
}
# Fingerprint the injected payload (size:mtime of each piece). Updating ANY of
# it - which the plugin does on reinstall/update - changes this, so a stuck
# safe-mode latch auto-clears once the user has updated.
guard_fingerprint() {
	for _f in "$SLSDIR/SLSsteam.so" \
	          "$HOME/.local/share/CloudRedirect/cloud_redirect.so" \
	          "$HOME/.local/share/Lumen/lumen"; do
		if [ -e "$_f" ]; then
			stat -c '%s:%Y' "$_f" 2>/dev/null || stat -f '%z:%m' "$_f" 2>/dev/null || printf '?'
		else
			printf -- '-'
		fi
		printf '|'
	done
}
GUARD_CUR_FP="$(guard_fingerprint)"

# True when Steam wrote a crash/assert minidump during the boot that started at
# epoch $2 (marker file $1), within that boot's first STARTUP_SECS. This is the
# definitive "Steam crashed before it became usable" signal. A clean kill (Game
# Mode <-> Desktop switch, shutdown) or a quick manual quit does NOT write a
# dump, so those never count as failures - which is why we use this instead of a
# bare "the session was short" heuristic.
guard_startup_crash() {
	[ -d "$SLSM_GUARD_DUMPS_DIR" ] || return 1
	case "$2" in ''|*[!0-9]*) return 1 ;; esac
	[ "$2" -gt 0 ] || return 1
	_ref="$GUARD_DIR/.crash_win_ref"
	touch -d "@$(( $2 + SLSM_GUARD_STARTUP_SECS ))" "$_ref" 2>/dev/null || { rm -f "$_ref" 2>/dev/null; return 1; }
	_hit="$(find "$SLSM_GUARD_DUMPS_DIR" -maxdepth 1 -name '*.dmp' -newer "$1" ! -newer "$_ref" 2>/dev/null | head -n1)"
	rm -f "$_ref" 2>/dev/null
	[ -n "$_hit" ]
}

# Already latched? Stay vanilla until the payload changes (user updated).
if [ -f "$GUARD_SAFE" ]; then
	if [ "$(cat "$GUARD_FP" 2>/dev/null)" = "$GUARD_CUR_FP" ]; then
		guard_log "safe mode active -> launching Steam without injection"
		exec "$STEAM_BIN" "$@"
	fi
	guard_log "payload changed since latch -> clearing safe mode, retrying injection"
	rm -f "$GUARD_SAFE" "$GUARD_FP" "$GUARD_COUNT" "$GUARD_LAST" 2>/dev/null || true
fi

# Assess the PREVIOUS boot: it failed only if Steam crashed at startup (wrote a
# minidump in its first STARTUP_SECS). A short but clean session is NOT a
# failure. (find/touch/stat failing degrade to "ok", so the guard never latches
# by accident.)
GUARD_FAILS="$(guard_read_int "$GUARD_COUNT")"
GUARD_CLIENT_CUR="$(guard_client_fp)"
guard_client_changed=0
if [ -f "$GUARD_LAST" ]; then
	_then="$(stat -c %Y "$GUARD_LAST" 2>/dev/null || stat -f %m "$GUARD_LAST" 2>/dev/null || echo 0)"
	if guard_startup_crash "$GUARD_LAST" "$_then"; then
		GUARD_FAILS=$(( GUARD_FAILS + 1 ))
		# If the client that just crashed differs from the last client we saw
		# boot cleanly, a fresh client update is almost certainly the cause -
		# recover on the FIRST crash instead of making the user sit through
		# MAX_FAILS loops. A one-off crash on an UNCHANGED client keeps the
		# conservative threshold (it is far more likely transient/unrelated).
		_prev_client="$(cat "$GUARD_CLIENT_LAST" 2>/dev/null || true)"
		_good_client="$(cat "$GUARD_CLIENT_GOOD" 2>/dev/null || true)"
		if [ -n "$_good_client" ] && [ "$_prev_client" != "$_good_client" ]; then
			guard_client_changed=1
			guard_log "steamclient.so changed since last clean boot -> first startup crash treated as a compatibility break"
		fi
		guard_log "previous boot crashed at startup -> fail ${GUARD_FAILS}/${SLSM_GUARD_MAX_FAILS}"
	else
		[ "$GUARD_FAILS" -ne 0 ] && guard_log "previous boot ok (no startup crash) -> reset fail count"
		GUARD_FAILS=0
		# Remember the client that just booted cleanly as the known-good baseline.
		_prev_client="$(cat "$GUARD_CLIENT_LAST" 2>/dev/null || true)"
		[ -n "$_prev_client" ] && printf '%s' "$_prev_client" > "$GUARD_CLIENT_GOOD" 2>/dev/null || true
	fi
fi
printf '%s' "$GUARD_FAILS" > "$GUARD_COUNT" 2>/dev/null || true

if [ "$GUARD_FAILS" -ge "$SLSM_GUARD_MAX_FAILS" ] || { [ "$guard_client_changed" = 1 ] && [ "$GUARD_FAILS" -ge 1 ]; }; then
	guard_log "fail count ${GUARD_FAILS} (client_changed=${guard_client_changed}) -> latching safe mode, launching vanilla Steam"
	printf '%s' "$GUARD_CUR_FP" > "$GUARD_FP" 2>/dev/null || true
	: > "$GUARD_SAFE" 2>/dev/null || true
	# Drop the half-injected appinfo.vdf so vanilla Steam rebuilds a clean one;
	# the abandoned splice (written at preinit before the hook abort) is what
	# stalls the engine. Steam regenerates appinfo.vdf on next launch.
	for _r in "$HOME/.steam/steam" "$HOME/.steam/debian-installation" "$HOME/.local/share/Steam"; do
		if [ -f "$_r/appcache/appinfo.vdf" ]; then
			rm -f "$_r/appcache/appinfo.vdf" 2>/dev/null && guard_log "removed $_r/appcache/appinfo.vdf"
		fi
	done
	guard_notify "slsteam-moon is paused because Steam failed to start after a recent update. Steam is running normally - open Desktop Mode and update the plugin to re-enable it."
	guard_log "recovery mode latched; Steam will launch unhooked until the payload is updated"
	exec "$STEAM_BIN" "$@"
fi

# Mark the start of THIS boot for the next invocation's health check, and record
# the client this boot is about to run so the next assessment can tell whether
# the client changed across a crash.
: > "$GUARD_LAST" 2>/dev/null || true
printf '%s' "$GUARD_CLIENT_CUR" > "$GUARD_CLIENT_LAST" 2>/dev/null || true

# CloudRedirect (optional): inject its 32-bit cloud-save hook via LD_PRELOAD.
# Our bundled build is CloudRedirect 2.1.5 (correct save restore via
# StripCasShaLeaf) with the steamclient.so wait extended 10s -> 120s so it
# attaches on slow-bootstrap distros (Arch/CachyOS) too. It is a plain
# LD_PRELOAD library: loading it as an LD_AUDIT auditor corrupts the client
# heap (realloc(): invalid pointer) during init, so it must NOT go in the
# LD_AUDIT list. SLSsteam stays on LD_AUDIT (library-inject.so first).
# CloudRedirect's constructor self-removes itself from LD_PRELOAD so child
# processes (the game, steamwebhelper) don't inherit it.
CR_SO="$HOME/.local/share/CloudRedirect/cloud_redirect.so"
if [ -f "$CR_SO" ]; then
	export LD_PRELOAD="$CR_SO${LD_PRELOAD:+:$LD_PRELOAD}"
fi

# extest (Steam Input on Wayland): SteamOS and Bazzite ship libextest, an X11
# XTEST shim that lets the Steam Controller / Steam Input drive the desktop
# cursor under Wayland (X11 has native XTEST, so it's only needed on Wayland).
# Their own desktop launchers (bazzite-steam / steam-jupiter) LD_PRELOAD it on
# Wayland. We launch the Steam binary directly — on purpose, so our LD_AUDIT and
# CloudRedirect survive (those launchers do `env LD_PRELOAD=...`, which REPLACES
# the list and would drop cloud_redirect.so) — so we replicate just this one
# preload, generically: only on Wayland and only when the lib actually exists
# (a no-op on distros that don't ship it).
if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
	for _ext in /usr/lib/extest/libextest.so \
	            /usr/lib64/extest/libextest.so \
	            /usr/lib/x86_64-linux-gnu/extest/libextest.so; do
		if [ -f "$_ext" ]; then
			export LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$_ext"
			break
		fi
	done
fi

AUDIT="$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so"

LD_AUDIT="$AUDIT${LD_AUDIT:+:$LD_AUDIT}" exec "$STEAM_BIN" "$@"
EOF

	chmod +x "$SLSDIR/path/steam"

	log_success "Steam wrapper created at $SLSDIR/path/steam"
	echo ""
	return 0
}

# Patch every Exec= line in $1 (in-place) so it runs through our wrapper, then
# stamp our marker. Writes a backup to $1.slssteam-backup if one isn't there.
#
# Each Exec= line is rewritten by swapping its LAUNCHER TOKEN for our wrapper
# while preserving an optional `env VAR=val …` prefix and all trailing
# arguments / field codes (e.g. "-silent %U", "steam://store"). The launcher
# token is the first word that is neither `env` nor a `VAR=val` assignment, so
# this is launcher-path-agnostic: it handles /usr/bin/steam, /opt/steam/steam,
# /usr/lib/steam/steam, a bare `steam`, bazzite-steam, steam-jupiter, etc. —
# not just a few hard-coded paths. Desktop Action lines (steam:// handlers) are
# rewritten the same way (wrapper + the steam:// arg), which the wrapper
# forwards to Steam.
#
# The marker is stamped ONLY when at least one Exec= was actually pointed at
# the wrapper. Stamping a file we failed to rewrite would make every later run
# treat it as already-patched (is_patched_desktop) and skip it forever, leaving
# the launcher pointing at vanilla Steam — the root cause of "the .desktop
# isn't patched / it redirects to default Steam, but `steam` from the console
# works". Returns 0 when the file ends up pointing at the wrapper, 1 otherwise.
patch_desktop_file() {
	local f="$1"
	local backup="$f.slssteam-backup"
	local wrapper="$SLSDIR/path/steam"
	local sudo_cmd="${2:-}"

	# Backup once.
	if [ ! -f "$backup" ]; then
		$sudo_cmd cp -- "$f" "$backup"
	fi

	# Rewrite the launcher token of every Exec= line and drop any stale marker.
	# awk handles the tokenisation so we avoid sed path-escaping pitfalls and
	# the previous "Exec=steam steam://" mangling.
	local tmp
	tmp="$(mktemp)"
	WRAPPER="$wrapper" TAG="$SLSM_TAG" awk '
		$0 == ENVIRON["TAG"] { next }                 # drop stale marker line
		/^Exec=/ {
			rest = substr($0, 6)                       # text after "Exec="
			n = split(rest, t, " ")
			swapped = 0
			out = "Exec="
			for (i = 1; i <= n; i++) {
				if (!swapped && t[i] != "env" && index(t[i], "=") == 0) {
					t[i] = ENVIRON["WRAPPER"]; swapped = 1
				}
				out = out t[i] (i < n ? " " : "")
			}
			print out
			next
		}
		{ print }
	' "$f" > "$tmp"

	# Only stamp + commit if an Exec= now runs our wrapper.
	if grep -qF "Exec=$wrapper" "$tmp" 2>/dev/null || grep -qF " $wrapper" "$tmp" 2>/dev/null; then
		if grep -q '^\[Desktop Entry\]' "$tmp" 2>/dev/null; then
			sed -i "0,/^\[Desktop Entry\]/ s|^\[Desktop Entry\]\$|[Desktop Entry]\n$SLSM_TAG|" "$tmp"
		else
			printf '%s\n' "$SLSM_TAG" >> "$tmp"
		fi
		$sudo_cmd cp -- "$tmp" "$f"
		rm -f "$tmp"
		return 0
	fi

	# Nothing rewritten — leave the file (and its untouched marker state) alone.
	rm -f "$tmp"
	return 1
}

# Looser variant of is_real_steam_desktop for autostart entries: the SteamOS/
# Bazzite autostart calls a distro launcher (bazzite-steam / steam-jupiter) that
# is_real_steam_desktop's "/steam"-anchored regex doesn't match, so accept any
# Exec= that mentions steam (still skipping the "Install Steam" stub and our own
# already-patched file).
is_autostart_steam_desktop() {
	local f="$1"
	[ -f "$f" ] || return 1
	grep -q "$SLSM_TAG" "$f" 2>/dev/null && return 0
	grep -q "^Name=Install Steam" "$f" 2>/dev/null && return 1
	grep -qiE '^Exec=.*steam' "$f" 2>/dev/null
}

# Rewrite ONLY the first (primary [Desktop Entry]) Exec= line so its launcher
# token becomes our wrapper, keeping the original arguments (e.g. "-silent %U").
# Distro-agnostic: works whatever the launcher is (bazzite-steam, steam-jupiter,
# /usr/bin/steam). Desktop Action Exec lines (steam:// URL handlers) are left
# pointing at the distro launcher on purpose — they're one-shot forwarders.
rewrite_primary_exec_to_wrapper() {
	local f="$1" esc_wrapper
	esc_wrapper=$(printf '%s' "$SLSDIR/path/steam" | sed -e 's/[\/&]/\\&/g')
	sed -i "0,/^Exec=/ s|^Exec=[^ ]*\(.*\)\$|Exec=$esc_wrapper\1|" "$f"
}

# Mirror an existing Steam autostart entry as a user-level XDG override that runs
# through our wrapper, so Steam is injected even when the desktop session
# auto-launches it (SteamOS/Bazzite). We ONLY act when an autostart entry
# already exists (user or system) — we never CREATE autostart where the user had
# none, so normal desktops are unaffected.
setup_autostart_override() {
	if is_patched_desktop "$USER_AUTOSTART"; then
		log_success "Autostart override already patched ($USER_AUTOSTART)"
		return 0
	fi

	local donor=""
	if is_autostart_steam_desktop "$USER_AUTOSTART"; then
		donor="$USER_AUTOSTART"            # user's own; back up + patch in place
	elif is_autostart_steam_desktop "$SYS_AUTOSTART"; then
		donor="$SYS_AUTOSTART"             # system autostart; seed a user override
	fi
	[ -n "$donor" ] || return 0            # no existing autostart -> no-op

	mkdir -p "$(dirname "$USER_AUTOSTART")"
	if [ "$donor" = "$USER_AUTOSTART" ]; then
		[ -f "$USER_AUTOSTART.slssteam-backup" ] || cp -- "$USER_AUTOSTART" "$USER_AUTOSTART.slssteam-backup"
	else
		log_info "Seeding $USER_AUTOSTART from $donor"
		cp -- "$donor" "$USER_AUTOSTART"
	fi

	rewrite_primary_exec_to_wrapper "$USER_AUTOSTART"
	# Drop any stale marker, then tag once after the [Desktop Entry] header.
	sed -i "/^$SLSM_TAG\$/d" "$USER_AUTOSTART"
	if grep -q '^\[Desktop Entry\]' "$USER_AUTOSTART" 2>/dev/null; then
		sed -i "0,/^\[Desktop Entry\]/ s|^\[Desktop Entry\]\$|[Desktop Entry]\n$SLSM_TAG|" "$USER_AUTOSTART"
	else
		echo "$SLSM_TAG" >> "$USER_AUTOSTART"
	fi
	log_success "Patched Steam autostart override: $USER_AUTOSTART"
}

setup_path_and_desktop()
{
	log_info "Setting up PATH and desktop integration"

	# --- Shell PATH integration -------------------------------------------
	local rc found=0
	for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
		[ -f "$rc" ] || continue
		if grep -q "SLSsteam/path" "$rc" 2>/dev/null; then
			log_success "Already in $(basename "$rc")"
		else
			{
				echo ''
				echo '# SLSsteam: Add wrapper to PATH'
				echo 'export PATH="$HOME/.local/share/SLSsteam/path:$PATH"'
			} >> "$rc"
			log_success "Added wrapper to $(basename "$rc")"
		fi
		found=1
	done
	if [ "$found" = 0 ]; then
		# Create a .bashrc if no shell rc files exist (rare but possible).
		echo 'export PATH="$HOME/.local/share/SLSsteam/path:$PATH"' > "$HOME/.bashrc"
		log_success "Created ~/.bashrc with wrapper PATH"
	fi

	# --- Detect Steam binary ----------------------------------------------
	local steam_bin
	steam_bin="$(detect_steam_binary)"
	if [ -z "$steam_bin" ]; then
		log_warn "Steam doesn't appear to be installed yet"
		log_warn "Install Steam first, then re-run: ./setup.sh install"
		return 0
	fi
	log_success "Found Steam binary at $steam_bin"

	# --- User-local override (XDG: always wins over system-wide) ----------
	mkdir -p "$USER_APPS"

	if is_patched_desktop "$USER_DESKTOP"; then
		log_success "User .desktop already patched ($USER_DESKTOP)"
	else
		local donor=""
		if is_real_steam_desktop "$USER_DESKTOP"; then
			# User already had their own; back it up and patch in place.
			donor="$USER_DESKTOP"
		else
			donor="$(find_donor_desktop)"
		fi

		if [ -n "$donor" ]; then
			if [ "$donor" != "$USER_DESKTOP" ]; then
				log_info "Seeding $USER_DESKTOP from $donor"
				cp -- "$donor" "$USER_DESKTOP"
			fi
			patch_desktop_file "$USER_DESKTOP"
			log_success "Patched user .desktop: $USER_DESKTOP"
		else
			# No donor available — generate a minimal launcher so the menu
			# entry at least works.
			log_info "No existing steam.desktop found; writing a minimal launcher"
			cat > "$USER_DESKTOP" << EOF
[Desktop Entry]
$SLSM_TAG
Name=Steam
Comment=Application for managing and playing games on Steam
Exec=$SLSDIR/path/steam %U
Icon=steam
Terminal=false
Type=Application
Categories=Network;FileTransfer;Game;
MimeType=x-scheme-handler/steam;x-scheme-handler/steamlink;
PrefersNonDefaultGPU=true
EOF
			log_success "Created $USER_DESKTOP"
		fi
	fi

	# Refresh XDG cache so launchers/menus pick up the change without a logout.
	if command -v update-desktop-database >/dev/null 2>&1; then
		update-desktop-database "$USER_APPS" >/dev/null 2>&1 || true
	fi

	# --- System-wide patch (best-effort) ----------------------------------
	# Belt-and-braces: also patch /usr/share/applications when it's the real
	# launcher. We don't strictly need it (XDG picks the user-local copy
	# first), but it covers oddball launchers that read system entries only.
	if [ -f "$SYS_DESKTOP" ] && is_real_steam_desktop "$SYS_DESKTOP" && ! is_patched_desktop "$SYS_DESKTOP"; then
		if command -v sudo >/dev/null 2>&1; then
			log_info "Patching system .desktop (requires sudo): $SYS_DESKTOP"
			patch_desktop_file "$SYS_DESKTOP" sudo
			log_success "Patched system .desktop"
			if command -v update-desktop-database >/dev/null 2>&1; then
				sudo update-desktop-database "/usr/share/applications" >/dev/null 2>&1 || true
			fi
		else
			log_warn "sudo not available; skipping system-wide .desktop patch"
		fi
	elif is_patched_desktop "$SYS_DESKTOP"; then
		log_success "System .desktop already patched"
	fi

	# --- Autostart override (SteamOS/Bazzite desktop auto-launch) ---------
	# Ensures injection even when the desktop session auto-starts Steam
	# (otherwise the user has to manually restart Steam to get injected).
	setup_autostart_override

	echo ""
	return 0
}

install_steamstub()
{
	TARGET="$1"
	HELPERSRC="./tools/steamstub-bypass"

	if [ ! -d "$HELPERSRC" ]; then
		log_warn "Helper scripts not found at $HELPERSRC — skipping Steam Stub setup"
		return 1
	fi

	log_info "Installing Steamless helper"
	mkdir -p "$TARGET/steamstub-bypass"
	cp -v "$HELPERSRC/run-steamless.sh"     "$TARGET/steamstub-bypass/"
	cp -v "$HELPERSRC/install-steamless.sh" "$TARGET/steamstub-bypass/"
	cp -v "$HELPERSRC/scan-all.sh"          "$TARGET/steamstub-bypass/"
	chmod u+x "$TARGET/steamstub-bypass/run-steamless.sh" \
	          "$TARGET/steamstub-bypass/install-steamless.sh" \
	          "$TARGET/steamstub-bypass/scan-all.sh"

	echo ""

	# Prefer the Steamless kit bundled in the release (offline, no
	# network).  Only fall back to the GitHub download if the bundle is
	# absent (e.g. a dev tree).  The download path is the historical
	# silent-failure source behind "Application load error 6".
	if [ -f "./tools/steamless-bin/Steamless.CLI.exe" ]; then
		log_info "Installing bundled Steamless kit"
		mkdir -p "$TARGET/steamless-bin"
		cp -r ./tools/steamless-bin/. "$TARGET/steamless-bin/"
	else
		log_warn "No bundled Steamless kit; fetching from upstream (needs internet)"
		bash "$TARGET/steamstub-bypass/install-steamless.sh" \
			--target "$TARGET/steamless-bin" || true
	fi

	# Verify the kit actually landed.  A missing CLI means the SteamStub
	# bypass is silently disabled at runtime and DRM-locked games fail
	# with "Application load error 6" — make that loud here.
	if [ -f "$TARGET/steamless-bin/Steamless.CLI.exe" ]; then
		log_success "Steamless ready ($TARGET/steamless-bin)"
	else
		log_warn "Steamless kit NOT installed — games with Steam DRM will"
		log_warn "fail to launch (Application load error 6). To fix later:"
		log_warn "  bash $TARGET/steamstub-bypass/install-steamless.sh --target $TARGET/steamless-bin"
	fi
	echo ""
}

install_all()
{
	print_banner

	print_section "Stopping Steam"
	kill_steam

	print_section "Installing libraries"
	install_slssteam

	print_section "Creating Steam wrapper"
	create_steam_wrapper

	print_section "Configuring PATH & desktop entry"
	setup_path_and_desktop

	print_section "Installing Steamless helper"
	install_steamstub "$SLSDIR"

	print_install_complete
}

# ============================================================================
# Uninstall
# ============================================================================

restore_or_remove_desktop() {
	local f="$1"
	local backup="$f.slssteam-backup"
	local sudo_cmd="${2:-}"

	if [ ! -f "$f" ]; then
		return 0
	fi
	if ! is_patched_desktop "$f"; then
		return 0
	fi

	if [ -f "$backup" ]; then
		log_info "Restoring $f from backup"
		$sudo_cmd cp -- "$backup" "$f"
		$sudo_cmd rm -- "$backup"
		log_success "Restored $f"
	else
		log_info "Removing $f (no backup found)"
		$sudo_cmd rm -- "$f"
		log_success "Removed $f"
	fi
}

uninstall()
{
	print_banner
	print_section "Uninstalling SLSsteam"

	kill_steam

	# Remove from shell rc files.
	local rc
	for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
		[ -f "$rc" ] || continue
		if grep -q "SLSsteam/path" "$rc" 2>/dev/null; then
			log_info "Cleaning wrapper PATH entry from $(basename "$rc")"
			sed -i '/# SLSsteam: Add wrapper to PATH/d' "$rc"
			sed -i '\|SLSsteam/path|d' "$rc"
		fi
	done

	# User-local .desktop.
	restore_or_remove_desktop "$USER_DESKTOP"
	if command -v update-desktop-database >/dev/null 2>&1; then
		update-desktop-database "$USER_APPS" >/dev/null 2>&1 || true
	fi

	# System-wide .desktop (only if we actually patched it).
	if [ -f "$SYS_DESKTOP" ] && (is_patched_desktop "$SYS_DESKTOP" || grep -q "SLSsteam" "$SYS_DESKTOP" 2>/dev/null); then
		if command -v sudo >/dev/null 2>&1; then
			log_info "Restoring system .desktop (requires sudo)"
			restore_or_remove_desktop "$SYS_DESKTOP" sudo
			if command -v update-desktop-database >/dev/null 2>&1; then
				sudo update-desktop-database "/usr/share/applications" >/dev/null 2>&1 || true
			fi
		else
			log_warn "sudo not available; cannot restore $SYS_DESKTOP automatically"
		fi
	fi

	# Legacy: /usr/games/steam patch from older versions.
	if [ -f "/usr/games/steam" ] && grep -q "SLSsteam" "/usr/games/steam" 2>/dev/null; then
		log_info "Found legacy /usr/games/steam modification"
		if [ -f "/usr/games/steam.slsteam-backup" ]; then
			log_info "Restoring original /usr/games/steam (requires sudo)"
			sudo cp "/usr/games/steam.slsteam-backup" "/usr/games/steam"
			sudo rm "/usr/games/steam.slsteam-backup"
			log_success "Restored /usr/games/steam"
		else
			log_warn "Legacy modification found but no backup exists"
		fi
	fi

	if [ -d "$SLSDIR" ]; then
		log_info "Removing $SLSDIR"
		rm -rf "$SLSDIR"
	fi

	print_uninstall_complete
}

# ============================================================================
# Entry point
# ============================================================================

if [[ $# -lt 1 ]]; then
	print_banner
	echo -e "${BOLD}Usage:${NC}  $0 ${GREEN}install${NC} | ${GREEN}uninstall${NC}"
	echo ""
	exit 0
fi

if [ "$1" == "install" ]; then
	install_all
elif [ "$1" == "uninstall" ]; then
	uninstall
else
	log_error "Unknown command: $1"
	echo -e "${BOLD}Usage:${NC}  $0 ${GREEN}install${NC} | ${GREEN}uninstall${NC}"
	exit 1
fi
