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

# Desktop-coverage logic (scan + patch every *steam*.desktop, blindagens,
# restore) lives in one sourceable lib shared with the wrapper and Lumen. Source
# the in-repo copy at install time so dc_run / dc_restore_all are available here.
WRAPPER="$SLSDIR/path/steam"
DC_TAG="$SLSM_TAG"
DC_BACKUP_ROOT="$SLSDIR/backup"
SETUP_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SETUP_DIR/tools/desktop-coverage.lib.sh" ]; then
	# shellcheck source=/dev/null
	. "$SETUP_DIR/tools/desktop-coverage.lib.sh"
fi
if [ -f "$SETUP_DIR/tools/desktop-guardian-units.lib.sh" ]; then
	# shellcheck source=/dev/null
	. "$SETUP_DIR/tools/desktop-guardian-units.lib.sh"
fi

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

# True on immutable / atomic systems (Bazzite, SteamOS, Fedora Atomic/ublue, …)
# where /usr is read-only: we must NOT attempt the system-wide .desktop patch
# (it would prompt for sudo then fail silently). User-level entries override the
# system ones via XDG precedence, so --user fully covers the normal launchers.
is_immutable_distro() {
	local id="" like="" variant=""
	if [ -r /etc/os-release ]; then
		# shellcheck disable=SC1091
		. /etc/os-release 2>/dev/null || true
		id="${ID:-}"; like="${ID_LIKE:-}"; variant="${VARIANT_ID:-}"
	fi
	case " $id $like " in
		*" bazzite "*|*" steamos "*|*" steamdeck "*|*" holoiso "*|\
		*" silverblue "*|*" kinoite "*|*" sericea "*|*" onyx "*|\
		*" bluefin "*|*" aurora "*|*" ucore "*) return 0 ;;
	esac
	# Fedora Atomic variants advertise via VARIANT_ID even when ID=fedora.
	case "$variant" in silverblue|kinoite|sericea|onyx|*atomic*) return 0 ;; esac
	command -v rpm-ostree >/dev/null 2>&1 && return 0
	command -v steamos-readonly >/dev/null 2>&1 && return 0
	# Last resort: a read-only root mount (ostree deployments mount / ro).
	if command -v findmnt >/dev/null 2>&1; then
		case ",$(findmnt -no OPTIONS / 2>/dev/null)," in *,ro,*) return 0 ;; esac
	fi
	return 1
}

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
	# Desktop-coverage helper (scan + patch all *steam*.desktop). Shipped in the
	# release; copied next to the wrapper so the wrapper and Lumen can invoke it.
	install -m 0644 ./tools/desktop-coverage.lib.sh "$SLSDIR/desktop-coverage.lib.sh" 2>/dev/null || \
		cp ./tools/desktop-coverage.lib.sh "$SLSDIR/desktop-coverage.lib.sh"
	install -m 0644 ./tools/desktop-guardian-units.lib.sh "$SLSDIR/desktop-guardian-units.lib.sh" 2>/dev/null || \
		cp ./tools/desktop-guardian-units.lib.sh "$SLSDIR/desktop-guardian-units.lib.sh"
	install -m 0755 ./ensure-desktop-coverage.sh    "$SLSDIR/ensure-desktop-coverage.sh" 2>/dev/null || \
		{ cp ./ensure-desktop-coverage.sh "$SLSDIR/ensure-desktop-coverage.sh"; chmod +x "$SLSDIR/ensure-desktop-coverage.sh"; }
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

# True when Steam wrote a FATAL crash minidump during the boot that started at
# epoch $2 (marker file $1), within that boot's first STARTUP_SECS. This is the
# definitive "Steam crashed before it became usable" signal. A clean kill (Game
# Mode <-> Desktop switch, shutdown) or a quick manual quit does NOT write a
# dump, so those never count as failures - which is why we use this instead of a
# bare "the session was short" heuristic.
#
# Match ONLY crash_*.dmp (fatal: segfault/abort that takes the client down).
# Steam also writes assert_*.dmp for NON-FATAL assertions (e.g. CloudRedirect's
# cloud-save path-resolution asserts in remotestoragefilesynccontext.cpp) while
# it keeps running perfectly fine, and those land in the same /tmp/dumps within
# the startup window. A bare '*.dmp' glob counted those as startup crashes and,
# coinciding with a Steam client self-update (client-changed -> latch on first
# crash), wrongly paused the hook on a healthy desktop boot.
guard_startup_crash() {
	[ -d "$SLSM_GUARD_DUMPS_DIR" ] || return 1
	case "$2" in ''|*[!0-9]*) return 1 ;; esac
	[ "$2" -gt 0 ] || return 1
	_ref="$GUARD_DIR/.crash_win_ref"
	touch -d "@$(( $2 + SLSM_GUARD_STARTUP_SECS ))" "$_ref" 2>/dev/null || { rm -f "$_ref" 2>/dev/null; return 1; }
	_hit="$(find "$SLSM_GUARD_DUMPS_DIR" -maxdepth 1 -name 'crash_*.dmp' -newer "$1" ! -newer "$_ref" 2>/dev/null | head -n1)"
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
	guard_notify "slsteam-moon is paused because Steam failed to start after a recent update. Steam is running normally - update the plugin from the LuaTools menu to re-enable it."
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

