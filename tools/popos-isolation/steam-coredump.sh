#!/bin/sh
# Launches Steam under SLSsteam (LD_AUDIT) with core dumps enabled so we can
# get a precise backtrace of the SIGILL in the IPC:CSteamEngine thread.
ulimit -c unlimited
SLSDIR="$HOME/.local/share/SLSsteam"
LD_AUDIT="$SLSDIR/SLSsteam.so${LD_AUDIT:+:$LD_AUDIT}" exec /usr/games/steam "$@"
