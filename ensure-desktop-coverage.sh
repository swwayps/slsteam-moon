#!/bin/sh
# Thin CLI over desktop-coverage.lib.sh. Installed to $SLSDIR; invoked by the
# wrapper (each launch, --user), by Lumen (--user on inject/exit and the
# autostart-only tick), and by setup.sh (--system at install). Best-effort:
# never fails a launch.
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
dc_run "${1:---user}"