# Lumen (millennium-less LuaTools bridge): restore Steam's verified index and
# stage an active theme OUTSIDE Steam's tree, then enable CEF debugging and
# launch the sidecar detached. SLSsteam publishes that staged bootstrap at the
# steamwebhelper exec boundary, after verification and before first paint. The
# synchronous preflight opens no socket; disabled/default users get no -dev,
# no native publish, and no theme runtime. Single-instance guarded.
LUMEN_DIR="$HOME/.local/share/Lumen"
LUMEN_THEME_DEV=0
unset LUMEN_THEME_PRELOAD_ACTIVE LUMEN_THEME_STAGING_DIR LUMEN_STEAMUI_DIR
if [ -x "$LUMEN_DIR/lumen" ]; then
	_lumen_preflight_rc=0
	env -u LD_AUDIT -u LD_PRELOAD -u LD_LIBRARY_PATH \
	    LUMEN_THEME_PRELOAD_ONLY=1 \
	    LUMEN_LUA_DIR="$LUMEN_DIR/lua" \
	    "$LUMEN_DIR/lumen" >/dev/null 2>&1 </dev/null || _lumen_preflight_rc=$?
	[ "$_lumen_preflight_rc" -eq 10 ] && LUMEN_THEME_DEV=1
	if [ "$LUMEN_THEME_DEV" -eq 1 ]; then
		export LUMEN_THEME_PRELOAD_ACTIVE=1
		export LUMEN_THEME_STAGING_DIR="$LUMEN_DIR/theme-preload"
		for _lumen_root in "$HOME/.steam/steam" \
		                   "$HOME/.steam/debian-installation" \
		                   "$HOME/.local/share/Steam"; do
			if [ -f "$_lumen_root/steamui/index.html" ]; then
				export LUMEN_STEAMUI_DIR="$_lumen_root/steamui"
				break
			fi
		done
		# Unknown/non-native Steam layout: retain the CDP fallback instead of
		# claiming the native gate is armed when it has nowhere safe to publish.
		if [ -z "${LUMEN_STEAMUI_DIR:-}" ]; then
			LUMEN_THEME_DEV=0
			unset LUMEN_THEME_PRELOAD_ACTIVE LUMEN_THEME_STAGING_DIR
		fi
	fi
	touch "$HOME/.steam/steam/.cef-enable-remote-debugging" 2>/dev/null || true
	touch "$HOME/.steam/debian-installation/.cef-enable-remote-debugging" 2>/dev/null || true
	if ! pgrep -f "$LUMEN_DIR/lumen" >/dev/null 2>&1; then
		env -u LD_AUDIT -u LD_PRELOAD -u LD_LIBRARY_PATH \
		    LUMEN_THEME_PRELOAD_ACTIVE="$LUMEN_THEME_DEV" \
		    LUMEN_BACKEND_DIR="$LUMEN_DIR/luatools/backend" \
		    LUMEN_LUA_DIR="$LUMEN_DIR/lua" \
		    setsid "$LUMEN_DIR/lumen" >/dev/null 2>&1 < /dev/null &
	fi
fi

AUDIT="$SLSDIR/library-inject.so:$SLSDIR/SLSsteam.so"

# Steam normally serves SteamUI from its packed web archive and ignores loose
# index.html overrides.  -dev is the client's own opt-in for loose SteamUI
# files. Add it only when the preflight confirmed a custom theme; disabled and
# default-theme users keep the exact normal launch arguments and packed path.
if [ "$LUMEN_THEME_DEV" -eq 1 ]; then
	case " $* " in *" -dev "*) ;; *) set -- -dev "$@" ;; esac
fi

# Re-assert desktop-entry coverage without putting reconciliation on the launch
# critical path. Prefer the serialized guardian; retain the legacy CLI fallback
# during upgrades or on desktops without a working user manager.
if command -v systemctl >/dev/null 2>&1 && \
   systemctl --user --quiet is-enabled slsteam-desktop-guardian.path >/dev/null 2>&1; then
	systemctl --user start slsteam-desktop-guardian.service >/dev/null 2>&1 &
