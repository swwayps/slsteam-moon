#!/usr/bin/env bash
# Static wiring checks: setup.sh sources the coverage lib, calls dc_run --system
# at install, the lib carries the 0644 fix, and the generated wrapper re-asserts
# coverage each launch.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
ck(){ if [ "$2" = yes ]; then echo "ok   - $1"; else echo "FAIL - $1"; fail=1; fi; }

ck "setup.sh sources the coverage lib" \
   "$(grep -q 'desktop-coverage.lib.sh' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup.sh calls dc_run --system" \
   "$(grep -qE 'dc_run[[:space:]]+(--system|"--system")' "$HERE/setup.sh" && echo yes || echo no)"
ck "lib applies 0644 (Cinnamon perms fix, not chmod +x)" \
   "$(grep -q 'chmod 0644' "$HERE/tools/desktop-coverage.lib.sh" && echo yes || echo no)"
ck "wrapper body re-asserts coverage each launch (--user)" \
   "$(grep -q 'ensure-desktop-coverage.sh" --user' "$HERE/setup.sh" && echo yes || echo no)"
ck "immutable distros run --user only (no system patch attempt)" \
   "$(grep -q 'is_immutable_distro' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup creates the central desktop-backup directory" \
   "$(grep -q 'SLSsteam/backup\|SLSDIR/backup' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup invokes legacy-backup migration before desktop repatch" \
   "$(grep -q 'dc_migrate_legacy_backups' "$HERE/setup.sh" && echo yes || echo no)"
ck "desktop helper no longer assigns adjacent backups" \
   "$(! grep -q 'bak="\$f.slssteam-backup"' "$HERE/tools/desktop-coverage.lib.sh" && echo yes || echo no)"
ck "immutable setup skips the system desktop database refresh" \
   "$(grep -q '\[ "\$system_desktop_changed" = 1 \].*command -v sudo' "$HERE/setup.sh" && echo yes || echo no)"

ck "setup sources guardian unit helper" \
   "$(grep -q 'desktop-guardian-units.lib.sh' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup performs mandatory guardian user reconciliation" \
   "$(grep -q 'dc_guardian_run' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup installs guardian units and generated drop-ins" \
   "$(grep -q 'dgu_install_units' "$HERE/setup.sh" && grep -q 'dgu_install_autostart_dropins' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup retries enabling byte-identical guardian units" \
   "$(grep -q '\[ "$guardian_status" = 1 \].*dgu_enable_units' "$HERE/setup.sh" && echo yes || echo no)"
ck "mandatory user reconciliation precedes sudo attempt" \
   "$(awk '/dc_guardian_run/{g=NR} /sudo -v/{s=NR} END{print (g && s && g<s)?"yes":"no"}' "$HERE/setup.sh")"
ck "sudo denial is warning-only rather than installation abort" \
   "$(awk '/if ! sudo -v/{inblock=1} inblock && /exit 1/{bad=1} inblock && /^\tfi/{inblock=0} END{print bad?"no":"yes"}' "$HERE/setup.sh")"
ck "wrapper prefers guardian service and retains CLI fallback" \
   "$(grep -q 'is-enabled slsteam-desktop-guardian.path' "$HERE/setup.sh" && grep -q 'start slsteam-desktop-guardian.service' "$HERE/setup.sh" && grep -q 'ensure-desktop-coverage.sh" --user' "$HERE/setup.sh" && echo yes || echo no)"
ck "uninstall removes guardian state before desktop restoration" \
   "$(awk '/dgu_remove_autostart_dropins/{d=NR} /dc_restore_all/{r=NR} END{print (d && r && d<r)?"yes":"no"}' "$HERE/setup.sh")"
ck "uninstall warns when system restore is deferred" \
   "$(grep -q 'retaining user desktop coverage' "$HERE/setup.sh" && echo yes || echo no)"

ck "guardian reconciliation failure does not abort unit installation" \
   "$(awk '/dc_guardian_run/{seen=1} seen && /dgu_install_units/{done=1} seen && !done && /return 1/{bad=1} END{print bad?"no":"yes"}' "$HERE/setup.sh")"
ck "setup verifies the guardian actually activated (is-enabled) and warns" \
   "$(awk '/setup_path_and_desktop\(\)/{f=1} f && /is-enabled slsteam-desktop-guardian.path/{a=1} f && /Cold-boot/{w=1} /^}/{if(f)exit} END{print (a&&w)?"yes":"no"}' "$HERE/setup.sh")"
ck "sudo-denied warning explains per-user coverage still applies" \
   "$(grep -q 'system-wide Steam entry stays unpatched' "$HERE/setup.sh" && grep -q 'per-user entry' "$HERE/setup.sh" && echo yes || echo no)"
ck "successful sudo fallback propagates its privilege prefix" \
   "$(grep -q 'DC_SUDO="sudo"' "$HERE/setup.sh" && echo yes || echo no)"

