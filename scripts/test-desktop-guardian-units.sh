#!/usr/bin/env bash
# Focused tests for desktop-guardian-units.lib.sh. Never contacts a real user manager.
set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
check() { # description expected actual
	if [ "$2" = "$3" ]; then printf 'ok   - %s\n' "$1"
	else printf 'FAIL - %s (want=[%s] got=[%s])\n' "$1" "$2" "$3"; fail=1; fi
}
contains() { # description haystack needle
	case "$2" in *"$3"*) printf 'ok   - %s\n' "$1" ;;
	*) printf 'FAIL - %s (missing=[%s])\n' "$1" "$3"; fail=1 ;;
	esac
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# This source is intentionally the first production dependency: the initial TDD
# run must fail here because the Task 5 library does not exist yet. The unit
# library itself must source the current coverage engine's XDG helpers.
unset -f dc_application_dirs dc_autostart_dirs dc_desktop_dir 2>/dev/null || true
# shellcheck source=/dev/null
. "$HERE/tools/desktop-guardian-units.lib.sh"
check "unit library sources coverage application helper" "yes" "$(type dc_application_dirs >/dev/null 2>&1 && echo yes || echo no)"
check "unit library sources coverage autostart helper" "yes" "$(type dc_autostart_dirs >/dev/null 2>&1 && echo yes || echo no)"
check "unit library sources coverage desktop helper" "yes" "$(type dc_desktop_dir >/dev/null 2>&1 && echo yes || echo no)"

service="$(dgu_service_content)"
contains "service has oneshot section" "$service" $'[Service]\nType=oneshot'
contains "service invokes guardian safely" "$service" 'ExecStart=%h/.local/share/SLSsteam/ensure-desktop-coverage.sh --guardian'

timer="$(dgu_timer_content)"
contains "timer starts after login" "$timer" 'OnStartupSec=30s'
contains "timer repeats every five minutes" "$timer" 'OnUnitActiveSec=5min'
contains "timer is persistent" "$timer" 'Persistent=true'

USER_APPS="$TMP/User Data/applications"
USER_AUTOSTART="$TMP/User Config/autostart"
USER_DESKTOP="$TMP/My Desktop"
SYS_APPS="$TMP/System Data/applications"
SYS_AUTOSTART="$TMP/System Config/autostart"
MISSING="$TMP/Missing System/applications"
mkdir -p "$USER_APPS" "$USER_AUTOSTART" "$USER_DESKTOP" "$SYS_APPS" "$SYS_AUTOSTART"

dc_application_dirs() {
	printf '%s\n' "$USER_APPS" "$SYS_APPS" "$SYS_APPS" "$MISSING"
}
dc_autostart_dirs() {
	printf '%s\n' "$USER_AUTOSTART" "$SYS_AUTOSTART" "$SYS_AUTOSTART"
}
dc_desktop_dir() { printf '%s\n' "$USER_DESKTOP"; }
path_unit="$(dgu_path_content)"
contains "path unit keeps spaced applications path literal" "$path_unit" "PathChanged=$USER_APPS"
contains "path unit keeps spaced autostart path literal" "$path_unit" "PathChanged=$USER_AUTOSTART"
contains "path unit keeps spaced desktop path literal" "$path_unit" "PathChanged=$USER_DESKTOP"
contains "path unit includes readable system applications" "$path_unit" "PathChanged=$SYS_APPS"
contains "path unit includes readable system autostart" "$path_unit" "PathChanged=$SYS_AUTOSTART"
check "path unit skips unavailable system directory" "0" "$(printf '%s\n' "$path_unit" | grep -cF "PathChanged=$MISSING")"
check "path unit deduplicates applications" "1" "$(printf '%s\n' "$path_unit" | grep -cF "PathChanged=$SYS_APPS")"
check "path unit deduplicates autostart" "1" "$(printf '%s\n' "$path_unit" | grep -cF "PathChanged=$SYS_AUTOSTART")"
contains "path unit targets guardian service" "$path_unit" 'Unit=slsteam-desktop-guardian.service'

