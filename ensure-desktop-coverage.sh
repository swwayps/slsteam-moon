#!/usr/bin/env bash
# Thin CLI over desktop-coverage.lib.sh. Installed to $SLSDIR; invoked by the
# wrapper/Lumen (--user), setup (--system), and the user service (--guardian).
# Legacy --user calls remain best-effort; guardian failures are explicit.
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
: "${WRAPPER:=$HOME/.local/share/SLSsteam/path/steam}"
# steam installed? (so the stub is eligible). Overridable for tests.
if [ -z "${DC_STEAM_INSTALLED:-}" ]; then
	DC_STEAM_INSTALLED=0
	for c in /usr/games/steam /usr/bin/steam /usr/local/bin/steam; do
		[ -x "$c" ] && { DC_STEAM_INSTALLED=1; break; }
	done
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

[ "$#" -eq 1 ] || exit 2
case "$1" in
	--user)
		dc_run --user || true
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
	--system)
		dc_run --system
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
