#!/bin/sh
# SLSsteam wrapper - injects via LD_AUDIT (rtld-audit).
SLSDIR="$HOME/.local/share/SLSsteam"
LD_AUDIT="$SLSDIR/SLSsteam.so${LD_AUDIT:+:$LD_AUDIT}" exec /usr/games/steam "$@"