FAKE_BIN="$TMP/fake commands"
mkdir -p "$FAKE_BIN"
DGU_CALLS="$TMP/systemctl.calls"
DGU_MV_LOG="$TMP/mv.calls"
DGU_ORDER_LOG="$TMP/remove-order.calls"
export DGU_CALLS DGU_MV_LOG DGU_ORDER_LOG
cat > "$FAKE_BIN/fake systemctl" <<'FAKE'
#!/bin/sh
{
	printf 'CALL'
	for arg do printf '\t<%s>' "$arg"; done
	printf '\n'
} >> "$DGU_CALLS"
case " $* " in
  *" disable "*)
    if [ -f "$DGU_UNIT_DIR/slsteam-desktop-guardian.service" ]; then
      printf 'files-present\n' >> "$DGU_ORDER_LOG"
    else
      printf 'files-missing\n' >> "$DGU_ORDER_LOG"
    fi
    ;;
esac
case " $* " in *" ${DGU_FAIL_MATCH:-__never__} "*) exit 42 ;; esac
exit 0
FAKE
cat > "$FAKE_BIN/mv" <<'FAKE'
#!/bin/sh
source= destination=
for arg do
	case "$arg" in
		-fT|--) continue ;;
	esac
	if [ -z "$source" ]; then source="$arg"
	else destination="$arg"
	fi
done
printf '%s\n%s\n' "$source" "$destination" >> "$DGU_MV_LOG"
exec /bin/mv "$@"
FAKE
chmod +x "$FAKE_BIN/fake systemctl" "$FAKE_BIN/mv"

DGU_UNIT_DIR="$TMP/Unit Dir With Spaces"
DGU_SYSTEMCTL="$FAKE_BIN/fake systemctl"
export DGU_UNIT_DIR DGU_SYSTEMCTL
PATH="$FAKE_BIN:$PATH"
export PATH
: > "$DGU_CALLS"; : > "$DGU_MV_LOG"

dgu_install_units
check "first install reports changed" "0" "$?"
check "service installed under override" "yes" "$([ -f "$DGU_UNIT_DIR/slsteam-desktop-guardian.service" ] && echo yes || echo no)"
check "service carries project ownership sentinel" "1" \
  "$(grep -cFx "$DGU_UNIT_SENTINEL" "$DGU_UNIT_DIR/slsteam-desktop-guardian.service" 2>/dev/null || true)"
check "path installed under override" "yes" "$([ -f "$DGU_UNIT_DIR/slsteam-desktop-guardian.path" ] && echo yes || echo no)"
check "timer installed under override" "yes" "$([ -f "$DGU_UNIT_DIR/slsteam-desktop-guardian.timer" ] && echo yes || echo no)"
check "installed service matches renderer" "$service" "$(cat "$DGU_UNIT_DIR/slsteam-desktop-guardian.service")"
check "installed path matches renderer" "$path_unit" "$(cat "$DGU_UNIT_DIR/slsteam-desktop-guardian.path")"
check "installed timer matches renderer" "$timer" "$(cat "$DGU_UNIT_DIR/slsteam-desktop-guardian.timer")"
check "changed install makes three manager calls" "3" "$(wc -l < "$DGU_CALLS")"
check "daemon reload arguments stay separate" $'CALL\t<--user>\t<daemon-reload>' "$(sed -n '1p' "$DGU_CALLS")"
check "enable arguments stay separate" $'CALL\t<--user>\t<enable>\t<--now>\t<slsteam-desktop-guardian.path>\t<slsteam-desktop-guardian.timer>' "$(sed -n '2p' "$DGU_CALLS")"
check "start arguments stay separate" $'CALL\t<--user>\t<start>\t<slsteam-desktop-guardian.service>' "$(sed -n '3p' "$DGU_CALLS")"
atomic="$(awk -v d="$DGU_UNIT_DIR/" 'NR % 2 == 1 && index($0, d) != 1 { bad=1 } END { print bad ? "no" : "yes" }' "$DGU_MV_LOG")"
check "atomic temp files are created in destination directory" "yes" "$atomic"
check "atomic install leaves no temporary files" "0" "$(compgen -G "$DGU_UNIT_DIR/.*.tmp.*" | wc -l)"

