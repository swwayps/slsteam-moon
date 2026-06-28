#!/usr/bin/env bash
# Host unit tests for desktop-coverage.lib.sh. Runs in a throwaway tmp tree.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DC_TAG="X-SLSteamMoon-Patched=true"
WRAPPER="/tmp/slsfake/path/steam"
# shellcheck source=/dev/null
. "$HERE/tools/desktop-coverage.lib.sh"

fail=0
check() { # desc expected actual
  if [ "$2" = "$3" ]; then printf 'ok   - %s\n' "$1"
  else printf 'FAIL - %s (want=[%s] got=[%s])\n' "$1" "$2" "$3"; fail=1; fi
}

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# classify: real launcher
cat > "$TMP/real.desktop" <<EOF
[Desktop Entry]
Name=Steam
Exec=/usr/games/steam %U
EOF
check "classify real launcher" "launcher" "$(dc_classify "$TMP/real.desktop")"

# classify: Install Steam stub
cat > "$TMP/stub.desktop" <<EOF
[Desktop Entry]
Name=Install Steam
Exec=/usr/games/steam %U
EOF
check "classify install-steam stub" "stub" "$(dc_classify "$TMP/stub.desktop")"

# classify: already patched
cat > "$TMP/patched.desktop" <<EOF
[Desktop Entry]
$DC_TAG
Name=Steam
Exec=$WRAPPER %U
EOF
check "classify already-patched" "patched" "$(dc_classify "$TMP/patched.desktop")"

# classify: unrelated
cat > "$TMP/other.desktop" <<EOF
[Desktop Entry]
Name=Steamy Tool
Exec=/usr/bin/steamy
EOF
check "classify unrelated (no steam launcher exec)" "unrelated" "$(dc_classify "$TMP/other.desktop")"

# strip pre-header (Valve shebang)
printf '#!/usr/bin/env xdg-open\n[Desktop Entry]\nName=Steam\n' > "$TMP/sh.desktop"
dc_strip_preheader "$TMP/sh.desktop"
check "strip shebang -> first line is [Desktop Entry]" "[Desktop Entry]" "$(head -1 "$TMP/sh.desktop")"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
