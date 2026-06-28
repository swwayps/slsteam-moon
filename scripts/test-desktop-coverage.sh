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

# rewrite Exec: primary + Desktop Action lines, launcher-token-agnostic
cat > "$TMP/rw.desktop" <<EOF
[Desktop Entry]
Name=Steam
Exec=/usr/games/steam %U
[Desktop Action Store]
Exec=steam steam://store
EOF
dc_rewrite_exec "$TMP/rw.desktop"
check "primary Exec -> wrapper" "Exec=$WRAPPER %U" "$(grep -m1 '^Exec=' "$TMP/rw.desktop")"
check "action Exec -> wrapper keeps args" "Exec=$WRAPPER steam://store" "$(grep '^Exec=' "$TMP/rw.desktop" | sed -n 2p)"

# env-prefixed launcher token is handled
cat > "$TMP/env.desktop" <<EOF
[Desktop Entry]
Exec=env VAR=1 /usr/bin/steam %U
EOF
dc_rewrite_exec "$TMP/env.desktop"
check "env-prefixed Exec -> wrapper after env" "Exec=env VAR=1 $WRAPPER %U" "$(grep -m1 '^Exec=' "$TMP/env.desktop")"

# patch_one: backup made, tag added, mode 0644, Exec -> wrapper, shebang stripped
printf '#!/usr/bin/env xdg-open\n[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$TMP/menu.desktop"
chmod 0711 "$TMP/menu.desktop"
dc_patch_one "$TMP/menu.desktop"
check "patch_one classify after -> patched" "patched" "$(dc_classify "$TMP/menu.desktop")"
check "patch_one mode 0644" "644" "$(stat -c '%a' "$TMP/menu.desktop")"
check "patch_one backup exists" "yes" "$([ -f "$TMP/menu.desktop.slssteam-backup" ] && echo yes || echo no)"
check "patch_one first line clean" "[Desktop Entry]" "$(head -1 "$TMP/menu.desktop")"
check "patch_one Exec wrapped" "Exec=$WRAPPER %U" "$(grep -m1 '^Exec=' "$TMP/menu.desktop")"

# desktop shortcut becomes a symlink to the patched menu entry (Steam skips symlinks)
mkdir -p "$TMP/apps" "$TMP/desk"
printf '[Desktop Entry]\n%s\nName=Steam\nExec=%s %%U\n' "$DC_TAG" "$WRAPPER" > "$TMP/apps/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$TMP/desk/steam.desktop"
dc_symlink_shortcut "$TMP/desk/steam.desktop" "$TMP/apps/steam.desktop"
check "shortcut is now a symlink" "yes" "$([ -L "$TMP/desk/steam.desktop" ] && echo yes || echo no)"
check "shortcut points at patched menu entry" "$TMP/apps/steam.desktop" "$(readlink "$TMP/desk/steam.desktop")"

# dc_run --user patches menu + autostart but NOT the stub; --system also stub
H="$TMP/home"; mkdir -p "$H/.local/share/applications" "$H/.config/autostart"
SYS="$TMP/sys"; mkdir -p "$SYS"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H/.local/share/applications/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam -silent %%U\n' > "$H/.config/autostart/steam.desktop"
printf '[Desktop Entry]\nName=Install Steam\nExec=/usr/games/steam %%U\n' > "$SYS/steam.desktop"
DC_HOME="$H" DC_SYS_APPS="$SYS" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
check "user run patches menu" "patched" "$(dc_classify "$H/.local/share/applications/steam.desktop")"
check "user run patches autostart" "patched" "$(dc_classify "$H/.config/autostart/steam.desktop")"
check "user run leaves stub alone" "stub" "$(dc_classify "$SYS/steam.desktop")"
DC_HOME="$H" DC_SYS_APPS="$SYS" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --system
check "system run patches stub (steam installed)" "patched" "$(dc_classify "$SYS/steam.desktop")"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
