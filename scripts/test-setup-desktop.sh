#!/usr/bin/env bash
# setup.sh's desktop integration, run with logging stubs for everything it
# delegates: which steps each kind of host gets and in which order, the
# launcher shim preflight, uninstall, and the immutable, restore and legacy
# restore paths below.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
ck(){ if [ "$2" = yes ]; then echo "ok   - $1"; else echo "FAIL - $1"; fail=1; fi; }
eq(){ if [ "$2" = "$3" ]; then echo "ok   - $1"; else echo "FAIL - $1 (want=[$2] got=[$3])"; fail=1; fi; }
# in_order <log> <line>...: every line is present, in this order.
in_order(){
  local log="$1" prev=0 n line; shift
  for line in "$@"; do
    n="$(printf '%s\n' "$log" | grep -nxF -- "$line" | head -1 | cut -d: -f1)"
    [ -n "$n" ] && [ "$n" -gt "$prev" ] || { echo no; return; }
    prev="$n"
  done
  echo yes
}
has_line(){ printf '%s\n' "$1" | grep -qxF -- "$2" && echo yes || echo no; }
has_prefix(){ printf '%s\n' "$1" | grep -q "^$2" && echo yes || echo no; }

WIRE_TMP="$(mktemp -d)"
trap 'rm -rf "$WIRE_TMP"' EXIT
# The words a stubbed run understands. The stubs log one line per call.
WIRE_STUBS='
  has() { case "$WIRE_WORDS" in *" $1 "*) return 0 ;; esac; return 1; }
  say() { printf "%s\n" "$*" >> "$WIRE_LOG"; }
  log_info() { :; }; log_success() { :; }
  log_warn() { say "warn $*"; }; log_error() { say "error $*"; }
  print_banner() { :; }; print_section() { :; }; print_uninstall_complete() { :; }
  kill_steam() { say kill_steam; }
  detect_steam_binary() { echo /usr/bin/steam; }
  ls_steam_bootstrapped() { ! has prebootstrap; }
  is_immutable_distro() { has immutable; }
  sudo() { say "sudo $*"; ! has sudo-denied; }
  systemctl() { has guardian-inactive && return 1; return 0; }
  update-desktop-database() { :; }
'
# wire <function> <word>...: run one setup.sh function in a fresh home.
wire() {
  local fn="$1" home; shift
  home="$(mktemp -d "$WIRE_TMP/home.XXXXXX")"
  mkdir -p "$home/.local/share/SLSsteam"
  # The calls log to a file: setup.sh sends some of them to /dev/null and reads
  # the output of others.
  HOME="$home" SLS_SETUP_LIB_ONLY=1 WIRE_WORDS=" $* " WIRE_FN="$fn" WIRE_LOG="$home/wire.log" bash -c '
    source "$1/setup.sh" >/dev/null 2>&1
    eval "$2"
    dc_guardian_run() { say dc_guardian_run; ! has guardian-fails; }
    dgu_install_units() { say dgu_install_units; ! has units-unchanged; }
    dgu_enable_units() { say dgu_enable_units; }
    dgu_install_autostart_dropins() { say dgu_install_autostart_dropins; }
    dgu_remove_autostart_dropins() { say dgu_remove_autostart_dropins; }
    dgu_remove_units() { say dgu_remove_units; }
    dc_write_policy() { say "dc_write_policy $1"; ! has policy-fails; }
    dc_forget_policy() { say dc_forget_policy; }
    dc_migrate_legacy_backups() { say "dc_migrate_legacy_backups $*"; }
    dc_run() { say "dc_run $* policy=${SLSM_COVERAGE_POLICY:-} sudo=${DC_SUDO:-}"; }
    dc_restore_all() { say "dc_restore_all sudo=${DC_SUDO:-}"; ! has restore-fails; }
    restore_system_launchers() { say restore_system_launchers; }
    ls_detect_launchers() { say ls_detect_launchers; echo "$HOME/no-such-dir/steam"; }
    ls_install_shims() { say "ls_install_shims sudo=$LS_SUDO"; }
    has no-launcher && setup_system_launcher() { say setup_system_launcher; return 1; }
    has launcher && setup_system_launcher() { say setup_system_launcher; return 0; }
    has declined && SLSM_SUDO_DENIED=1
    unset SUDO_ASKPASS SLSM_SUDO_PRIMED
    "$WIRE_FN"; say "status $?"
    [ -d "$DC_BACKUP_ROOT" ] && say backup-root
    [ -f "$SLSDIR/coverage-policy.effective" ] && say "effective $(cat "$SLSDIR/coverage-policy.effective")"
    [ -d "$SLSDIR" ] || say slsdir-removed
    grep -m1 "^Exec=" "$USER_DESKTOP" >> "$WIRE_LOG" 2>/dev/null || true
  ' bash "$HERE" "$WIRE_STUBS" > /dev/null 2>&1
  sed "s#$home#HOME#g" "$home/wire.log"
}