service_inode="$(stat -c '%i' "$DGU_UNIT_DIR/slsteam-desktop-guardian.service")"
: > "$DGU_CALLS"; : > "$DGU_MV_LOG"
dgu_install_units
check "unchanged install reports no reload needed" "1" "$?"
check "unchanged install preserves inode" "$service_inode" "$(stat -c '%i' "$DGU_UNIT_DIR/slsteam-desktop-guardian.service")"
check "unchanged install performs no replacement" "0" "$(wc -l < "$DGU_MV_LOG")"
check "unchanged install performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"

printf '%s\nstale\n' "$DGU_UNIT_SENTINEL" > "$DGU_UNIT_DIR/slsteam-desktop-guardian.timer"
: > "$DGU_CALLS"; : > "$DGU_MV_LOG"
dgu_install_units
check "repair reports changed" "0" "$?"
check "repair restores timer content" "$timer" "$(cat "$DGU_UNIT_DIR/slsteam-desktop-guardian.timer")"
check "repair alone reloads and enables units" "3" "$(wc -l < "$DGU_CALLS")"

printf '%s\nstale again\n' "$DGU_UNIT_SENTINEL" > "$DGU_UNIT_DIR/slsteam-desktop-guardian.timer"
: > "$DGU_CALLS"; : > "$DGU_MV_LOG"
if bash -e -c '. "$1"; dgu_install_units' _ "$HERE/tools/desktop-guardian-units.lib.sh"; then
	errexit_status=0
else
	errexit_status=$?
fi
check "mixed-state repair is safe for errexit callers" "0" "$errexit_status"
check "errexit repair restores later stale unit" "$timer" "$(cat "$DGU_UNIT_DIR/slsteam-desktop-guardian.timer")"
check "errexit repair still notifies manager" "3" "$(wc -l < "$DGU_CALLS")"

: > "$DGU_CALLS"
DGU_FAIL_MATCH=daemon-reload dgu_enable_units 2> "$TMP/enable.stderr"
check "best-effort enable returns success" "0" "$?"
check "manager failure does not suppress later calls" "3" "$(wc -l < "$DGU_CALLS")"
contains "manager failure is logged" "$(cat "$TMP/enable.stderr")" 'daemon-reload failed'

: > "$DGU_CALLS"
DGU_NO_SERVICE_START=1 dgu_enable_units
check "runtime unit refresh skips recursive service start" "2" "$(wc -l < "$DGU_CALLS")"
check "runtime unit refresh still enables path and timer" "1" \
  "$(grep -c $'CALL\t<--user>\t<enable>\t<--now>\t<slsteam-desktop-guardian.path>\t<slsteam-desktop-guardian.timer>' "$DGU_CALLS" 2>/dev/null || true)"
check "runtime unit refresh never starts its current service" "0" \
  "$(grep -c $'CALL\t<--user>\t<start>\t<slsteam-desktop-guardian.service>' "$DGU_CALLS" 2>/dev/null || true)"

: > "$DGU_CALLS"; : > "$DGU_ORDER_LOG"
dgu_remove_units
check "remove reports changed" "0" "$?"
check "remove disables units before deleting files" "files-present" "$(head -n 1 "$DGU_ORDER_LOG")"
check "remove deletes all three units" "0" "$(for f in service path timer; do [ -e "$DGU_UNIT_DIR/slsteam-desktop-guardian.$f" ] && printf x; done | wc -c)"
check "remove disables path and timer" $'CALL\t<--user>\t<disable>\t<--now>\t<slsteam-desktop-guardian.path>\t<slsteam-desktop-guardian.timer>' "$(sed -n '1p' "$DGU_CALLS")"
check "remove reloads manager" $'CALL\t<--user>\t<daemon-reload>' "$(sed -n '2p' "$DGU_CALLS")"