ck "setup exposes an overrideable os-release path" \
   "$(grep -qE '^OS_RELEASE_FILE=.*(/etc/os-release)' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup supports an explicit immutable override" \
   "$(grep -q 'SLSM_IMMUTABLE' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup sources the shared launcher shim library" \
   "$(grep -q 'launcher-shim.lib.sh' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup delegates launcher installation to the shared library" \
   "$(grep -q 'ls_install_shims' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup delegates launcher restoration to the shared library" \
   "$(grep -q 'ls_restore_shims' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup retains full tree when launcher original is unrecoverable" \
   "$(grep -q 'SLSM_KEEP_LAUNCHER_TREE' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup checks bootstrap before marking Steam installed" \
   "$(grep -q 'ls_steam_bootstrapped' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup has a source-only test guard" \
   "$(grep -q 'SLS_SETUP_LIB_ONLY' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup persists the effective coverage policy" \
   "$(grep -q 'dc_write_policy' "$HERE/setup.sh" && echo yes || echo no)"
ck "mandatory user reconciliation precedes launcher shim attempt" \
   "$(awk '/dc_guardian_run/{g=NR} /setup_system_launcher/{s=NR} END{print (g && s && g<s)?"yes":"no"}' "$HERE/setup.sh")"
ck "launcher shim attempt precedes policy persistence" \
   "$(awk '/setup_system_launcher/{s=NR} /dc_write_policy/{p=NR} END{print (s && p && s<p)?"yes":"no"}' "$HERE/setup.sh")"
ck "policy persistence precedes optional system desktop fallback" \
   "$(awk '/dc_write_policy/{p=NR} /dc_run --system/{s=NR} END{print (p && s && p<s)?"yes":"no"}' "$HERE/setup.sh")"
ck "setup uses a vanilla launcher before Steam bootstrap" \
   "$(grep -q 'steam_bootstrapped.*= 1' "$HERE/setup.sh" && grep -q 'minimal_exec="\$steam_bin"' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup wrapper has a pre-bootstrap guard" \
   "$(grep -q 'slsm_bootstrap_guard' "$HERE/setup.sh" && echo yes || echo no)"
ck "failed policy write forces the same-run desktop fallback" \
   "$(grep -q 'SLSM_COVERAGE_POLICY=desktop' "$HERE/setup.sh" && echo yes || echo no)"
ck "failed policy write clears stale persisted state" \
   "$(awk '/^setup_path_and_desktop\(\)/{inside=1} inside && /dc_forget_policy/{found=1} inside && /^}/{exit} END{print found?"yes":"no"}' "$HERE/setup.sh")"
ck "failed policy write records desktop fallback state" \
   "$(grep -q 'coverage-policy.effective' "$HERE/setup.sh" && echo yes || echo no)"
ck "launcher denial skips optional system sudo fallback" \
   "$(awk '/^setup_path_and_desktop\(\)/{inside=1} inside && /SLSM_SUDO_DENIED/{found=1} inside && /^}/{exit} END{print found?"yes":"no"}' "$HERE/setup.sh")"
ck "setup discovers launchers before shim sudo preflight" \
   "$(awk '/setup_system_launcher\(\)/{inside=1} inside && /ls_detect_launchers/{l=NR} inside && /sudo -v/{s=NR; exit} END{print (l && s && l<s)?"yes":"no"}' "$HERE/setup.sh")"
ck "setup no longer contains the inline launcher writer" \
   "$(! grep -q 'tmp_shim' "$HERE/setup.sh" && echo yes || echo no)"
ck "runtime coverage derives installed state from bootstrap" \
   "$(grep -q 'ls_steam_bootstrapped' "$HERE/ensure-desktop-coverage.sh" && echo yes || echo no)"
ck "wrapper bootstrap guard requires the canonical Steam symlink" \
   "$(grep -q '\\[ -L \"\\$HOME/.steam/steam\" \\]' "$HERE/setup.sh" && echo yes || echo no)"

# Behavioral regression: an immutable host should normally have no system shim,
# but an interrupted historical uninstall may leave one behind. In that case
# setup.sh must report failure and retain both the backup flag and full helper
# tree instead of returning success before the cleanup guard can act.
SETUP_TMP="$(mktemp -d)"
trap 'rm -rf "$SETUP_TMP"' EXIT
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
ck "legacy desktop restore copies through the given sudo" \
   "$(grep -qxF "cp -- $LEGACY_BACKUP $LEGACY_DESKTOP" "$LEGACY_TMP/sudo.log" 2>/dev/null && echo yes || echo no)"
ck "legacy desktop restore puts the entry back and drops the backup" \
   "$(grep -q 'Exec=steam' "$LEGACY_DESKTOP" && [ ! -e "$LEGACY_BACKUP" ] && echo yes || echo no)"
rm -rf "$LEGACY_TMP"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