# The libraries setup.sh delegates to are loaded when it is sourced.
LOADED="$(HOME="$WIRE_TMP" SLS_SETUP_LIB_ONLY=1 bash -c '
  source "$1/setup.sh" >/dev/null 2>&1
  type dc_guardian_run dgu_install_units ls_install_shims >/dev/null 2>&1 && echo yes || echo no
' bash "$HERE")"
ck "setup loads the coverage, guardian unit and launcher shim libraries" "$LOADED"

# Mutable host whose distro launcher can be wrapped: user coverage first, then
# the shim, which becomes the policy; no administrator prompt.
LOG="$(wire setup_path_and_desktop launcher)"
ck "user reconciliation, guardian units and drop-ins precede the launcher shim" \
   "$(in_order "$LOG" dc_guardian_run dgu_install_units dgu_install_autostart_dropins setup_system_launcher)"
ck "a wrapped launcher is persisted as the policy after the shim attempt" \
   "$(in_order "$LOG" setup_system_launcher "dc_write_policy launcher")"
eq "a wrapped launcher asks for no administrator access" "no" "$(has_prefix "$LOG" 'sudo ')"
eq "a wrapped launcher leaves the system entries alone" "no" "$(has_prefix "$LOG" 'dc_run --system')"
ck "setup creates the central desktop backup folder" "$(has_line "$LOG" backup-root)"
ck "setup reports success" "$(has_line "$LOG" 'status 0')"
ck "a bootstrapped Steam gets a minimal launcher through the wrapper" \
   "$(has_line "$LOG" 'Exec=HOME/.local/share/SLSsteam/path/steam %U')"

# Mutable host without a usable launcher: the desktop policy, then the system
# entries through sudo after migrating their legacy backups.
LOG="$(wire setup_path_and_desktop no-launcher)"
ck "user reconciliation precedes the administrator prompt" \
   "$(in_order "$LOG" dc_guardian_run "dc_write_policy desktop" "sudo -v" \
      "dc_migrate_legacy_backups --system" "dc_run --system policy=desktop sudo=sudo")"
ck "a patched system layer refreshes the system desktop database" \
   "$(has_line "$LOG" 'sudo update-desktop-database /usr/share/applications')"

# Administrator access declined: a warning that names the per-user coverage,
# no system pass, and the install still succeeds.
LOG="$(wire setup_path_and_desktop no-launcher sudo-denied)"
eq "declined sudo runs no system pass" "no" "$(has_prefix "$LOG" 'dc_run --system')"
ck "declined sudo warns that the per-user entry still covers launches" \
   "$(printf '%s\n' "$LOG" | grep '^warn ' | grep -q 'per-user entry' && echo yes || echo no)"
ck "declined sudo does not abort the install" "$(has_line "$LOG" 'status 0')"
LOG="$(wire setup_path_and_desktop no-launcher declined)"
eq "access declined earlier in the run is not asked for again" "no" "$(has_prefix "$LOG" 'sudo ')"

# Immutable host: user coverage only.
LOG="$(wire setup_path_and_desktop no-launcher immutable)"
eq "an immutable host asks for no administrator access" "no" "$(has_prefix "$LOG" 'sudo ')"
eq "an immutable host runs no system pass" "no" "$(has_prefix "$LOG" 'dc_run --system')"

# Guardian outcomes: a failed reconciliation still installs the units, unchanged
# units are enabled again, and units the user manager did not enable are named.
LOG="$(wire setup_path_and_desktop launcher guardian-fails)"
ck "a failed user reconciliation still installs the guardian units" \
   "$(in_order "$LOG" dc_guardian_run dgu_install_units dgu_install_autostart_dropins)"
LOG="$(wire setup_path_and_desktop launcher units-unchanged)"
ck "unchanged guardian units are enabled again" "$(in_order "$LOG" dgu_install_units dgu_enable_units)"
LOG="$(wire setup_path_and_desktop launcher guardian-inactive)"
ck "guardian units the user manager did not enable are reported" \
   "$(printf '%s\n' "$LOG" | grep -q '^warn Desktop guardian installed but NOT active' && echo yes || echo no)"

# A policy that cannot be written falls back to the desktop layer in the same
# run, clears the old policy and records the fallback.
LOG="$(wire setup_path_and_desktop launcher policy-fails)"
ck "an unwritable policy clears the previous one" "$(in_order "$LOG" "dc_write_policy launcher" dc_forget_policy)"
ck "an unwritable policy records the desktop fallback" "$(has_line "$LOG" 'effective desktop')"
ck "an unwritable policy patches the system entries in the same run" \
   "$(has_line "$LOG" 'dc_run --system policy=desktop sudo=sudo')"

