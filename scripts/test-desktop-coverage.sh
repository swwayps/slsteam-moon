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

# desktop shortcut: patch an EXISTING one as a regular trusted file (NOT a
# symlink — GNOME renders a symlinked .desktop as "steam.desktop" + untrusted),
# and NEVER create one the user did not have.
mkdir -p "$TMP/apps" "$TMP/desk"
printf '[Desktop Entry]\n%s\nName=Steam\nExec=%s %%U\n' "$DC_TAG" "$WRAPPER" > "$TMP/apps/steam.desktop"
# (a) absent shortcut -> not created
dc_patch_shortcut "$TMP/desk/steam.desktop"
check "shortcut not created when absent" "no" "$([ -e "$TMP/desk/steam.desktop" ] && echo yes || echo no)"
# (b) existing vanilla shortcut -> patched regular file (not a symlink), exec bit
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$TMP/desk/steam.desktop"
dc_patch_shortcut "$TMP/desk/steam.desktop"
check "existing shortcut patched" "patched" "$(dc_classify "$TMP/desk/steam.desktop")"
check "shortcut is a regular file (not symlink)" "yes" "$([ -f "$TMP/desk/steam.desktop" ] && [ ! -L "$TMP/desk/steam.desktop" ] && echo yes || echo no)"
check "shortcut exec bit set" "yes" "$([ -x "$TMP/desk/steam.desktop" ] && echo yes || echo no)"

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

# MIGRATION: a legacy already-tagged entry left 0711 with a Valve shebang must be
# normalized to 0644 + clean first line on a re-run (the Cinnamon-bug fix path).
H5="$TMP/home5"; mkdir -p "$H5/.local/share/applications"
printf '#!/usr/bin/env xdg-open\n[Desktop Entry]\n%s\nName=Steam\nExec=%s %%U\n' "$DC_TAG" "$WRAPPER" \
  > "$H5/.local/share/applications/steam.desktop"
chmod 0711 "$H5/.local/share/applications/steam.desktop"
DC_HOME="$H5" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "migrate: still patched" "patched" "$(dc_classify "$H5/.local/share/applications/steam.desktop")"
check "migrate: 0711 -> 0644" "644" "$(stat -c '%a' "$H5/.local/share/applications/steam.desktop")"
check "migrate: shebang stripped" "[Desktop Entry]" "$(head -1 "$H5/.local/share/applications/steam.desktop")"

# MIGRATION 2: a legacy entry left mode 000/unreadable (root-owned 0711 in the
# field) must be made readable + patched, not skipped as "unrelated".
H6="$TMP/home6"; mkdir -p "$H6/.local/share/applications"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H6/.local/share/applications/steam.desktop"
chmod 000 "$H6/.local/share/applications/steam.desktop"
DC_HOME="$H6" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "migrate unreadable: patched" "patched" "$(dc_classify "$H6/.local/share/applications/steam.desktop")"
check "migrate unreadable: 0644" "644" "$(stat -c '%a' "$H6/.local/share/applications/steam.desktop")"

DC_HOME="$H" DC_SYS_APPS="$SYS" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --system
check "system run patches stub (steam installed)" "patched" "$(dc_classify "$SYS/steam.desktop")"

# CLI: --user runs without error against a fake HOME and patches the menu entry
H2="$TMP/home2"; mkdir -p "$H2/.local/share/applications"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H2/.local/share/applications/steam.desktop"
HOME="$H2" DC_HOME="$H2" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" WRAPPER="$WRAPPER" \
  bash "$HERE/ensure-desktop-coverage.sh" --user >/dev/null 2>&1
check "CLI --user patches menu entry" "patched" "$(dc_classify "$H2/.local/share/applications/steam.desktop")"

# restore: dc_run then dc_restore_all -> entries back to vanilla, backups consumed
H4="$TMP/home4"; mkdir -p "$H4/.local/share/applications" "$H4/.config/autostart" "$H4/Desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H4/.local/share/applications/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam -silent %%U\n' > "$H4/.config/autostart/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H4/Desktop/steam.desktop"
DC_HOME="$H4" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "before restore: menu patched" "patched" "$(dc_classify "$H4/.local/share/applications/steam.desktop")"
check "before restore: shortcut patched (regular file)" "patched" "$(dc_classify "$H4/Desktop/steam.desktop")"
DC_HOME="$H4" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_restore_all
check "restore: menu entry not patched" "0" "$(grep -c "$DC_TAG" "$H4/.local/share/applications/steam.desktop" 2>/dev/null | head -1)"
check "restore: menu backup consumed" "no" "$([ -f "$H4/.local/share/applications/steam.desktop.slssteam-backup" ] && echo yes || echo no)"
check "restore: autostart not patched" "0" "$(grep -c "$DC_TAG" "$H4/.config/autostart/steam.desktop" 2>/dev/null | head -1)"
check "restore: shortcut restored to vanilla regular file" "0" "$(grep -c "$DC_TAG" "$H4/Desktop/steam.desktop" 2>/dev/null | head -1)"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
