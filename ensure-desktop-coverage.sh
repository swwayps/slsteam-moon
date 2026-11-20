#!/usr/bin/env bash
# Thin CLI over desktop-coverage.lib.sh. Installed to $SLSDIR; invoked by the
# wrapper/Lumen (--user) and the user service (--guardian). Both run the
# guardian's user pass; --user never fails a launch, --guardian reports its
# failures. setup.sh patches the system layer in process.
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
: "${WRAPPER:=$HOME/.local/share/SLSsteam/path/steam}"
# Prefer the shared launcher eligibility check when this helper was installed
# with it. Tests/diagnostics may inject DC_STEAM_ROOT or LS_STEAM_ROOT; without
# an override, production still requires Valve's canonical Steam symlink.
if [ -n "${DC_STEAM_ROOT:-}" ] && [ -z "${LS_STEAM_ROOT:-}" ]; then
	LS_STEAM_ROOT="$DC_STEAM_ROOT"
fi
# shellcheck source=/dev/null
if [ -f "$SELF_DIR/launcher-shim.lib.sh" ]; then
	. "$SELF_DIR/launcher-shim.lib.sh"
elif [ -f "$SELF_DIR/tools/launcher-shim.lib.sh" ]; then
	. "$SELF_DIR/tools/launcher-shim.lib.sh"
fi
DC_STEAM_INSTALLED=0
if command -v ls_steam_bootstrapped >/dev/null 2>&1; then
	if ls_steam_bootstrapped; then
		DC_STEAM_INSTALLED=1
	fi
else
	# Compatibility path for older installations that shipped only the
	# desktop helper. Never infer installation from an executable package
	# launcher: the Install Steam stub is executable before bootstrap.
	_dc_steam_root="${LS_STEAM_ROOT:-${DC_STEAM_ROOT:-}}"
	if [ -z "$_dc_steam_root" ] && [ -L "$HOME/.steam/steam" ]; then
		_dc_steam_root="$(readlink -e -q "$HOME/.steam/steam" 2>/dev/null || true)"
	fi
	if [ -n "$_dc_steam_root" ] && [ -f "$_dc_steam_root/steam.sh" ] && \
	   [ -x "$_dc_steam_root/steam.sh" ]; then
		DC_STEAM_INSTALLED=1
	fi
fi
export DC_STEAM_INSTALLED WRAPPER
# shellcheck source=/dev/null
if [ -f "$SELF_DIR/desktop-coverage.lib.sh" ]; then
	. "$SELF_DIR/desktop-coverage.lib.sh"
else
	. "$SELF_DIR/tools/desktop-coverage.lib.sh"
fi
# Unit convergence is optional: old/partial installations may not have shipped
# this helper yet, while user desktop reconciliation must continue to work.
if [ -f "$SELF_DIR/desktop-guardian-units.lib.sh" ]; then
	. "$SELF_DIR/desktop-guardian-units.lib.sh"
elif [ -f "$SELF_DIR/tools/desktop-guardian-units.lib.sh" ]; then
	. "$SELF_DIR/tools/desktop-guardian-units.lib.sh"
fi

# A pass whose inputs are unchanged returns in milliseconds (see
# dc_coverage_unchanged in the lib). `--force` after the mode always does the
# full reconciliation — used by an explicit repair, and by anything that cannot
# assume the shipped coverage logic is the one that recorded the digest.
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || exit 2
case "${2:-}" in
	'') : ;;
	--force) DC_FORCE=1; export DC_FORCE ;;
	*) exit 2 ;;
esac
case "$1" in
	--user)
		dc_guardian_run || true
		# Also converge the generated-autostart drop-in on the per-launch path.
		# The wrapper/Lumen call this every injected launch, so even when the
		# systemd user units are inert (installed while systemctl --user was
		# unreachable), a single injected launch still creates the cold-boot
		# closure. Best-effort: never fail a launch.
		if command -v dgu_install_autostart_dropins >/dev/null 2>&1; then
			dgu_install_autostart_dropins || true
		fi
		exit 0
		;;
	--guardian)
		dc_guardian_run
		guardian_status=$?
		[ "$guardian_status" -eq 0 ] || exit "$guardian_status"
		# Keep watched XDG paths and generated-autostart closure current when a
		# source appears or the effective XDG layout changes after setup. Avoid
		# starting this oneshot recursively when refreshing its own unit files.
		if command -v dgu_install_units >/dev/null 2>&1; then
			DGU_NO_SERVICE_START=1 dgu_install_units || true
			dgu_install_autostart_dropins || true
		fi
		exit 0
		;;
	*) exit 2 ;;
esac