# Before Steam's first bootstrap the minimal launcher calls the distro launcher
# directly, and no launcher shim is attempted.
LOG="$(wire setup_path_and_desktop no-launcher prebootstrap)"
ck "a pre-bootstrap minimal launcher stays vanilla" "$(has_line "$LOG" 'Exec=/usr/bin/steam %U')"
LOG="$(wire setup_system_launcher prebootstrap)"
eq "a pre-bootstrap host leaves the distro launcher alone" "status 1" "$LOG"

# Launcher shim preflight: launchers are found before sudo is asked for, and the
# shared library installs the shims with that privilege.
LOG="$(wire setup_system_launcher)"
ck "launchers are found before the shim asks for sudo" \
   "$(in_order "$LOG" ls_detect_launchers "sudo -v" "ls_install_shims sudo=sudo" "status 0")"
eq "an immutable host never touches the distro launcher" "status 1" "$(wire setup_system_launcher immutable)"

# Uninstall: generated launch paths go first, system entries restore through
# sudo, then the launchers, and only a complete restore removes the folder.
LOG="$(wire uninstall)"
ck "uninstall removes guardian state before restoring entries and launchers" \
   "$(in_order "$LOG" dgu_remove_autostart_dropins dgu_remove_units "dc_restore_all sudo=sudo" \
      restore_system_launchers dc_forget_policy slsdir-removed)"
LOG="$(wire uninstall restore-fails)"
ck "an incomplete restore keeps the user coverage and says so" \
   "$(printf '%s\n' "$LOG" | grep -q '^warn .*retaining user desktop coverage' && \
      ! printf '%s\n' "$LOG" | grep -qx slsdir-removed && echo yes || echo no)"
ck "an immutable uninstall restores without sudo" "$(has_line "$(wire uninstall immutable)" 'dc_restore_all sudo=')"

# Immutable detection: os-release fixtures and the explicit override. The probe
# also asks rpm-ostree, steamos-readonly and findmnt, so it runs on a PATH that
# has only a findmnt reporting a writable root (test sandboxes mount / read-only).
mkdir -p "$WIRE_TMP/osbin"
printf '#!/bin/sh\necho rw,relatime\n' > "$WIRE_TMP/osbin/findmnt"
chmod +x "$WIRE_TMP/osbin/findmnt"
immutable_for(){
  printf '%s\n' "$1" > "$WIRE_TMP/os-release"
  OS_RELEASE_FILE="$WIRE_TMP/os-release" SLSM_IMMUTABLE="${2:-}" SLS_SETUP_LIB_ONLY=1 HOME="$WIRE_TMP" \
    bash -c 'source "$1/setup.sh" >/dev/null 2>&1; PATH="$2"; is_immutable_distro && echo yes || echo no' \
    bash "$HERE" "$WIRE_TMP/osbin"
}
eq "an ordinary os-release reads as mutable" "no" "$(immutable_for 'ID=ubuntu')"
eq "SteamOS reads as immutable" "yes" "$(immutable_for 'ID=steamos')"
eq "SLSM_IMMUTABLE=1 overrides a mutable os-release" "yes" "$(immutable_for 'ID=ubuntu' 1)"

# Behavioral regression: an immutable host should normally have no system shim,
# but an interrupted historical uninstall may leave one behind. In that case
# setup.sh must report failure and retain both the backup flag and full helper
# tree instead of returning success before the cleanup guard can act.
SETUP_TMP="$(mktemp -d)"
trap 'rm -rf "$WIRE_TMP" "$SETUP_TMP" "${MISMATCH_TMP:-}"' EXIT
SETUP_HOME="$SETUP_TMP/home"
SETUP_BIN="$SETUP_TMP/bin"
SETUP_OS_RELEASE="$SETUP_TMP/os-release"
mkdir -p "$SETUP_HOME/.local/share/SLSsteam" "$SETUP_BIN"
printf 'ID=steamos\n' > "$SETUP_OS_RELEASE"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec /broken/immutable-setup\n' > "$SETUP_BIN/steam"
chmod 0755 "$SETUP_BIN/steam"
SETUP_IMMUTABLE_STATE="$(
  HOME="$SETUP_HOME" \
  OS_RELEASE_FILE="$SETUP_OS_RELEASE" \
  SLS_SETUP_LIB_ONLY=1 \
  bash -c '
    source "$1/setup.sh"
    log_warn() { :; }
    LS_LAUNCHER_DIRS=("$2")
    SLSM_KEEP_LAUNCHER_BACKUP=0
    SLSM_KEEP_LAUNCHER_TREE=0
    if restore_system_launchers; then result=yes; else result=no; fi
    printf "%s:%s:%s" "$result" "$SLSM_KEEP_LAUNCHER_BACKUP" "$SLSM_KEEP_LAUNCHER_TREE"
  ' bash "$HERE" "$SETUP_BIN"
)"
ck "setup immutable shim reports incomplete restore" \
   "$([ "$SETUP_IMMUTABLE_STATE" = "no:1:1" ] && echo yes || echo no)"

