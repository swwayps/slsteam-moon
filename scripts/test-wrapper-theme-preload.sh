#!/usr/bin/env bash
# The generated launcher must stage the active theme before Steam starts and
# pass the native publish paths into the client. Disabled/default mode must not
# add -dev or retain any native theme-gate environment.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FN="$(awk '
	/^create_steam_wrapper\(\)/ { f=1 }
	f && /<< '\''EOF'\''/ { inhd=1 }
	f { print }
	f && inhd && /^EOF$/ { inhd=0; next }
	f && !inhd && /^}/ { exit }
' "$REPO_ROOT/setup.sh")"

ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT
export HOME="$ROOT/home" XDG_STATE_HOME="$ROOT/state"
SLSDIR="$HOME/.local/share/SLSsteam"
LUMEN_DIR="$HOME/.local/share/Lumen"
mkdir -p "$SLSDIR" "$LUMEN_DIR/lua" "$HOME/.steam/steam/steamui"
printf vanilla > "$HOME/.steam/steam/steamui/index.html"
printf so > "$SLSDIR/SLSsteam.so"
printf audit > "$SLSDIR/library-inject.so"

( log_info() { :; }; log_success() { :; }; log_warn() { :; }; eval "$FN"; \
  SLSDIR="$SLSDIR" create_steam_wrapper )

ORDER="$ROOT/order"
export ORDER
cat > "$LUMEN_DIR/lumen" <<'EOF'
#!/bin/sh
if [ "${LUMEN_THEME_PRELOAD_ONLY:-}" = 1 ]; then
	printf 'preflight\n' >> "$ORDER"
	exit "${PRELOAD_STATUS:-10}"
fi
printf 'sidecar\n' >> "$ORDER"
EOF
chmod +x "$LUMEN_DIR/lumen"

FAKE_STEAM="$ROOT/steam"
cat > "$FAKE_STEAM" <<'EOF'
#!/bin/sh
printf 'steam:%s\n' "$*" >> "$ORDER"
printf 'theme:%s|%s|%s\n' "${LUMEN_THEME_PRELOAD_ACTIVE:-}" \
  "${LUMEN_THEME_STAGING_DIR:-}" "${LUMEN_STEAMUI_DIR:-}" >> "$ORDER"
EOF
chmod +x "$FAKE_STEAM"
export SLSM_STEAM_BIN="$FAKE_STEAM" SLSM_GUARD_DUMPS_DIR="$ROOT/dumps"
mkdir -p "$SLSM_GUARD_DUMPS_DIR"

sh "$SLSDIR/path/steam" >/dev/null 2>&1
sleep 0.1
first="$(sed -n '1p' "$ORDER")"
steam_line="$(grep -n '^steam:' "$ORDER" | head -n1 | cut -d: -f1)"
preflight_line="$(grep -n '^preflight$' "$ORDER" | head -n1 | cut -d: -f1)"
[ "$first" = preflight ]
[ "$preflight_line" -lt "$steam_line" ]
[ "$(grep '^steam:' "$ORDER")" = 'steam:-dev' ]
expected="theme:1|$LUMEN_DIR/theme-preload|$HOME/.steam/steam/steamui"
[ "$(grep '^theme:' "$ORDER")" = "$expected" ]

: > "$ORDER"
PRELOAD_STATUS=0 sh "$SLSDIR/path/steam" >/dev/null 2>&1
sleep 0.1
[ "$(grep '^steam:' "$ORDER")" = 'steam:' ]
[ "$(grep '^theme:' "$ORDER")" = 'theme:||' ]
echo "ok - theme preflight gates native publish without contaminating default mode"