elif [ -x "$SLSDIR/ensure-desktop-coverage.sh" ]; then
	# Lowest priority we can give it: this fallback runs concurrently with Steam's
	# own start, and a pass that does have work to do is seconds of shell CPU.
	if command -v nice >/dev/null 2>&1; then
		WRAPPER="$SLSDIR/path/steam" nice -n 10 "$SLSDIR/ensure-desktop-coverage.sh" --user >/dev/null 2>&1 &
	else
		WRAPPER="$SLSDIR/path/steam" "$SLSDIR/ensure-desktop-coverage.sh" --user >/dev/null 2>&1 &
	fi
fi

LD_AUDIT="$AUDIT${LD_AUDIT:+:$LD_AUDIT}" exec "$STEAM_BIN" "$@"
EOF

	chmod +x "$SLSDIR/path/steam"

	log_success "Steam wrapper created at $SLSDIR/path/steam"
	echo ""
	return 0
}

# NOTE: the user-level autostart override (mirror the system/Steam autostart entry
# so the desktop session's auto-launch of Steam runs through our wrapper) now lives
# in the shared coverage lib as dc_seed_autostart_override, invoked from dc_run —
# so it runs at install AND on every per-launch/Lumen re-assert, and covers ANY
# distro that auto-starts Steam via /etc/xdg/autostart (SteamOS/Bazzite and beyond),
# not just at install time. The old setup_autostart_override/is_autostart_steam_desktop/
# rewrite_primary_exec_to_wrapper helpers were removed as dead code.