# A usable mirrored file that is not referenced by the live shim is not a safe
# restore fallback; retain the complete helper tree for a later retry instead.
MISMATCH_TMP="$(mktemp -d)"
MISMATCH_HOME="$MISMATCH_TMP/home"
MISMATCH_BIN="$MISMATCH_TMP/usr/bin"
MISMATCH_OS_RELEASE="$MISMATCH_TMP/os-release"
MISMATCH_BACKUP_ROOT="$MISMATCH_HOME/.local/share/SLSsteam/system-launcher-backup"
MISMATCH_BACKUP="$MISMATCH_BACKUP_ROOT/${MISMATCH_BIN#/}/steam.orig"
mkdir -p "$MISMATCH_BIN" "$(dirname "$MISMATCH_BACKUP")" "$MISMATCH_HOME/.local/share/SLSsteam"
printf 'ID=ubuntu\n' > "$MISMATCH_OS_RELEASE"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/mismatch\n' \
  "$MISMATCH_BACKUP_ROOT/wrong.orig" > "$MISMATCH_BIN/steam"
printf '#!/bin/sh\nprintf "MISMATCH-BACKUP\\n"\n' > "$MISMATCH_BACKUP"
chmod 0755 "$MISMATCH_BIN/steam" "$MISMATCH_BACKUP"
MISMATCH_STATE="$(
  HOME="$MISMATCH_HOME" \
  OS_RELEASE_FILE="$MISMATCH_OS_RELEASE" \
  SLS_SETUP_LIB_ONLY=1 \
  bash -c '
    source "$1/setup.sh"
    log_warn() { :; }
    LS_LAUNCHER_DIRS=("$2")
    SLSM_KEEP_LAUNCHER_BACKUP=0
    SLSM_KEEP_LAUNCHER_TREE=0
    if restore_system_launchers; then result=yes; else result=no; fi
    printf "%s:%s:%s" "$result" "$SLSM_KEEP_LAUNCHER_BACKUP" "$SLSM_KEEP_LAUNCHER_TREE"
  ' bash "$HERE" "$MISMATCH_BIN"
)"
ck "setup rejects an unassociated mirrored restore fallback" \
   "$([ "$MISMATCH_STATE" = "no:1:1" ] && echo yes || echo no)"

# The legacy per-file restore (used when the coverage lib is missing) copies a
# system entry back through the privilege prefix it was given.
LEGACY_TMP="$(mktemp -d)"
LEGACY_HOME="$LEGACY_TMP/home"
LEGACY_BIN="$LEGACY_TMP/bin"
LEGACY_DESKTOP="$LEGACY_TMP/applications/steam.desktop"
LEGACY_BACKUP="$LEGACY_HOME/.local/share/SLSsteam/backup/${LEGACY_DESKTOP#/}"
mkdir -p "$LEGACY_BIN" "$(dirname "$LEGACY_DESKTOP")" "$(dirname "$LEGACY_BACKUP")"
printf '[Desktop Entry]\nExec=wrapped\n' > "$LEGACY_DESKTOP"
printf '[Desktop Entry]\nExec=steam\n' > "$LEGACY_BACKUP"
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "%s/sudo.log"\nexec "$@"\n' "$LEGACY_TMP" > "$LEGACY_BIN/sudo"
chmod 0755 "$LEGACY_BIN/sudo"
HOME="$LEGACY_HOME" PATH="$LEGACY_BIN:$PATH" SLS_SETUP_LIB_ONLY=1 \
  bash -c 'source "$1/setup.sh"; restore_or_remove_desktop "$2" sudo' \
  bash "$HERE" "$LEGACY_DESKTOP" > /dev/null 2>&1
ck "legacy desktop restore writes through the given sudo" \
   "$(grep -q " $LEGACY_DESKTOP\$" "$LEGACY_TMP/sudo.log" 2>/dev/null && echo yes || echo no)"
# The backup sits in the user's home, so the user's shell opens it, not root.
ck "legacy desktop restore never names the backup to sudo" \
   "$(grep -qF "$LEGACY_BACKUP" "$LEGACY_TMP/sudo.log" 2>/dev/null && echo no || echo yes)"
ck "legacy desktop restore puts the entry back and drops the backup" \
   "$(grep -q 'Exec=steam' "$LEGACY_DESKTOP" && [ ! -e "$LEGACY_BACKUP" ] && echo yes || echo no)"
rm -rf "$LEGACY_TMP"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