: > "$DGU_CALLS"
dgu_remove_units
check "empty remove reports unchanged" "1" "$?"
check "empty remove performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"

printf '# foreign unit\n[Service]\nType=oneshot\n' > "$DGU_UNIT_DIR/$DGU_SERVICE"
: > "$DGU_CALLS"; : > "$DGU_ORDER_LOG"
dgu_remove_units
check "foreign same-name unit reports unchanged" "1" "$?"
check "foreign same-name unit is preserved" "# foreign unit" \
  "$(head -n 1 "$DGU_UNIT_DIR/$DGU_SERVICE")"
check "foreign unit removal performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"

# Installation also fails closed before publishing a partial set when an
# existing same-name unit is not owned by this project.
: > "$DGU_CALLS"; : > "$DGU_MV_LOG"
dgu_install_units
dgu_foreign_install=$?
check "foreign same-name unit blocks install" "2" "$dgu_foreign_install"
check "foreign same-name unit survives install" "# foreign unit" \
  "$(head -n 1 "$DGU_UNIT_DIR/$DGU_SERVICE")"
check "foreign collision publishes no sibling units" "0" \
  "$({ [ -e "$DGU_UNIT_DIR/$DGU_PATH" ] && printf x; [ -e "$DGU_UNIT_DIR/$DGU_TIMER" ] && printf x; } | wc -c)"
check "foreign collision performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"
rm -f -- "$DGU_UNIT_DIR/$DGU_SERVICE"

mkdir -p "$DGU_UNIT_DIR/$DGU_SERVICE"
: > "$DGU_CALLS"; : > "$DGU_MV_LOG"
dgu_install_units 2> "$TMP/collision.stderr"
check "directory collision reports install error" "2" "$?"
check "directory collision receives no nested temp file" "0" "$(find "$DGU_UNIT_DIR/$DGU_SERVICE" -mindepth 1 -maxdepth 1 -type f | wc -l)"
check "directory collision performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"
rm -rf -- "$DGU_UNIT_DIR/$DGU_SERVICE" "$DGU_UNIT_DIR/$DGU_PATH" "$DGU_UNIT_DIR/$DGU_TIMER"

# A failed pure renderer must abort before publishing any partial unit set or
# contacting the manager.
dc_application_dirs() { return 7; }
: > "$DGU_CALLS"
dgu_install_units
check "renderer failure reports error" "2" "$?"
check "renderer failure publishes no units" "0" "$(for f in service path timer; do [ -e "$DGU_UNIT_DIR/slsteam-desktop-guardian.$f" ] && printf x; done | wc -c)"
check "renderer failure performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"

# Generated XDG-autostart drop-ins close the generator race while preserving
# ordinary launcher arguments and removing Desktop field codes.
WRAPPER="$TMP/Wrapper Dir/steam"
AUTO_USER="$TMP/Auto User/autostart"
AUTO_SYSTEM="$TMP/Auto System/autostart"
mkdir -p "$AUTO_USER" "$AUTO_SYSTEM"
dc_autostart_dirs() { printf '%s\n' "$AUTO_USER" "$AUTO_SYSTEM"; }

cat > "$TMP/autostart-steam.desktop" <<'EOF'
[Desktop Entry]
Name=Steam
Exec=/usr/bin/steam %U
EOF
cat > "$TMP/autostart-bazzite.desktop" <<'EOF'
[Desktop Entry]
Name=Steam
Exec=/usr/bin/bazzite-steam -silent %U
EOF
cat > "$TMP/autostart-env.desktop" <<'EOF'
[Desktop Entry]
Name=Steam
Exec=env FOO=1 /usr/bin/steam -silent %U
EOF
check "autostart unit name uses generated service template" \
  "app-steam@autostart.service" "$(dgu_autostart_unit_name steam.desktop 2>/dev/null || true)"
