#!/usr/bin/env bash
# Host unit tests for desktop-coverage.lib.sh. Runs in a throwaway tmp tree.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DC_TAG="X-SLSteamMoon-Patched=true"
WRAPPER="/tmp/slsfake/path/steam"
# Keep legacy fixtures hermetic even when the host exports custom XDG roots.
unset XDG_DATA_HOME XDG_DATA_DIRS XDG_CONFIG_HOME XDG_CONFIG_DIRS XDG_DESKTOP_DIR
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

# Desktop Exec parser/rewrite matrix. Supported lines preserve their raw prefix
# and suffix; unsafe, unrelated, and malformed commands fail closed unchanged.
while IFS='|' read -r desc input supported expected; do
  [ -n "$desc" ] || continue
  if dc_exec_supported_launcher "$input"; then actual_supported=yes; else actual_supported=no; fi
  check "exec support: $desc" "$supported" "$actual_supported"
  actual_rewrite="$(dc_rewrite_exec_line "$input" 2>/dev/null || printf '%s' "$input")"
  check "exec rewrite: $desc" "$expected" "$actual_rewrite"
done <<EOF
steam absolute|/usr/bin/steam %U|yes|$WRAPPER %U
steam jupiter|steam-jupiter -silent %U|yes|$WRAPPER -silent %U
bazzite steam absolute|/usr/bin/bazzite-steam -silent %U|yes|$WRAPPER -silent %U
env assignment|env MANGOHUD=1 /usr/bin/steam %U|yes|env MANGOHUD=1 $WRAPPER %U
env quoted assignment and launcher|env "MANGOHUD=1" "/usr/bin/steam" %U|yes|env "MANGOHUD=1" $WRAPPER %U
shell wrapper|sh -c 'steam "\$@"' sh %U|no|sh -c 'steam "\$@"' sh %U
bash wrapper|bash -lc steam|no|bash -lc steam
dash wrapper|dash -c steam|no|dash -c steam
flatpak launcher|flatpak run com.valvesoftware.Steam %U|no|flatpak run com.valvesoftware.Steam %U
unrelated executable|/usr/bin/steamy %U|no|/usr/bin/steamy %U
relative executable path|relative/path/steam %U|no|relative/path/steam %U
URI-like executable|https://example/steam %U|no|https://example/steam %U
invalid unquoted escape|/usr/bin/ste\am %U|no|/usr/bin/ste\am %U
invalid quoted escape|"/usr/bin/ste\am" %U|no|"/usr/bin/ste\am" %U
invalid env assignment|env 1MANGOHUD=1 /usr/bin/steam %U|no|env 1MANGOHUD=1 /usr/bin/steam %U
malformed quote|env MANGOHUD=1 "/usr/bin/steam %U|no|env MANGOHUD=1 "/usr/bin/steam %U
EOF

# Classification uses the parser rather than filename/text substring matching.
for launcher in \
  '/usr/bin/steam %U' \
  'steam-jupiter -silent %U' \
  '/usr/bin/bazzite-steam -silent %U' \
  'env MANGOHUD=1 /usr/bin/steam %U' \
  'env "MANGOHUD=1" "/usr/bin/steam" %U'; do
  printf '[Desktop Entry]\nName=Steam\nExec=%s\n' "$launcher" > "$TMP/classify-parser.desktop"
  check "classify supported Exec: $launcher" "launcher" "$(dc_classify "$TMP/classify-parser.desktop")"
done
for launcher in \
  'sh -c '\''steam "$@"'\'' sh %U' \
  'flatpak run com.valvesoftware.Steam %U' \
  '/usr/bin/steamy %U' \
  'env MANGOHUD=1 "/usr/bin/steam %U'; do
  printf '[Desktop Entry]\nName=Steam\nExec=%s\n' "$launcher" > "$TMP/classify-parser.desktop"
  check "classify rejected Exec: $launcher" "unrelated" "$(dc_classify "$TMP/classify-parser.desktop")"
done

# Rewrite every applicable primary/Action Exec while preserving unsupported
# Desktop Action commands byte-for-byte.
cat > "$TMP/rw.desktop" <<EOF
[Desktop Entry]
Name=Steam
Exec=/usr/games/steam %U
[Desktop Action Store]
Exec=steam-jupiter steam://store
[Desktop Action Unsafe]
Exec=sh -c 'steam "\$@"' sh %U
EOF
dc_rewrite_exec "$TMP/rw.desktop"
check "primary Exec -> wrapper" "Exec=$WRAPPER %U" "$(grep -m1 '^Exec=' "$TMP/rw.desktop")"
check "supported action Exec -> wrapper keeps args" "Exec=$WRAPPER steam://store" "$(grep '^Exec=' "$TMP/rw.desktop" | sed -n 2p)"
check "unsupported action Exec remains unchanged" "Exec=sh -c 'steam \"\$@\"' sh %U" "$(grep '^Exec=' "$TMP/rw.desktop" | sed -n 3p)"

# Unterminated quoting anywhere in the entry makes the whole patch unsafe: no
# partial rewrite, tag, or backup may be published.
cat > "$TMP/malformed-mixed.desktop" <<EOF
[Desktop Entry]
Name=Steam
Exec=/usr/bin/steam %U
[Desktop Action Broken]
Exec=env MANGOHUD=1 "/usr/bin/steam %U
EOF
malformed_before="$(sha256sum "$TMP/malformed-mixed.desktop" | awk '{print $1}')"
if DC_BACKUP_ROOT="$TMP/central-malformed" dc_patch_one "$TMP/malformed-mixed.desktop"; then
  malformed_status=patched
else
  malformed_status=rejected
fi
check "patch_one rejects entry containing malformed Exec" "rejected" "$malformed_status"
check "malformed entry remains byte-identical" "$malformed_before" \
  "$(sha256sum "$TMP/malformed-mixed.desktop" | awk '{print $1}')"