setup_path_and_desktop()
{
	log_info "Setting up PATH and desktop integration"
	local system_desktop_changed=0 user_desktop_changed=0

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

	# --- Mandatory user coverage, then optional system fallback -------------
	mkdir -p "$USER_APPS"
	export DC_STEAM_INSTALLED=1
	mkdir -p "$DC_BACKUP_ROOT" 2>/dev/null || {
		log_error "Could not create desktop backup directory: $DC_BACKUP_ROOT"
		return 1
	}

	# Same-ID user shadows are the authoritative layer and are reconciled before
	# any privilege prompt. A per-entry reconciliation failure is retryable (the
	# guardian timer/path retries it), so it must NOT abort installation of the
	# guardian itself — that would leave the machine with no ongoing self-healing,
	# which is exactly the "injection lost after reboot" failure.
	# DC_FORCE: installing is never a boot path, and the shipped coverage logic may
	# behave differently from the one that recorded the last digest, so the
	# unchanged-input fast path must not apply here.
	if DC_FORCE=1 dc_guardian_run; then
		log_success "Reconciled user desktop entries"
	else
		log_warn "Some user desktop entries could not be fully reconciled yet; the guardian will retry them"
	fi

	# User-manager integration is an acceleration/repair layer. Desktop shadows
	# remain functional if systemd --user is unavailable.
	local guardian_status
	dgu_install_units; guardian_status=$?
	[ "$guardian_status" = 1 ] && dgu_enable_units
	[ "$guardian_status" = 2 ] && log_warn "Could not install all desktop guardian units; user desktop coverage remains active"
	dgu_install_autostart_dropins; guardian_status=$?
	[ "$guardian_status" = 2 ] && log_warn "Could not install all generated-autostart drop-ins; XDG shadows remain active"

	# Enabling the units is best-effort (systemctl --user may be unreachable when
	# the installer is run via sudo/root or a non-graphical session). Verify it
	# actually took: silently-inert units are the top cause of injection being
	# lost on the next boot, so surface it clearly instead of reporting success.
	if command -v systemctl >/dev/null 2>&1; then
		if ! systemctl --user is-enabled slsteam-desktop-guardian.path >/dev/null 2>&1; then
			log_warn "Desktop guardian installed but NOT active (systemd --user unreachable at install time). Cold-boot injection self-healing is OFF; re-run this installer from your normal desktop session (not via sudo/root)."
		fi
	fi

	if is_immutable_distro; then
		log_info "Immutable distro (read-only /usr): using user-level coverage only; no administrator access requested."
	elif command -v sudo >/dev/null 2>&1; then
		# Mutable systems retain the historical system layer as a fallback, but
		# denying sudo no longer discards the already-working user coverage.
		if ! sudo -v; then
			log_warn "Administrator access not granted: the system-wide Steam entry stays unpatched. Menu and taskbar launches still use the injected per-user entry (it wins by desktop-file-id); only a launcher pinned by absolute path to the system file would skip the wrapper. Re-run with admin access to also cover the system entry."
		else
			dc_migrate_legacy_backups --system
			if DC_FORCE=1 dc_run --system; then
				system_desktop_changed=1
				log_success "Patched optional system Steam desktop entries"
			else
				log_warn "Optional system desktop fallback could not be fully applied"
			fi
		fi
	else
		log_warn "sudo not available: the system-wide Steam entry stays unpatched. Menu and taskbar launches still use the injected per-user entry (it wins by desktop-file-id); only a launcher pinned by absolute path to the system file would skip the wrapper."
	fi

	# Fallback: if the user still has no menu entry (no donor anywhere), write a
	# minimal patched launcher so the menu always works, then blind the shortcut.
	if [ ! -e "$USER_DESKTOP" ]; then
		log_info "Writing a minimal Steam launcher"
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
		chmod 0644 "$USER_DESKTOP"
		user_desktop_changed=1
		log_success "Created $USER_DESKTOP"
	fi

	# Guardian reconciliation already converged user caches. Refresh here only
	# for the direct minimal fallback or a successful mutable-system pass.
	if command -v update-desktop-database >/dev/null 2>&1; then
		[ "$user_desktop_changed" = 1 ] && \
			update-desktop-database "$USER_APPS" >/dev/null 2>&1 || true
		[ "$system_desktop_changed" = 1 ] && command -v sudo >/dev/null 2>&1 && \
			sudo update-desktop-database "/usr/share/applications" >/dev/null 2>&1 || true
	fi

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
	local backup="$DC_BACKUP_ROOT/${f#/}" legacy
	local sudo_cmd="${2:-}"

	[ -f "$f" ] || return 0
	if [ ! -f "$backup" ]; then
		for legacy in "$f.slssteam-backup" "$f.slsteam-bak"; do
			[ -f "$legacy" ] || continue
			mkdir -p "$(dirname "$backup")" 2>/dev/null || break
			if [ -n "$sudo_cmd" ]; then
				$sudo_cmd cat -- "$legacy" > "$backup" 2>/dev/null || { rm -f "$backup"; break; }
			else
				cp -- "$legacy" "$backup" 2>/dev/null || { rm -f "$backup"; break; }
			fi
			$sudo_cmd rm -f -- "$legacy" 2>/dev/null || true
			break
		done
	fi
	if ! is_patched_desktop "$f" && [ ! -f "$backup" ]; then
		return 0
	fi

	if [ -f "$backup" ]; then
		log_info "Restoring $f from backup"
		if $sudo_cmd cp -- "$backup" "$f"; then
			rm -f -- "$backup"
			log_success "Restored $f"
		fi
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
	local desktop_restore_complete=1

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

	# Stop generated launch paths before restoring their desktop-file sources.
	# Removal is sentinel-scoped and therefore preserves foreign unit drop-ins.
	if command -v dgu_remove_autostart_dropins >/dev/null 2>&1; then
		dgu_remove_autostart_dropins || true
	fi
	if command -v dgu_remove_units >/dev/null 2>&1; then
		dgu_remove_units || true
	fi

	# Restore every patched/symlinked *steam*.desktop (menu user+system incl. the
	# stub, ~/Desktop shortcut, autostart user+system) from their backups via the
	# shared lib. System paths use sudo when available.
	if command -v dc_restore_all >/dev/null 2>&1; then
		if is_immutable_distro || ! command -v sudo >/dev/null 2>&1; then
			DC_SUDO=""
		else
			DC_SUDO="sudo"
		fi
		export DC_SUDO
		if dc_restore_all; then
			log_success "Restored Steam desktop entries"
		else
			desktop_restore_complete=0
			log_warn "System desktop restoration is unavailable; retaining user desktop coverage and helpers for a later retry"
		fi
	else
		# Lib unavailable (older layout): fall back to the legacy per-file restore.
		restore_or_remove_desktop "$USER_DESKTOP"
		[ -f "$SYS_DESKTOP" ] && command -v sudo >/dev/null 2>&1 && \
			restore_or_remove_desktop "$SYS_DESKTOP" sudo
	fi
	if command -v update-desktop-database >/dev/null 2>&1; then
		update-desktop-database "$USER_APPS" >/dev/null 2>&1 || true
		! is_immutable_distro && command -v sudo >/dev/null 2>&1 && \
			sudo update-desktop-database "/usr/share/applications" >/dev/null 2>&1 || true
	fi

	# Legacy: /usr/games/steam patch from older versions.
	if ! is_immutable_distro && [ -f "/usr/games/steam" ] && grep -q "SLSsteam" "/usr/games/steam" 2>/dev/null; then
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

	if [ -d "$SLSDIR" ] && [ "$desktop_restore_complete" = 1 ]; then
		log_info "Removing $SLSDIR"
		rm -rf "$SLSDIR"
	elif [ -d "$SLSDIR" ]; then
		log_warn "Keeping $SLSDIR because desktop restoration is incomplete"
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