check "autostart args remove lone field code" "" \
  "$(dgu_autostart_args "$TMP/autostart-steam.desktop" 2>/dev/null || true)"
check "autostart args preserve ordinary flag" "-silent" \
  "$(dgu_autostart_args "$TMP/autostart-bazzite.desktop" 2>/dev/null || true)"
check "autostart args discard env prefix and field code" "-silent" \
  "$(dgu_autostart_args "$TMP/autostart-env.desktop" 2>/dev/null || true)"

cp "$TMP/autostart-env.desktop" "$AUTO_USER/steam.desktop"
cp "$TMP/autostart-bazzite.desktop" "$AUTO_SYSTEM/com.valvesoftware.Steam.desktop"
rm -rf -- "$DGU_UNIT_DIR"; mkdir -p "$DGU_UNIT_DIR"; : > "$DGU_CALLS"
dgu_install_autostart_dropins
check "drop-in install reports changed" "0" "$?"
STEAM_DROPIN="$DGU_UNIT_DIR/app-steam@autostart.service.d/slsteam-guardian.conf"
COM_DROPIN="$DGU_UNIT_DIR/app-com.valvesoftware.Steam@autostart.service.d/slsteam-guardian.conf"
check "steam drop-in is created for existing user autostart" "yes" \
  "$([ -f "$STEAM_DROPIN" ] && echo yes || echo no)"
check "system donor gets same-ID generated drop-in" "yes" \
  "$([ -f "$COM_DROPIN" ] && echo yes || echo no)"
check "drop-in carries project ownership sentinel" "1" \
  "$(grep -cFx "$DGU_AUTOSTART_SENTINEL" "$STEAM_DROPIN" 2>/dev/null || true)"
check "drop-in resets ExecStart before replacement" $'ExecStart=\nExecStart="'"$WRAPPER"$'" -silent' \
  "$(grep '^ExecStart=' "$STEAM_DROPIN" 2>/dev/null)"
check "drop-in change reloads user manager once" "1" \
  "$(grep -c $'CALL\t<--user>\t<daemon-reload>' "$DGU_CALLS" 2>/dev/null || true)"

# A converged pass is byte-idempotent and does not reload the manager.
steam_dropin_sum="$(sha256sum "$STEAM_DROPIN" | awk '{print $1}')"
: > "$DGU_CALLS"
dgu_install_autostart_dropins
check "converged drop-in install reports unchanged" "1" "$?"
check "converged drop-in remains byte-identical" "$steam_dropin_sum" \
  "$(sha256sum "$STEAM_DROPIN" | awk '{print $1}')"
check "converged drop-in performs no manager calls" "0" "$(wc -l < "$DGU_CALLS")"

# Never create autostart from nothing or overwrite a foreign same-name drop-in.
EMPTY_AUTO="$TMP/Empty Auto/autostart"; mkdir -p "$EMPTY_AUTO"
dc_autostart_dirs() { printf '%s\n' "$EMPTY_AUTO"; }
EMPTY_UNITS="$TMP/Empty Units"; mkdir -p "$EMPTY_UNITS"; DGU_UNIT_DIR="$EMPTY_UNITS"
: > "$DGU_CALLS"
dgu_install_autostart_dropins
check "no autostart source creates no drop-in" "0" \
  "$(find "$EMPTY_UNITS" -type f | wc -l)"
FOREIGN="$EMPTY_UNITS/app-steam@autostart.service.d/slsteam-guardian.conf"
mkdir -p "$(dirname "$FOREIGN")"; printf '# foreign\n' > "$FOREIGN"
cp "$TMP/autostart-steam.desktop" "$EMPTY_AUTO/steam.desktop"
dgu_install_autostart_dropins
check "foreign same-name drop-in is preserved" "# foreign" "$(cat "$FOREIGN")"