check "malformed entry creates no backup" "no" \
  "$([ -e "$TMP/central-malformed/${TMP#/}/malformed-mixed.desktop" ] && echo yes || echo no)"

# Patch validation accepts a rewritten command whose exact wrapper executable is
# preceded by env and valid assignments.
printf '[Desktop Entry]\nName=Steam\nExec=env MANGOHUD=1 /usr/bin/steam %%U\n' > "$TMP/env-patch.desktop"
DC_BACKUP_ROOT="$TMP/central-env" dc_patch_one "$TMP/env-patch.desktop"
check "patch_one accepts env-prefixed rewritten Exec" "patched" "$(dc_classify "$TMP/env-patch.desktop")"
check "env-prefixed patch keeps prefix" "Exec=env MANGOHUD=1 $WRAPPER %U" "$(grep -m1 '^Exec=' "$TMP/env-patch.desktop")"
if dc_file_has_wrapper_exec "$TMP/env-patch.desktop"; then wrapper_valid=yes; else wrapper_valid=no; fi
check "env-prefixed patch validates exact wrapper token" "yes" "$wrapper_valid"

# Wrapper transport into awk must be byte-exact, and special path bytes must
# be Desktop Entry-escaped so validation resolves to the exact wrapper.
normal_wrapper="$WRAPPER"
WRAPPER='/tmp/sls\fake/path/steam'
backslash_rewrite="$(dc_rewrite_exec_line '/usr/bin/steam %U')"
check "rewrite uses four backslashes for Desktop Entry" '"/tmp/sls\\\\fake/path/steam" %U' "$backslash_rewrite"
printf '[Desktop Entry]\nExec=%s\n' "$backslash_rewrite" > "$TMP/backslash-wrapper.desktop"
if dc_file_has_wrapper_exec "$TMP/backslash-wrapper.desktop"; then wrapper_valid=yes; else wrapper_valid=no; fi
check "validation matches wrapper backslashes exactly" "yes" "$wrapper_valid"
WRAPPER='/tmp/sls#fake/path/steam'
reserved_rewrite="$(dc_rewrite_exec_line '/usr/bin/steam %U')"
check "rewrite quotes reserved wrapper characters" '"/tmp/sls#fake/path/steam" %U' "$reserved_rewrite"
printf '[Desktop Entry]\nExec=%s\n' "$reserved_rewrite" > "$TMP/reserved-wrapper.desktop"
if dc_file_has_wrapper_exec "$TMP/reserved-wrapper.desktop"; then wrapper_valid=yes; else wrapper_valid=no; fi
check "validation resolves reserved wrapper exactly" "yes" "$wrapper_valid"
WRAPPER="$normal_wrapper"

# patch_one: CENTRAL backup made, no adjacent backup left, tag added, mode 0644,
# Exec -> wrapper, shebang stripped.
printf '#!/usr/bin/env xdg-open\n[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$TMP/menu.desktop"
chmod 0711 "$TMP/menu.desktop"
DC_BACKUP_ROOT="$TMP/central" dc_patch_one "$TMP/menu.desktop"
check "patch_one classify after -> patched" "patched" "$(dc_classify "$TMP/menu.desktop")"
check "patch_one mode 0644" "644" "$(stat -c '%a' "$TMP/menu.desktop")"
menu_backup="$TMP/central/${TMP#/}/menu.desktop"
check "patch_one central backup exists" "yes" "$([ -f "$menu_backup" ] && echo yes || echo no)"
check "patch_one adjacent backup absent" "no" "$([ -f "$TMP/menu.desktop.slssteam-backup" ] && echo yes || echo no)"
check "patch_one central backup kept vanilla" "Exec=/usr/games/steam %U" "$(grep -m1 '^Exec=' "$menu_backup")"
check "patch_one first line clean" "[Desktop Entry]" "$(head -1 "$TMP/menu.desktop")"
check "patch_one Exec wrapped" "Exec=$WRAPPER %U" "$(grep -m1 '^Exec=' "$TMP/menu.desktop")"

# A later idempotent pass must preserve the first original backup byte-for-byte.
before_sum="$(sha256sum "$menu_backup" | awk '{print $1}')"
DC_BACKUP_ROOT="$TMP/central" dc_patch_one "$TMP/menu.desktop"
after_sum="$(sha256sum "$menu_backup" | awk '{print $1}')"
check "patch_one does not overwrite central original" "$before_sum" "$after_sum"

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
DC_HOME="$H" DC_BACKUP_ROOT="$H/.local/share/SLSsteam/backup" DC_SYS_APPS="$SYS" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
check "user run patches menu" "patched" "$(dc_classify "$H/.local/share/applications/steam.desktop")"
check "user run patches autostart" "patched" "$(dc_classify "$H/.config/autostart/steam.desktop")"
check "user run leaves stub alone" "stub" "$(dc_classify "$SYS/steam.desktop")"
check "user run leaves no adjacent autostart backup" "no" "$([ -e "$H/.config/autostart/steam.desktop.slssteam-backup" ] && echo yes || echo no)"

# MIGRATION: a legacy already-tagged entry left 0711 with a Valve shebang must be
# normalized to 0644 + clean first line on a re-run (the Cinnamon-bug fix path).
H5="$TMP/home5"; mkdir -p "$H5/.local/share/applications"
printf '#!/usr/bin/env xdg-open\n[Desktop Entry]\n%s\nName=Steam\nExec=%s %%U\n' "$DC_TAG" "$WRAPPER" \
  > "$H5/.local/share/applications/steam.desktop"
chmod 0711 "$H5/.local/share/applications/steam.desktop"
DC_HOME="$H5" DC_BACKUP_ROOT="$H5/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "migrate: still patched" "patched" "$(dc_classify "$H5/.local/share/applications/steam.desktop")"
check "migrate: 0711 -> 0644" "644" "$(stat -c '%a' "$H5/.local/share/applications/steam.desktop")"
check "migrate: shebang stripped" "[Desktop Entry]" "$(head -1 "$H5/.local/share/applications/steam.desktop")"

# MIGRATION 2: a legacy entry left mode 000/unreadable (root-owned 0711 in the
# field) must be made readable + patched, not skipped as "unrelated".
H6="$TMP/home6"; mkdir -p "$H6/.local/share/applications"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H6/.local/share/applications/steam.desktop"
chmod 000 "$H6/.local/share/applications/steam.desktop"
DC_HOME="$H6" DC_BACKUP_ROOT="$H6/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "migrate unreadable: patched" "patched" "$(dc_classify "$H6/.local/share/applications/steam.desktop")"
check "migrate unreadable: 0644" "644" "$(stat -c '%a' "$H6/.local/share/applications/steam.desktop")"

DC_HOME="$H" DC_BACKUP_ROOT="$H/.local/share/SLSsteam/backup" DC_SYS_APPS="$SYS" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --system
check "system run patches stub (steam installed)" "patched" "$(dc_classify "$SYS/steam.desktop")"
sys_backup="$H/.local/share/SLSsteam/backup/${SYS#/}/steam.desktop"
check "system run keeps central original" "Exec=/usr/games/steam %U" "$(grep -m1 '^Exec=' "$sys_backup")"
DC_HOME="$H" DC_BACKUP_ROOT="$H/.local/share/SLSsteam/backup" DC_SUDO="" dc_restore_one "$SYS/steam.desktop"
check "system restore recovers original" "Exec=/usr/games/steam %U" "$(grep -m1 '^Exec=' "$SYS/steam.desktop")"
check "system restore consumes central backup" "no" "$([ -e "$sys_backup" ] && echo yes || echo no)"

# CLI: --user runs without error against a fake HOME and patches the menu entry
H2="$TMP/home2"; mkdir -p "$H2/.local/share/applications"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H2/.local/share/applications/steam.desktop"
HOME="$H2" DC_HOME="$H2" DC_BACKUP_ROOT="$H2/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" WRAPPER="$WRAPPER" \
  bash "$HERE/ensure-desktop-coverage.sh" --user >/dev/null 2>&1
check "CLI --user patches menu entry" "patched" "$(dc_classify "$H2/.local/share/applications/steam.desktop")"

# restore: dc_run then dc_restore_all -> entries back to vanilla, backups consumed
H4="$TMP/home4"; mkdir -p "$H4/.local/share/applications" "$H4/.config/autostart" "$H4/Desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H4/.local/share/applications/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam -silent %%U\n' > "$H4/.config/autostart/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H4/Desktop/steam.desktop"
DC_HOME="$H4" DC_BACKUP_ROOT="$H4/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "before restore: menu patched" "patched" "$(dc_classify "$H4/.local/share/applications/steam.desktop")"
check "before restore: shortcut patched (regular file)" "patched" "$(dc_classify "$H4/Desktop/steam.desktop")"
menu4_backup="$H4/.local/share/SLSsteam/backup/${H4#/}/.local/share/applications/steam.desktop"
check "before restore: central menu backup exists" "yes" "$([ -f "$menu4_backup" ] && echo yes || echo no)"
DC_HOME="$H4" DC_BACKUP_ROOT="$H4/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_restore_all
check "restore: menu entry not patched" "0" "$(grep -c "$DC_TAG" "$H4/.local/share/applications/steam.desktop" 2>/dev/null | head -1)"
check "restore: central menu backup consumed" "no" "$([ -f "$menu4_backup" ] && echo yes || echo no)"
check "restore: autostart not patched" "0" "$(grep -c "$DC_TAG" "$H4/.config/autostart/steam.desktop" 2>/dev/null | head -1)"
check "restore: shortcut restored to vanilla regular file" "0" "$(grep -c "$DC_TAG" "$H4/Desktop/steam.desktop" 2>/dev/null | head -1)"

# LEGACY MIGRATION: adjacent backups are discovered even when the active
# autostart file was deleted (Steam autostart disabled), moved to the central
# mirrored path, and removed from every scanned XDG/shortcut directory.
H10="$TMP/home10"; mkdir -p "$H10/.local/share/applications" "$H10/.config/autostart" "$H10/Desktop"
SYS10="$TMP/sys10"; SYSAS10="$TMP/sysas10"; mkdir -p "$SYS10" "$SYSAS10"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H10/.local/share/applications/steam.desktop.slssteam-backup"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam -silent %%U\n' > "$H10/.config/autostart/steam.desktop.slssteam-backup"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H10/Desktop/steam.desktop.slsteam-bak"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$SYS10/steam.desktop.slssteam-backup"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam -silent %%U\n' > "$SYSAS10/steam.desktop.slssteam-backup"
DC_HOME="$H10" DC_BACKUP_ROOT="$H10/.local/share/SLSsteam/backup" DC_SYS_APPS="$SYS10" DC_SYS_AUTOSTART="$SYSAS10" DC_SUDO="" dc_run --system
legacy_as_central="$H10/.local/share/SLSsteam/backup/${H10#/}/.config/autostart/steam.desktop"
legacy_sys_central="$H10/.local/share/SLSsteam/backup/${SYS10#/}/steam.desktop"
check "legacy migration keeps deleted autostart original centrally" "yes" "$([ -f "$legacy_as_central" ] && echo yes || echo no)"
check "legacy migration mirrors system path" "yes" "$([ -f "$legacy_sys_central" ] && echo yes || echo no)"
check "legacy migration removes user applications backup" "no" "$([ -e "$H10/.local/share/applications/steam.desktop.slssteam-backup" ] && echo yes || echo no)"
check "legacy migration removes autostart backup" "no" "$([ -e "$H10/.config/autostart/steam.desktop.slssteam-backup" ] && echo yes || echo no)"
check "legacy migration removes old .slsteam-bak shortcut backup" "no" "$([ -e "$H10/Desktop/steam.desktop.slsteam-bak" ] && echo yes || echo no)"
check "legacy migration removes system applications backup" "no" "$([ -e "$SYS10/steam.desktop.slssteam-backup" ] && echo yes || echo no)"
check "legacy migration removes system autostart backup" "no" "$([ -e "$SYSAS10/steam.desktop.slssteam-backup" ] && echo yes || echo no)"

# SEED autostart override (SteamOS/Bazzite): a SYSTEM autostart exists but the
# user has no ~/.config/autostart/steam.desktop -> dc_run seeds a patched user
# override that shadows the read-only system entry.
H7="$TMP/home7"; mkdir -p "$H7/.local/share/applications" "$H7/.config/autostart"
SYSAS7="$TMP/sysas7"; mkdir -p "$SYSAS7"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam -silent %%U\n' > "$SYSAS7/steam.desktop"
DC_HOME="$H7" DC_BACKUP_ROOT="$H7/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$SYSAS7" DC_SUDO="" dc_run --user
check "seed: user autostart created" "yes" "$([ -f "$H7/.config/autostart/steam.desktop" ] && echo yes || echo no)"
check "seed: user autostart patched" "patched" "$(dc_classify "$H7/.config/autostart/steam.desktop")"
check "seed: wrapper Exec + silent arg kept" "Exec=$WRAPPER -silent %U" "$(grep -m1 '^Exec=' "$H7/.config/autostart/steam.desktop")"
seed7_backup="$H7/.local/share/SLSsteam/backup/${H7#/}/.config/autostart/steam.desktop"
check "seed: no central backup (seeded, not pre-existing)" "no" "$([ -f "$seed7_backup" ] && echo yes || echo no)"
# restore of a SEEDED override deletes it (user never had this file)
DC_HOME="$H7" DC_BACKUP_ROOT="$H7/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$SYSAS7" DC_SUDO="" dc_restore_all
check "seed restore: override removed" "no" "$([ -e "$H7/.config/autostart/steam.desktop" ] && echo yes || echo no)"

# NO-SEED on a normal desktop: no system autostart, no user autostart -> we must
# NOT create an autostart entry where the user had none.
H8="$TMP/home8"; mkdir -p "$H8/.local/share/applications" "$H8/.config/autostart"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam %%U\n' > "$H8/.local/share/applications/steam.desktop"
DC_HOME="$H8" DC_BACKUP_ROOT="$H8/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "no-seed: autostart NOT created on normal desktop" "no" "$([ -e "$H8/.config/autostart/steam.desktop" ] && echo yes || echo no)"

# SEED is a no-op when the user ALREADY has an autostart entry (the normal glob
# patches it in place; a real backup is kept so restore returns it to vanilla).
H9="$TMP/home9"; mkdir -p "$H9/.local/share/applications" "$H9/.config/autostart"
SYSAS9="$TMP/sysas9"; mkdir -p "$SYSAS9"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam -silent %%U\n' > "$SYSAS9/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/games/steam -silent %%U\n' > "$H9/.config/autostart/steam.desktop"
DC_HOME="$H9" DC_BACKUP_ROOT="$H9/.local/share/SLSsteam/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$SYSAS9" DC_SUDO="" dc_run --user
check "seed no-op: existing user autostart patched in place" "patched" "$(dc_classify "$H9/.config/autostart/steam.desktop")"
seed9_backup="$H9/.local/share/SLSsteam/backup/${H9#/}/.config/autostart/steam.desktop"
check "seed no-op: central backup kept for pre-existing entry" "yes" "$([ -f "$seed9_backup" ] && echo yes || echo no)"

# XDG directory model: custom data/config roots and desktop location.
H11="$TMP/home11"
mkdir -p "$H11/data/applications" "$TMP/data-a/applications" "$TMP/data-b/applications" \
         "$H11/conf/autostart" "$TMP/conf-a/autostart" "$H11/Desk"
check "xdg application dirs" \
  "$H11/data/applications|$TMP/data-a/applications|$TMP/data-b/applications" \
  "$(XDG_DATA_HOME="$H11/data" XDG_DATA_DIRS="$TMP/data-a:$TMP/data-b" DC_HOME="$H11" \
      dc_application_dirs | paste -sd'|' -)"
check "relative XDG data home falls back" "$H11/.local/share" \
  "$(XDG_DATA_HOME=relative/data DC_HOME="$H11" dc_data_home)"
check "relative XDG data dirs are skipped" \
  "$H11/data/applications|$TMP/data-a/applications" \
  "$(XDG_DATA_HOME="$H11/data" XDG_DATA_DIRS="relative:$TMP/data-a" DC_HOME="$H11" \
      dc_application_dirs | paste -sd'|' -)"
check "xdg colon dirs preserve glob characters" "$TMP/data-*/applications" \
  "$(dc_colon_dirs "$TMP/data-*" '/unused' applications)"
check "xdg autostart dirs" \
  "$H11/conf/autostart|$TMP/conf-a/autostart" \
  "$(XDG_CONFIG_HOME="$H11/conf" XDG_CONFIG_DIRS="$TMP/conf-a" DC_HOME="$H11" \
      dc_autostart_dirs | paste -sd'|' -)"
check "relative XDG config home falls back" "$H11/.config" \
  "$(XDG_CONFIG_HOME=relative/conf DC_HOME="$H11" dc_config_home)"
check "relative XDG config dirs are skipped" \
  "$H11/conf/autostart|$TMP/conf-a/autostart" \
  "$(XDG_CONFIG_HOME="$H11/conf" XDG_CONFIG_DIRS="relative:$TMP/conf-a" DC_HOME="$H11" \
      dc_autostart_dirs | paste -sd'|' -)"
printf 'XDG_DESKTOP_DIR="$HOME/Desk"\n' > "$H11/conf/user-dirs.dirs"
check "custom XDG desktop dir" "$H11/Desk" \
  "$(XDG_CONFIG_HOME="$H11/conf" DC_HOME="$H11" dc_desktop_dir)"
printf 'XDG_DESKTOP_DIR="${HOME}/Desk"\ntouch "%s"\n' "$TMP/user-dirs-executed" \
  > "$H11/conf/user-dirs.dirs"
check "braced HOME XDG desktop dir" "$H11/Desk" \
  "$(XDG_CONFIG_HOME="$H11/conf" DC_HOME="$H11" dc_desktop_dir)"
check "user-dirs.dirs is never executed" "no" \
  "$([ -e "$TMP/user-dirs-executed" ] && echo yes || echo no)"

# Custom XDG user roots must drive patch discovery, legacy migration and restore.
H12="$TMP/home12"
mkdir -p "$H12/data/applications" "$H12/conf/autostart" "$H12/Desk"
printf 'XDG_DESKTOP_DIR="$HOME/Desk"\n' > "$H12/conf/user-dirs.dirs"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H12/data/applications/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam -silent %%U\n' > "$H12/conf/autostart/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H12/Desk/steam.desktop"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H12/conf/autostart/old-steam.desktop.slssteam-backup"
XDG_DATA_HOME="$H12/data" XDG_CONFIG_HOME="$H12/conf" DC_HOME="$H12" \
  DC_BACKUP_ROOT="$H12/backup" DC_SYS_APPS="$TMP/none" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_run --user
check "custom roots: application patched" "patched" \
  "$(dc_classify "$H12/data/applications/steam.desktop")"
check "custom roots: autostart patched" "patched" \
  "$(dc_classify "$H12/conf/autostart/steam.desktop")"
check "custom roots: desktop shortcut patched" "patched" \
  "$(dc_classify "$H12/Desk/steam.desktop")"
check "custom roots: legacy autostart backup migrated" "no" \
  "$([ -e "$H12/conf/autostart/old-steam.desktop.slssteam-backup" ] && echo yes || echo no)"
XDG_DATA_HOME="$H12/data" XDG_CONFIG_HOME="$H12/conf" DC_HOME="$H12" \
  DC_BACKUP_ROOT="$H12/backup" DC_SYS_APPS="$TMP/none" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_restore_all
check "custom roots restore: application vanilla" "0" \
  "$(grep -c "$DC_TAG" "$H12/data/applications/steam.desktop" 2>/dev/null | head -1)"
check "custom roots restore: autostart vanilla" "0" \
  "$(grep -c "$DC_TAG" "$H12/conf/autostart/steam.desktop" 2>/dev/null | head -1)"
check "custom roots restore: shortcut vanilla" "0" \
  "$(grep -c "$DC_TAG" "$H12/Desk/steam.desktop" 2>/dev/null | head -1)"

# Same-ID application shadows: system IDs must be represented under the user
# XDG applications directory without replacing donor metadata or user originals.
H13="$TMP/home13"; U13="$H13/data/applications"
S13A="$TMP/sys13-a/applications"; S13B="$TMP/sys13-b/applications"
mkdir -p "$U13" "$S13A" "$S13B"
cat > "$S13A/steam.desktop" <<'EOF'
#!/usr/bin/env xdg-open
[Desktop Entry]
Type=Application
Name=Steam
Name[pt_BR]=Steam Localizado
Icon=steam-original
MimeType=x-scheme-handler/steam;application/x-steam;
Actions=Store;Unsafe;
Exec=/usr/bin/steam %U
[Desktop Action Store]
Name=Store
Exec=steam-jupiter steam://store
[Desktop Action Unsafe]
Name=Unsafe
Exec=sh -c 'steam "$@"' sh %U
EOF
cat > "$S13B/com.valvesoftware.Steam.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Valve Steam
Icon=valve-steam
MimeType=x-scheme-handler/steam;
Actions=Library;
Exec=/usr/bin/bazzite-steam -silent %U
[Desktop Action Library]
Name=Library
Exec=env MANGOHUD=1 /usr/bin/steam steam://open/games
EOF
cat > "$U13/com.valvesoftware.Steam.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=My Steam
Name[pt_BR]=Meu Steam
Icon=my-steam
MimeType=x-scheme-handler/steam;
Actions=Friends;
Exec=/usr/games/steam %U
[Desktop Action Friends]
Name=Friends
Exec=steam steam://open/friends
EOF
cp -- "$U13/com.valvesoftware.Steam.desktop" "$TMP/preexisting-com.desktop"
steam_donor_before="$(sha256sum "$S13A/steam.desktop" | awk '{print $1}')"
com_donor_before="$(sha256sum "$S13B/com.valvesoftware.Steam.desktop" | awk '{print $1}')"
XDG_DATA_HOME="$H13/data" XDG_DATA_DIRS="${S13A%/applications}:${S13B%/applications}" \
  DC_HOME="$H13" DC_BACKUP_ROOT="$H13/backup" DC_SYS_APPS="$TMP/none" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
steam_shadow="$U13/steam.desktop"
com_shadow="$U13/com.valvesoftware.Steam.desktop"
check "shadows: steam.desktop same-ID created" "yes" \
  "$([ -f "$steam_shadow" ] && echo yes || echo no)"
check "shadows: com.valvesoftware.Steam.desktop retained" "yes" \
  "$([ -f "$com_shadow" ] && echo yes || echo no)"
check "shadows: seeded ownership tag" "1" \
  "$(grep -cFx "$DC_SEED_TAG" "$steam_shadow" 2>/dev/null | head -1)"
check "shadows: seeded application mode 0644" "644" \
  "$(stat -c '%a' "$steam_shadow" 2>/dev/null || true)"
check "shadows: localized Name preserved" "Name[pt_BR]=Steam Localizado" \
  "$(grep -m1 -F 'Name[pt_BR]=Steam Localizado' "$steam_shadow" 2>/dev/null || true)"
check "shadows: Icon preserved" "Icon=steam-original" \
  "$(grep -m1 '^Icon=' "$steam_shadow" 2>/dev/null || true)"
check "shadows: MimeType preserved" "MimeType=x-scheme-handler/steam;application/x-steam;" \
  "$(grep -m1 '^MimeType=' "$steam_shadow" 2>/dev/null || true)"
check "shadows: Desktop Actions list preserved" "Actions=Store;Unsafe;" \
  "$(grep -m1 '^Actions=' "$steam_shadow" 2>/dev/null || true)"
check "shadows: primary Exec wrapped" "Exec=$WRAPPER %U" \
  "$(grep '^Exec=' "$steam_shadow" 2>/dev/null | sed -n '1p')"
check "shadows: applicable Action Exec wrapped" "Exec=$WRAPPER steam://store" \
  "$(grep '^Exec=' "$steam_shadow" 2>/dev/null | sed -n '2p')"
check "shadows: unsupported Action preserved" "Exec=sh -c 'steam \"\$@\"' sh %U" \
  "$(grep '^Exec=' "$steam_shadow" 2>/dev/null | sed -n '3p')"
seed13_backup="$H13/backup/${steam_shadow#/}"
check "shadows: seeded content has no central backup" "no" \
  "$([ -e "$seed13_backup" ] && echo yes || echo no)"
com13_backup="$H13/backup/${com_shadow#/}"
check "shadows: pre-existing user entry centrally backed up" "yes" \
  "$([ -f "$com13_backup" ] && echo yes || echo no)"
check "shadows: backup is exact first user original" "yes" \
  "$(cmp -s "$TMP/preexisting-com.desktop" "$com13_backup" 2>/dev/null && echo yes || echo no)"
check "shadows: pre-existing metadata preserved" "Name[pt_BR]=Meu Steam" \
  "$(grep -m1 -F 'Name[pt_BR]=Meu Steam' "$com_shadow" 2>/dev/null || true)"
check "shadows: pre-existing primary Exec wrapped" "Exec=$WRAPPER %U" \
  "$(grep '^Exec=' "$com_shadow" 2>/dev/null | sed -n '1p')"
check "shadows: pre-existing Action Exec wrapped" "Exec=$WRAPPER steam://open/friends" \
  "$(grep '^Exec=' "$com_shadow" 2>/dev/null | sed -n '2p')"
check "shadows: steam system donor remains byte-identical" "$steam_donor_before" \
  "$(sha256sum "$S13A/steam.desktop" | awk '{print $1}')"
check "shadows: com system donor remains byte-identical" "$com_donor_before" \
  "$(sha256sum "$S13B/com.valvesoftware.Steam.desktop" | awk '{print $1}')"
steam_shadow_first="$(sha256sum "$steam_shadow" 2>/dev/null | awk '{print $1}')"
com_shadow_first="$(sha256sum "$com_shadow" 2>/dev/null | awk '{print $1}')"
com_backup_first="$(sha256sum "$com13_backup" 2>/dev/null | awk '{print $1}')"
XDG_DATA_HOME="$H13/data" XDG_DATA_DIRS="${S13A%/applications}:${S13B%/applications}" \
  DC_HOME="$H13" DC_BACKUP_ROOT="$H13/backup" DC_SYS_APPS="$TMP/none" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
check "shadows: second run leaves seeded bytes unchanged" "$steam_shadow_first" \
  "$(sha256sum "$steam_shadow" 2>/dev/null | awk '{print $1}')"
check "shadows: second run leaves pre-existing bytes unchanged" "$com_shadow_first" \
  "$(sha256sum "$com_shadow" 2>/dev/null | awk '{print $1}')"
check "shadows: second run leaves first-original backup unchanged" "$com_backup_first" \
  "$(sha256sum "$com13_backup" 2>/dev/null | awk '{print $1}')"
XDG_DATA_HOME="$H13/data" XDG_DATA_DIRS="${S13A%/applications}:${S13B%/applications}" \
  DC_HOME="$H13" DC_BACKUP_ROOT="$H13/backup" DC_SYS_APPS="$TMP/none" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" dc_restore_all
check "shadows restore: seeded same-ID entry deleted" "no" \
  "$([ -e "$steam_shadow" ] && echo yes || echo no)"
check "shadows restore: pre-existing user entry restored exactly" "yes" \
  "$(cmp -s "$TMP/preexisting-com.desktop" "$com_shadow" 2>/dev/null && echo yes || echo no)"

# Discovery is content-safe and case-insensitive: a supported mixed-case Steam
# filename qualifies, while Name text alone never creates an unrelated shadow.
H14="$TMP/home14"; U14="$H14/data/applications"; S14="$TMP/sys14/applications"
mkdir -p "$U14" "$S14"
printf '[Desktop Entry]\nType=Application\nName=Mixed Steam\nExec=/usr/bin/steam %%U\n' \
  > "$S14/StEaM-Client.desktop"
printf '[Desktop Entry]\nType=Application\nName=Steam Notes\nExec=/usr/bin/notes %%U\n' \
  > "$S14/Steam-Notes.desktop"
XDG_DATA_HOME="$H14/data" XDG_DATA_DIRS="${S14%/applications}" DC_HOME="$H14" \
  DC_BACKUP_ROOT="$H14/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
check "shadows discovery: mixed-case filename seeded by same ID" "patched" \
  "$(dc_classify "$U14/StEaM-Client.desktop")"
check "shadows discovery: unrelated Name=Steam entry rejected" "no" \
  "$([ -e "$U14/Steam-Notes.desktop" ] && echo yes || echo no)"

# An installer stub is never copied when a real donor exists. If it is the only
# donor and Steam is installed, create a minimal complete same-ID launcher.
H15="$TMP/home15"; U15="$H15/data/applications"
S15A="$TMP/sys15-a/applications"; S15B="$TMP/sys15-b/applications"
mkdir -p "$U15" "$S15A" "$S15B"
printf '[Desktop Entry]\nType=Application\nName=Install Steam\nExec=/usr/bin/steam %%U\n' \
  > "$S15A/steam.desktop"
printf '[Desktop Entry]\nType=Application\nName=Steam\nExec=/usr/bin/steam %%U\n' \
  > "$S15B/com.valvesoftware.Steam.desktop"
XDG_DATA_HOME="$H15/data" XDG_DATA_DIRS="${S15A%/applications}:${S15B%/applications}" \
  DC_HOME="$H15" DC_BACKUP_ROOT="$H15/backup" DC_SYS_APPS="$TMP/none" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
check "shadows stub: real donor prevents Install Steam shadow" "no" \
  "$([ -e "$U15/steam.desktop" ] && echo yes || echo no)"
check "shadows stub: real donor still gets same-ID shadow" "patched" \
  "$(dc_classify "$U15/com.valvesoftware.Steam.desktop")"
H16="$TMP/home16"; U16="$H16/data/applications"; S16="$TMP/sys16/applications"
mkdir -p "$U16" "$S16"
cat > "$S16/steam.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Install Steam
Comment=Package installer stub
Exec=sh -c 'STEAM_FRAME_FORCE_CLOSE=1 steam %U'
EOF
XDG_DATA_HOME="$H16/data" XDG_DATA_DIRS="${S16%/applications}" DC_HOME="$H16" \
  DC_BACKUP_ROOT="$H16/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --user
check "shadows Debian stub-only: minimal same-ID entry created" "yes" \
  "$([ -f "$U16/steam.desktop" ] && echo yes || echo no)"
check "shadows Debian stub-only: installer identity not copied" "Name=Steam" \
  "$(grep -m1 '^Name=' "$U16/steam.desktop" 2>/dev/null || true)"
check "shadows Debian stub-only: minimal entry has Type" "Type=Application" \
  "$(grep -m1 '^Type=' "$U16/steam.desktop" 2>/dev/null || true)"
check "shadows Debian stub-only: minimal entry routes through wrapper" "Exec=$WRAPPER %U" \
  "$(grep -m1 '^Exec=' "$U16/steam.desktop" 2>/dev/null || true)"
check "shadows Debian stub-only: minimal entry is seeded" "1" \
  "$(grep -cFx "$DC_SEED_TAG" "$U16/steam.desktop" 2>/dev/null | head -1)"
DC_HOME="$H16" DC_BACKUP_ROOT="$H16/backup" DC_SYS_APPS="$S16" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="" DC_STEAM_INSTALLED=1 dc_run --system
check "system run patches installed Debian shell stub" \
  "Exec=env STEAM_FRAME_FORCE_CLOSE=1 $WRAPPER %U" \
  "$(grep -m1 '^Exec=' "$S16/steam.desktop" 2>/dev/null || true)"

# A force-close shell wrapper on a Steam-named entry (a widely shared tweak for
# Steam not exiting) leaves the primary launcher outside the wrapper. Desktop
# Actions alone must never satisfy the patch, and an already-tagged entry must
# still be repaired instead of being treated as done.
FC1="$TMP/home-fc1"; AFC1="$FC1/data/applications"
mkdir -p "$AFC1"
cat > "$AFC1/steam.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Steam
Exec=sh -c 'STEAM_FRAME_FORCE_CLOSE=1 steam %U'
[Desktop Action Store]
Name=Store
Exec=steam steam://store
EOF
DC_HOME="$FC1" DC_BACKUP_ROOT="$FC1/backup" DC_SUDO="" dc_patch_one "$AFC1/steam.desktop"
check "force-close wrapper: untagged primary Exec repaired" \
  "Exec=env STEAM_FRAME_FORCE_CLOSE=1 $WRAPPER %U" \
  "$(grep -m1 '^Exec=' "$AFC1/steam.desktop" 2>/dev/null || true)"

FC2="$TMP/home-fc2"; AFC2="$FC2/data/applications"
mkdir -p "$AFC2"
cat > "$AFC2/steam.desktop" <<EOF
[Desktop Entry]
$DC_TAG
Type=Application
Name=Steam
Exec=sh -c 'STEAM_FRAME_FORCE_CLOSE=1 steam %U'
[Desktop Action Store]
Name=Store
Exec=$WRAPPER steam://store
EOF
DC_HOME="$FC2" DC_BACKUP_ROOT="$FC2/backup" DC_SUDO="" dc_patch_one "$AFC2/steam.desktop"
check "force-close wrapper: tagged entry with wrapper actions still repaired" \
  "Exec=env STEAM_FRAME_FORCE_CLOSE=1 $WRAPPER %U" \
  "$(grep -m1 '^Exec=' "$AFC2/steam.desktop" 2>/dev/null || true)"

# An unsupported primary launcher we cannot rewrite must never be tagged as
# patched: the tag is what makes a later run believe the entry is covered.
FC3="$TMP/home-fc3"; AFC3="$FC3/data/applications"
mkdir -p "$AFC3"
cat > "$AFC3/steam.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Steam
Exec=/opt/custom/launch-steam.sh %U
[Desktop Action Store]
Name=Store
Exec=steam steam://store
EOF
DC_HOME="$FC3" DC_BACKUP_ROOT="$FC3/backup" DC_SUDO="" dc_patch_one "$AFC3/steam.desktop"
check "unrewritable primary Exec is not tagged as patched" "0" \
  "$(grep -cF "$DC_TAG" "$AFC3/steam.desktop" 2>/dev/null | head -1)"

# A shell command that does more than launch Steam must stay untouched: the
# repair reproduces the launch, so anything else would be silently discarded.
FC4="$TMP/home-fc4"; AFC4="$FC4/data/applications"
mkdir -p "$AFC4"
cat > "$AFC4/steam.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Steam
Exec=sh -c 'mangohud steam %U && notify-send done'
[Desktop Action Store]
Name=Store
Exec=steam steam://store
EOF
DC_HOME="$FC4" DC_BACKUP_ROOT="$FC4/backup" DC_SUDO="" dc_patch_one "$AFC4/steam.desktop"
check "compound shell command is left untouched" \
  "Exec=sh -c 'mangohud steam %U && notify-send done'" \
  "$(grep -m1 '^Exec=' "$AFC4/steam.desktop" 2>/dev/null || true)"

# Guardian command shims make serialization and cache ordering observable.
GUARDIAN_EVENTS="$TMP/guardian-events"
FLOCK_SHIM="$TMP/flock-shim"
UPDATE_SHIM="$TMP/update-desktop-database-shim"
KBUILD_SHIM="$TMP/kbuildsycoca6-shim"
SUDO_SHIM="$TMP/sudo-shim"
cat > "$FLOCK_SHIM" <<'EOF'
#!/bin/sh
printf 'flock:%s\n' "$*" >> "$DC_TEST_EVENTS"
if [ "${DC_TEST_FLOCK_FAIL:-0}" = 1 ] && [ "${1:-}" = -n ]; then
  exit 1
fi
if [ "${1:-}" = -u ]; then
  [ -f "$DC_TEST_STATE_LOG" ] && printf '%s\n' 'release:state-present' >> "$DC_TEST_EVENTS"
fi
EOF
cat > "$UPDATE_SHIM" <<'EOF'
#!/bin/sh
if grep -qF "Exec=$WRAPPER" "$DC_TEST_APP" 2>/dev/null; then state=wrapped; else state=vanilla; fi
printf 'update:%s:%s\n' "$state" "$1" >> "$DC_TEST_EVENTS"
EOF
cat > "$KBUILD_SHIM" <<'EOF'
#!/bin/sh
printf 'kbuild:%s\n' "$*" >> "$DC_TEST_EVENTS"
EOF
cat > "$SUDO_SHIM" <<'EOF'
#!/bin/sh
printf 'sudo:%s\n' "$*" >> "$DC_TEST_EVENTS"
exit 99
EOF
chmod +x "$FLOCK_SHIM" "$UPDATE_SHIM" "$KBUILD_SHIM" "$SUDO_SHIM"

# A converged application is examined but does not trigger either cache tool.
H17="$TMP/home17"; mkdir -p "$H17/data/applications" "$H17/conf/autostart" \
  "$TMP/empty17/applications" "$H17/runtime" "$H17/state"
printf '[Desktop Entry]\n%s\nName=Steam\nExec=%s %%U\n' "$DC_TAG" "$WRAPPER" \
  > "$H17/data/applications/steam.desktop"
: > "$GUARDIAN_EVENTS"
state17="$H17/state/slsteam-moon/guardian.log"
if XDG_DATA_HOME="$H17/data" XDG_DATA_DIRS="$TMP/empty17" XDG_CONFIG_HOME="$H17/conf" \
  XDG_STATE_HOME="$H17/state" XDG_RUNTIME_DIR="$H17/runtime" DC_HOME="$H17" \
  DC_BACKUP_ROOT="$H17/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="$SUDO_SHIM" DC_FLOCK="$FLOCK_SHIM" \
  DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" DC_KBUILDSYCOCA="$KBUILD_SHIM" \
  DC_TEST_EVENTS="$GUARDIAN_EVENTS" DC_TEST_STATE_LOG="$state17" DC_TEST_APP="$H17/data/applications/steam.desktop" \
  dc_guardian_run; then guardian17_status=0; else guardian17_status=$?; fi
check "guardian no-change exits successfully" "0" "$guardian17_status"
check "guardian no-change runs no cache command" "0" \
  "$(grep -Ec '^(update|kbuild):' "$GUARDIAN_EVENTS" 2>/dev/null || true)"
check "guardian writes state under overridden XDG_STATE_HOME" "yes" \
  "$([ -f "$state17" ] && echo yes || echo no)"
check "guardian summary has exactly the six counters" "yes" \
  "$(grep -Eq '^[^ ]+ examined=[0-9]+ changed=[0-9]+ app_changed=[0-9]+ autostart_changed=[0-9]+ skipped=[0-9]+ failed=[0-9]+$' "$state17" 2>/dev/null && echo yes || echo no)"
check "guardian releases lock after writing summary" "yes" \
  "$(grep -qFx 'release:state-present' "$GUARDIAN_EVENTS" 2>/dev/null && echo yes || echo no)"
current_user="${USER:-$(id -un)}"
check "guardian state message contains no username" "no" \
  "$(grep -Fq "$current_user" "$state17" 2>/dev/null && echo yes || echo no)"

# Publishing an application shadow precedes cache convergence. Runtime guardian
# must never invoke the privileged system fallback or mutate its donor.
H18="$TMP/home18"; S18="$TMP/sys18/applications"
mkdir -p "$H18/data/applications" "$H18/conf/autostart" "$H18/runtime" "$H18/state" "$S18"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$S18/steam.desktop"
sys18_before="$(sha256sum "$S18/steam.desktop" | awk '{print $1}')"
: > "$GUARDIAN_EVENTS"
state18="$H18/state/slsteam-moon/guardian.log"; app18="$H18/data/applications/steam.desktop"
if XDG_DATA_HOME="$H18/data" XDG_DATA_DIRS="${S18%/applications}" XDG_CONFIG_HOME="$H18/conf" \
  XDG_STATE_HOME="$H18/state" XDG_RUNTIME_DIR="$H18/runtime" DC_HOME="$H18" \
  DC_BACKUP_ROOT="$H18/backup" DC_SYS_APPS="$S18" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="$SUDO_SHIM" DC_FLOCK="$FLOCK_SHIM" DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" \
  DC_KBUILDSYCOCA="$KBUILD_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" \
  DC_TEST_STATE_LOG="$state18" DC_TEST_APP="$app18" WRAPPER="$WRAPPER" dc_guardian_run; then
  guardian18_status=0
else
  guardian18_status=$?
fi
check "guardian application change exits successfully" "0" "$guardian18_status"
check "guardian application shadow is published before ordered caches" \
  "update:wrapped:$H18/data/applications|kbuild:--noincremental" \
  "$(grep -E '^(update|kbuild):' "$GUARDIAN_EVENTS" 2>/dev/null | paste -sd'|' -)"
check "guardian application change increments app counter" "yes" \
  "$(grep -Eq ' app_changed=[1-9][0-9]* ' "$state18" 2>/dev/null && echo yes || echo no)"
check "guardian runtime never invokes sudo" "0" \
  "$(grep -c '^sudo:' "$GUARDIAN_EVENTS" 2>/dev/null || true)"
check "guardian runtime leaves system donor unchanged" "$sys18_before" \
  "$(sha256sum "$S18/steam.desktop" | awk '{print $1}')"

# An autostart-only repair is tracked separately and never rebuilds app caches.
H19="$TMP/home19"; mkdir -p "$H19/data/applications" "$H19/conf/autostart" \
  "$TMP/empty19/applications" "$H19/runtime" "$H19/state"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam -silent %%U\n' > "$H19/conf/autostart/steam.desktop"
: > "$GUARDIAN_EVENTS"
state19="$H19/state/slsteam-moon/guardian.log"
XDG_DATA_HOME="$H19/data" XDG_DATA_DIRS="$TMP/empty19" XDG_CONFIG_HOME="$H19/conf" \
  XDG_STATE_HOME="$H19/state" XDG_RUNTIME_DIR="$H19/runtime" DC_HOME="$H19" \
  DC_BACKUP_ROOT="$H19/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" \
  DC_FLOCK="$FLOCK_SHIM" DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" DC_KBUILDSYCOCA="$KBUILD_SHIM" \
  DC_TEST_EVENTS="$GUARDIAN_EVENTS" DC_TEST_STATE_LOG="$state19" DC_TEST_APP="$TMP/missing19" \
  dc_guardian_run >/dev/null 2>&1
check "guardian autostart-only repair is applied" "patched" \
  "$(dc_classify "$H19/conf/autostart/steam.desktop")"
check "guardian autostart-only change increments counter" "yes" \
  "$(grep -Eq ' autostart_changed=[1-9][0-9]* ' "$state19" 2>/dev/null && echo yes || echo no)"
check "guardian autostart-only change runs no app cache" "0" \
  "$(grep -Ec '^(update|kbuild):' "$GUARDIAN_EVENTS" 2>/dev/null || true)"

# Lock contention is a successful no-op: no file, backup, state, or cache mutation.
H20="$TMP/home20"; mkdir -p "$H20/data/applications" "$H20/conf/autostart" \
  "$TMP/empty20/applications" "$H20/runtime" "$H20/state"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H20/data/applications/steam.desktop"
app20_before="$(sha256sum "$H20/data/applications/steam.desktop" | awk '{print $1}')"
: > "$GUARDIAN_EVENTS"
state20="$H20/state/slsteam-moon/guardian.log"
if XDG_DATA_HOME="$H20/data" XDG_DATA_DIRS="$TMP/empty20" XDG_CONFIG_HOME="$H20/conf" \
  XDG_STATE_HOME="$H20/state" XDG_RUNTIME_DIR="$H20/runtime" DC_HOME="$H20" \
  DC_BACKUP_ROOT="$H20/backup" DC_FLOCK="$FLOCK_SHIM" DC_TEST_FLOCK_FAIL=1 \
  DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" DC_KBUILDSYCOCA="$KBUILD_SHIM" \
  DC_TEST_EVENTS="$GUARDIAN_EVENTS" DC_TEST_STATE_LOG="$state20" DC_TEST_APP="$H20/data/applications/steam.desktop" \
  dc_guardian_run; then guardian20_status=0; else guardian20_status=$?; fi
check "guardian lock contention exits successfully" "0" "$guardian20_status"
check "guardian lock contention leaves application unchanged" "$app20_before" \
  "$(sha256sum "$H20/data/applications/steam.desktop" | awk '{print $1}')"
check "guardian lock contention creates no backup" "no" \
  "$([ -d "$H20/backup" ] && echo yes || echo no)"
check "guardian lock contention runs no cache" "0" \
  "$(grep -Ec '^(update|kbuild):' "$GUARDIAN_EVENTS" 2>/dev/null || true)"

# A mandatory malformed user entry is counted as failed and left retryable. Once
# corrected, the next guardian run repairs it rather than treating it as done.
H21="$TMP/home21"; mkdir -p "$H21/data/applications" "$H21/conf/autostart" \
  "$TMP/empty21/applications" "$H21/runtime" "$H21/state"
printf '[Desktop Entry]\nName=Steam\nExec="/usr/bin/steam %%U\n' > "$H21/data/applications/steam.desktop"
: > "$GUARDIAN_EVENTS"
state21="$H21/state/slsteam-moon/guardian.log"
if XDG_DATA_HOME="$H21/data" XDG_DATA_DIRS="$TMP/empty21" XDG_CONFIG_HOME="$H21/conf" \
  XDG_STATE_HOME="$H21/state" XDG_RUNTIME_DIR="$H21/runtime" DC_HOME="$H21" \
  DC_BACKUP_ROOT="$H21/backup" DC_FLOCK="$FLOCK_SHIM" DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" \
  DC_KBUILDSYCOCA="$KBUILD_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" \
  DC_TEST_STATE_LOG="$state21" DC_TEST_APP="$H21/data/applications/steam.desktop" dc_guardian_run; then
  guardian21_first=0
else
  guardian21_first=$?
fi
check "guardian failed mandatory rewrite returns nonzero" "2" "$guardian21_first"
check "guardian failed mandatory rewrite increments failed" "yes" \
  "$(grep -Eq ' failed=[1-9][0-9]*$' "$state21" 2>/dev/null && echo yes || echo no)"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H21/data/applications/steam.desktop"
if XDG_DATA_HOME="$H21/data" XDG_DATA_DIRS="$TMP/empty21" XDG_CONFIG_HOME="$H21/conf" \
  XDG_STATE_HOME="$H21/state" XDG_RUNTIME_DIR="$H21/runtime" DC_HOME="$H21" \
  DC_BACKUP_ROOT="$H21/backup" DC_FLOCK="$FLOCK_SHIM" DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" \
  DC_KBUILDSYCOCA="$KBUILD_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" \
  DC_TEST_STATE_LOG="$state21" DC_TEST_APP="$H21/data/applications/steam.desktop" dc_guardian_run; then
  guardian21_second=0
else
  guardian21_second=$?
fi
check "guardian retries corrected rewrite next run" "0" "$guardian21_second"
check "guardian retry patches mandatory entry" "patched" \
  "$(dc_classify "$H21/data/applications/steam.desktop")"
check "guardian retry summary clears failures" "yes" \
  "$(tail -n 1 "$state21" 2>/dev/null | grep -Eq ' failed=0$' && echo yes || echo no)"

# A failure to persist the summary log is diagnostic only: it must NOT be
# reported as a reconciliation failure. Point XDG_STATE_HOME at a regular file so
# the summary mkdir/write fails while the mandatory entry still reconciles.
H23="$TMP/home23"; mkdir -p "$H23/data/applications" "$H23/conf/autostart" \
  "$TMP/empty23/applications" "$H23/runtime"
: > "$H23/state_is_a_file"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$H23/data/applications/steam.desktop"
if XDG_DATA_HOME="$H23/data" XDG_DATA_DIRS="$TMP/empty23" XDG_CONFIG_HOME="$H23/conf" \
  XDG_STATE_HOME="$H23/state_is_a_file" XDG_RUNTIME_DIR="$H23/runtime" DC_HOME="$H23" \
  DC_BACKUP_ROOT="$H23/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" \
  DC_FLOCK="$FLOCK_SHIM" DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" \
  DC_KBUILDSYCOCA="$KBUILD_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" \
  DC_TEST_APP="$H23/data/applications/steam.desktop" dc_guardian_run; then
  guardian23_status=0
else
  guardian23_status=$?
fi
check "guardian summary-write failure is not a reconciliation failure" "0" "$guardian23_status"
check "guardian still patched the mandatory entry despite summary failure" "patched" \
  "$(dc_classify "$H23/data/applications/steam.desktop")"

# The CLI accepts exactly one documented mode. --user remains best-effort while
# --guardian reports mandatory user failures.
cli_status() {
  local home="$1"; shift
  HOME="$home" DC_HOME="$home" XDG_DATA_HOME="$home/data" XDG_DATA_DIRS="$TMP/cli-empty" \
    XDG_CONFIG_HOME="$home/conf" XDG_STATE_HOME="$home/state" XDG_RUNTIME_DIR="$home/runtime" \
    DC_BACKUP_ROOT="$home/backup" DC_SYS_APPS="$TMP/none" DC_SYS_AUTOSTART="$TMP/none" \
    DC_SUDO="" DC_FLOCK="$FLOCK_SHIM" DC_UPDATE_DESKTOP_DATABASE="$UPDATE_SHIM" \
    DC_KBUILDSYCOCA="$KBUILD_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" \
    DC_TEST_STATE_LOG="$home/state/slsteam-moon/guardian.log" DC_TEST_APP="$home/data/applications/steam.desktop" \
    WRAPPER="$WRAPPER" bash "$HERE/ensure-desktop-coverage.sh" "$@" >/dev/null 2>&1
  printf '%s\n' "$?"
}
H22="$TMP/home22"; mkdir -p "$H22/data/applications" "$H22/conf/autostart" \
  "$H22/state" "$H22/runtime" "$TMP/cli-empty/applications"
printf '[Desktop Entry]\nName=Steam\nExec="/usr/bin/steam %%U\n' > "$H22/data/applications/steam.desktop"
: > "$GUARDIAN_EVENTS"
check "CLI --user is accepted and best-effort" "0" "$(cli_status "$H22" --user)"
check "CLI --system is accepted" "0" "$(cli_status "$H22" --system)"
check "CLI --guardian reports mandatory failure" "2" "$(cli_status "$H22" --guardian)"
check "CLI rejects missing mode" "2" "$(cli_status "$H22")"
check "CLI rejects unknown mode" "2" "$(cli_status "$H22" --unknown)"
check "CLI rejects multiple modes" "2" "$(cli_status "$H22" --user --guardian)"

# Ordered uninstall: if a mutable system original cannot be restored, retain
# the working user shadow and its first-original state for a later retry.
H23="$TMP/home23"; U23="$H23/data/applications"; S23="$TMP/sys23/applications"
mkdir -p "$U23" "$H23/conf/autostart" "$S23"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$S23/steam.desktop"
DC_HOME="$H23" DC_BACKUP_ROOT="$H23/backup" DC_SUDO="" dc_patch_one "$S23/steam.desktop"
printf '[Desktop Entry]\n%s\n%s\nName=Steam\nExec=%s %%U\n' \
  "$DC_SEED_TAG" "$DC_TAG" "$WRAPPER" > "$U23/steam.desktop"
sys23_backup="$H23/backup/${S23#/}/steam.desktop"
: > "$GUARDIAN_EVENTS"
if XDG_DATA_HOME="$H23/data" XDG_CONFIG_HOME="$H23/conf" DC_HOME="$H23" \
  DC_BACKUP_ROOT="$H23/backup" DC_SYS_APPS="$S23" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="$SUDO_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" dc_restore_all; then
  restore23_fail=0
else
  restore23_fail=$?
fi
check "uninstall reports unavailable system restoration" "2" "$restore23_fail"
check "uninstall retains working user shadow on system failure" "yes" \
  "$([ -f "$U23/steam.desktop" ] && echo yes || echo no)"
check "uninstall retains system original for retry" "yes" \
  "$([ -f "$sys23_backup" ] && echo yes || echo no)"
check "uninstall leaves failed system entry covered" "patched" \
  "$(dc_classify "$S23/steam.desktop")"

if XDG_DATA_HOME="$H23/data" XDG_CONFIG_HOME="$H23/conf" DC_HOME="$H23" \
  DC_BACKUP_ROOT="$H23/backup" DC_SYS_APPS="$S23" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="" dc_restore_all; then restore23_ok=0; else restore23_ok=$?; fi
check "uninstall succeeds when mutable restoration becomes available" "0" "$restore23_ok"
check "successful uninstall restores system original" "Exec=/usr/bin/steam %U" \
  "$(grep -m1 '^Exec=' "$S23/steam.desktop")"
check "successful uninstall removes seeded user shadow" "no" \
  "$([ -e "$U23/steam.desktop" ] && echo yes || echo no)"
if XDG_DATA_HOME="$H23/data" XDG_CONFIG_HOME="$H23/conf" DC_HOME="$H23" \
  DC_BACKUP_ROOT="$H23/backup" DC_SYS_APPS="$S23" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="" dc_restore_all; then restore23_again=0; else restore23_again=$?; fi
check "second uninstall is a successful no-op" "0" "$restore23_again"

# A legacy system backup with its active file missing must not be silently lost
# when privilege is unavailable; user coverage remains until migration succeeds.
H24="$TMP/home24"; U24="$H24/data/applications"; S24="$TMP/sys24/applications"
mkdir -p "$U24" "$H24/conf/autostart" "$S24"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$S24/steam.desktop.slssteam-backup"
printf '[Desktop Entry]\n%s\n%s\nName=Steam\nExec=%s %%U\n' \
  "$DC_SEED_TAG" "$DC_TAG" "$WRAPPER" > "$U24/steam.desktop"
if XDG_DATA_HOME="$H24/data" XDG_CONFIG_HOME="$H24/conf" DC_HOME="$H24" \
  DC_BACKUP_ROOT="$H24/backup" DC_SYS_APPS="$S24" DC_SYS_AUTOSTART="$TMP/none" \
  DC_SUDO="$SUDO_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" dc_restore_all; then
  restore24=0
else
  restore24=$?
fi
check "uninstall reports failed orphaned system-backup migration" "2" "$restore24"
check "orphaned system backup failure retains user coverage" "yes" \
  "$([ -f "$U24/steam.desktop" ] && echo yes || echo no)"
check "orphaned adjacent backup remains retryable" "yes" \
  "$([ -f "$S24/steam.desktop.slssteam-backup" ] && echo yes || echo no)"

# Optional mutable-system fallback must propagate a write failure instead of
# setting setup's system_desktop_changed flag on a false success.
H25="$TMP/home25"; S25="$TMP/sys25/applications"
mkdir -p "$H25/data/applications" "$H25/conf/autostart" "$TMP/empty25/applications" "$S25"
printf '[Desktop Entry]\nName=Steam\nExec=/usr/bin/steam %%U\n' > "$S25/steam.desktop"
sys25_before="$(sha256sum "$S25/steam.desktop" | awk '{print $1}')"
if XDG_DATA_HOME="$H25/data" XDG_DATA_DIRS="$TMP/empty25" XDG_CONFIG_HOME="$H25/conf" \
  DC_HOME="$H25" DC_BACKUP_ROOT="$H25/backup" DC_SYS_APPS="$S25" \
  DC_SYS_AUTOSTART="$TMP/none" DC_SUDO="$SUDO_SHIM" DC_TEST_EVENTS="$GUARDIAN_EVENTS" \
  DC_STEAM_INSTALLED=1 dc_run --system; then system25_status=0; else system25_status=$?; fi
check "system fallback propagates failed privileged patch" "2" "$system25_status"
check "failed system fallback leaves source unchanged" "$sys25_before" \
  "$(sha256sum "$S25/steam.desktop" | awk '{print $1}')"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