# Removal touches project-owned drop-ins only.
DGU_UNIT_DIR="$TMP/Unit Dir With Spaces"
dc_autostart_dirs() { printf '%s\n' "$AUTO_USER" "$AUTO_SYSTEM"; }
: > "$DGU_CALLS"
dgu_remove_autostart_dropins
check "remove deletes project steam drop-in" "no" \
  "$([ -e "$STEAM_DROPIN" ] && echo yes || echo no)"
check "remove deletes project secondary drop-in" "no" \
  "$([ -e "$COM_DROPIN" ] && echo yes || echo no)"
check "remove reloads manager after owned changes" "1" \
  "$(grep -c $'CALL\t<--user>\t<daemon-reload>' "$DGU_CALLS" 2>/dev/null || true)"
DGU_UNIT_DIR="$EMPTY_UNITS"
dgu_remove_autostart_dropins
check "remove preserves foreign drop-in" "# foreign" "$(cat "$FOREIGN")"

# The guardian CLI owns runtime convergence of path-unit layout and generated
# autostart drop-ins. This covers an autostart source created after setup.
CLI_HOME="$TMP/CLI Home"
CLI_SYSTEM_DATA="$TMP/CLI System Data"
CLI_SYSTEM_CONFIG="$TMP/CLI System Config"
CLI_UNITS="$CLI_HOME/config/systemd/user"
CLI_WRAPPER="$CLI_HOME/.local/share/SLSsteam/path/steam"
mkdir -p "$CLI_HOME/data/applications" "$CLI_HOME/config/autostart" \
         "$CLI_HOME/Desktop" "$CLI_SYSTEM_DATA/applications" \
         "$CLI_SYSTEM_CONFIG/autostart"
cat > "$CLI_SYSTEM_DATA/applications/steam.desktop" <<'EOF'
[Desktop Entry]
Name=Steam
Exec=/usr/bin/steam %U
EOF
cat > "$CLI_SYSTEM_CONFIG/autostart/steam.desktop" <<'EOF'
[Desktop Entry]
Name=Steam
Exec=/usr/bin/bazzite-steam -silent %U
EOF
: > "$DGU_CALLS"
HOME="$CLI_HOME" DC_HOME="$CLI_HOME" XDG_DATA_HOME="$CLI_HOME/data" \
  XDG_DATA_DIRS="$CLI_SYSTEM_DATA" XDG_CONFIG_HOME="$CLI_HOME/config" \
  XDG_CONFIG_DIRS="$CLI_SYSTEM_CONFIG" DC_SYS_APPS="$CLI_SYSTEM_DATA/applications" \
  DC_SYS_AUTOSTART="$CLI_SYSTEM_CONFIG/autostart" DC_STEAM_INSTALLED=1 \
  WRAPPER="$CLI_WRAPPER" DGU_UNIT_DIR="$CLI_UNITS" \
  DGU_SYSTEMCTL="$FAKE_BIN/fake systemctl" \
  bash "$HERE/ensure-desktop-coverage.sh" --guardian >/dev/null 2>&1
check "guardian CLI installs runtime path unit" "yes" \
  "$([ -f "$CLI_UNITS/slsteam-desktop-guardian.path" ] && echo yes || echo no)"
CLI_DROPIN="$CLI_UNITS/app-steam@autostart.service.d/slsteam-guardian.conf"
check "guardian CLI creates late autostart drop-in" "yes" \
  "$([ -f "$CLI_DROPIN" ] && echo yes || echo no)"
check "late autostart drop-in preserves ordinary flag" \
  "ExecStart=\"$CLI_WRAPPER\" -silent" \
  "$(sed -n '/^ExecStart=.* -silent$/p' "$CLI_DROPIN" 2>/dev/null)"

if [ "$fail" -eq 0 ]; then
	printf 'ALL PASS\n'
else
	printf 'TEST FAILURES\n' >&2
fi
exit "$fail"
