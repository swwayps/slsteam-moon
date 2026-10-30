#!/usr/bin/env bash
# TDD fixture tests for the distro Steam launcher shim.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'chmod -R u+w "$TMP" 2>/dev/null || true; rm -rf "$TMP"' EXIT
# The shim trusts a captured original only when root or the caller owns every
# folder above it. In a test sandbox / and /tmp are not root's (user
# namespace), so this stub reports them as root's, as on a real system.
mkdir -p "$TMP/stat-stub"
printf '#!/bin/sh\nlast=""\nfor a in "$@"; do last="$a"; done\ncase "$last:$*" in\n\t/:*%%u*|/tmp:*%%u*) echo 0; exit 0 ;;\nesac\nexec "%s" "$@"\n' \
  "$(command -v stat)" > "$TMP/stat-stub/stat"
chmod +x "$TMP/stat-stub/stat"
export PATH="$TMP/stat-stub:$PATH"
fail=0
check() {
  if [ "$2" = "$3" ]; then
    printf 'ok   - %s\n' "$1"
  else
    printf 'FAIL - %s (want=[%s] got=[%s])\n' "$1" "$2" "$3"
    fail=1
  fi
}

HOME="$TMP/home"
export HOME
mkdir -p "$HOME/.local/share/SLSsteam/path" "$TMP/usr/bin" "$TMP/usr/games" "$TMP/steam-root"
printf '#!/bin/sh\nexit 0\n' > "$TMP/steam-root/steam.sh"
chmod +x "$TMP/steam-root/steam.sh"
mkdir -p "$HOME/.steam"
ln -s "$TMP/steam-root" "$HOME/.steam/steam"

cat > "$HOME/.local/share/SLSsteam/path/steam" <<'EOF'
#!/bin/sh
printf 'WRAPPER bin=%s args=%s\n' "${SLSM_STEAM_BIN:-none}" "$*"
EOF
chmod +x "$HOME/.local/share/SLSsteam/path/steam"

printf '#!/bin/sh\nprintf "ORIG-BIN %s\\n" "$*"\n' > "$TMP/usr/bin/steam"
printf '#!/bin/sh\nprintf "ORIG-GAMES %s\\n" "$*"\n' > "$TMP/usr/games/steam"
chmod +x "$TMP/usr/bin/steam" "$TMP/usr/games/steam"

LS_SLSDIR="$HOME/.local/share/SLSsteam"
LS_BACKUP_ROOT="$LS_SLSDIR/system-launcher-backup"
# Never the host's /var/lib: cases that need root's tree set their own.
LS_SYSTEM_BACKUP_ROOT="$TMP/var-lib/slsteam-moon/launcher-backup"
LS_LAUNCHER_DIRS=("$TMP/usr/bin" "$TMP/usr/games")
LS_STEAM_ROOT="$TMP/steam-root"
LS_SUDO=""
# shellcheck source=/dev/null
. "$HERE/tools/launcher-shim.lib.sh"

check "backup paths do not collide" "different" \
  "$([ "$(ls_backup_path "$TMP/usr/bin/steam")" != "$(ls_backup_path "$TMP/usr/games/steam")" ] && echo different || echo collided)"
check "both launchers are detected" "2" "$(ls_detect_launchers | wc -l | tr -d ' ')"
check "both launchers are wrapped" "yes" "$(ls_install_shims && echo yes || echo no)"
check "bin launcher has the shim tag" "yes" "$(ls_is_our_shim "$TMP/usr/bin/steam" && echo yes || echo no)"
check "games launcher has the shim tag" "yes" "$(ls_is_our_shim "$TMP/usr/games/steam" && echo yes || echo no)"
check "two wrapped paths are reported" "2" "$(ls_wrapped_paths | wc -l | tr -d ' ')"

check "bin backup preserves its own original" "ORIG-BIN" \
  "$(sh "$(ls_backup_path "$TMP/usr/bin/steam")" | cut -d' ' -f1)"
check "games backup preserves its own original" "ORIG-GAMES" \
  "$(sh "$(ls_backup_path "$TMP/usr/games/steam")" | cut -d' ' -f1)"
# The wrapper is handed the `steam`-named alias for the captured original, never
# the `.orig` path itself: Valve's launcher aborts under any other argv[0].
# See scripts/test-launcher-basename.sh.
check "shim passes a name-safe original to wrapper" \
  "WRAPPER bin=$(ls_backup_alias_path "$TMP/usr/bin/steam") args=-silent" \
  "$(sh "$TMP/usr/bin/steam" -silent)"
check "shim alias resolves to the captured original" "$(ls_backup_path "$TMP/usr/bin/steam")" \
  "$(readlink -f "$(ls_backup_alias_path "$TMP/usr/bin/steam")")"

ls_install_shims
check "reinstall does not capture the shim" "ORIG-BIN" \
  "$(sh "$(ls_backup_path "$TMP/usr/bin/steam")" | cut -d' ' -f1)"

mv "$HOME/.local/share/SLSsteam/path/steam" "$TMP/wrapper-away"
check "missing wrapper falls through to original" "ORIG-BIN" \
  "$(sh "$TMP/usr/bin/steam" | cut -d' ' -f1)"
mv "$TMP/wrapper-away" "$HOME/.local/share/SLSsteam/path/steam"

mv "$(ls_backup_path "$TMP/usr/bin/steam")" "$TMP/backup-away"
mkdir -p "$HOME/.steam/steam"
printf '#!/bin/sh\nprintf "STEAMSH %s\\n" "$*"\n' > "$HOME/.steam/steam/steam.sh"
chmod +x "$HOME/.steam/steam/steam.sh"
rm -f "$HOME/.local/share/SLSsteam/path/steam"
check "missing wrapper and backup fall through to steam.sh" "STEAMSH" \
  "$(sh "$TMP/usr/bin/steam" | cut -d' ' -f1)"
mv "$TMP/backup-away" "$(ls_backup_path "$TMP/usr/bin/steam")"

# Restore through a fresh wrapper library state.
cat > "$HOME/.local/share/SLSsteam/path/steam" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$HOME/.local/share/SLSsteam/path/steam"
check "restore removes all shims" "yes" "$(ls_restore_shims && echo yes || echo no)"
check "bin original is restored" "ORIG-BIN" "$(sh "$TMP/usr/bin/steam" | cut -d' ' -f1)"
check "games original is restored" "ORIG-GAMES" "$(sh "$TMP/usr/games/steam" | cut -d' ' -f1)"
check "no wrapped launcher remains" "0" "$(ls_wrapped_paths | wc -l | tr -d ' ')"

# A launcher that cannot be written without a privilege prefix must be left
# untouched and must report failure. Root can write mode-0555 files, so report
# this write-barrier coverage as an explicit skip in root test environments.
mkdir -p "$TMP/readonly"
printf '#!/bin/sh\nprintf "ORIG-RO\\n"\n' > "$TMP/readonly/steam"
chmod 0555 "$TMP/readonly/steam" "$TMP/readonly"
LS_LAUNCHER_DIRS=("$TMP/readonly")
LS_SUDO=""
if [ "$(id -u)" -eq 0 ]; then
  printf 'skip - unwritable launcher checks (root can write anything)\n'
else
  check "unwritable launcher reports failure" "no" "$(ls_install_shims && echo yes || echo no)"
  check "unwritable launcher stays original" "ORIG-RO" "$(sh "$TMP/readonly/steam")"
fi

# Debian-family systems may expose an Install Steam package stub at the same
# launcher path before the real Steam tree exists. It must remain untouched
# until bootstrap has produced an executable steam.sh in the resolved root.
STUB_HOME="$TMP/stub-home"
STUB_ROOT="$TMP/stub-steam-root"
STUB_BIN="$TMP/stub-usr/games"
mkdir -p "$STUB_HOME/.local/share/SLSsteam/path" "$STUB_ROOT" "$STUB_BIN"
cat > "$STUB_HOME/.local/share/SLSsteam/path/steam" <<'EOF'
#!/bin/sh
printf 'WRAPPER-STUB bin=%s args=%s\n' "${SLSM_STEAM_BIN:-none}" "$*"
EOF
chmod +x "$STUB_HOME/.local/share/SLSsteam/path/steam"
printf '#!/bin/sh\nprintf "INSTALL-STUB %s\\n" "$*"\n' > "$STUB_BIN/steam"
chmod +x "$STUB_BIN/steam"
HOME="$STUB_HOME"
export HOME
LS_SLSDIR="$HOME/.local/share/SLSsteam"
LS_BACKUP_ROOT="$LS_SLSDIR/system-launcher-backup"
LS_LAUNCHER_DIRS=("$STUB_BIN")
LS_STEAM_ROOT="$STUB_ROOT"
stub_before="$(sha256sum "$STUB_BIN/steam" | awk '{print $1}')"
check "unbootstrapped Steam root is rejected" "no" \
  "$(ls_steam_bootstrapped && echo yes || echo no)"
check "unbootstrapped Install Steam stub is ineligible" "no" \
  "$(ls_launcher_eligible "$STUB_BIN/steam" && echo yes || echo no)"
check "unbootstrapped stub is not wrapped" "no" \
  "$(ls_install_shims && echo yes || echo no)"
check "unbootstrapped stub remains byte-identical" "$stub_before" \
  "$(sha256sum "$STUB_BIN/steam" | awk '{print $1}')"
check "unbootstrapped stub remains executable and usable" "INSTALL-STUB" \
  "$(sh "$STUB_BIN/steam" | cut -d' ' -f1)"

# Simulate the package converting the same path into its post-bootstrap
# launcher. A later setup run must now be allowed to wrap it.
printf '#!/bin/sh\nprintf "REAL-LAUNCHER %s\\n" "$*"\n' > "$STUB_BIN/steam"
chmod +x "$STUB_BIN/steam"
printf '#!/bin/sh\nprintf "BOOTSTRAPPED-STEAMSH %s\\n" "$*"\n' > "$STUB_ROOT/steam.sh"
chmod +x "$STUB_ROOT/steam.sh"
mkdir -p "$STUB_HOME/.steam"
ln -s "$STUB_ROOT" "$STUB_HOME/.steam/steam"
check "bootstrapped Steam root is accepted" "yes" \
  "$(ls_steam_bootstrapped && echo yes || echo no)"
check "post-bootstrap launcher becomes eligible" "yes" \
  "$(ls_launcher_eligible "$STUB_BIN/steam" && echo yes || echo no)"
check "post-bootstrap rerun installs the shim" "yes" \
  "$(ls_install_shims && echo yes || echo no)"
check "post-bootstrap shim is installed" "yes" \
  "$(ls_is_our_shim "$STUB_BIN/steam" && echo yes || echo no)"
check "post-bootstrap backup captures the real launcher" "REAL-LAUNCHER" \
  "$(sh "$(ls_backup_path "$STUB_BIN/steam")" | cut -d' ' -f1)"
check "post-bootstrap shim passes real launcher to wrapper" \
  "WRAPPER-STUB bin=$(ls_backup_alias_path "$STUB_BIN/steam") args=-silent" \
  "$(sh "$STUB_BIN/steam" -silent)"

# A backup must never alias the injected wrapper, and generated shims must
# reject an executable directory before trying to exec it as Steam.
WRAPPER_ALIAS_BACKUP="$TMP/wrapper-alias-backup"
ln -s "$STUB_HOME/.local/share/SLSsteam/path/steam" "$WRAPPER_ALIAS_BACKUP"
check "wrapper alias is not a usable launcher backup" "no" \
  "$(ls_backup_is_usable "$WRAPPER_ALIAS_BACKUP" && echo yes || echo no)"
RUNTIME_HOME="$TMP/runtime-home"
RUNTIME_ROOT="$TMP/runtime-root"
RUNTIME_ORIG_DIR="$TMP/runtime-original-dir"
RUNTIME_SHIM="$TMP/runtime-shim"
mkdir -p "$RUNTIME_HOME/.steam" "$RUNTIME_ROOT" "$RUNTIME_ORIG_DIR"
ln -s "$RUNTIME_ROOT" "$RUNTIME_HOME/.steam/steam"
printf '#!/bin/sh\nprintf "RUNTIME-FALLBACK %s\\n" "$*"\n' > "$RUNTIME_ROOT/steam.sh"
chmod 0755 "$RUNTIME_ROOT/steam.sh"
chmod 0755 "$RUNTIME_ORIG_DIR"
ls_shim_content "$RUNTIME_ORIG_DIR" > "$RUNTIME_SHIM"
chmod 0755 "$RUNTIME_SHIM"
check "generated shim rejects executable directory backup" "RUNTIME-FALLBACK" \
  "$(HOME="$RUNTIME_HOME" sh "$RUNTIME_SHIM" | cut -d' ' -f1)"

# The remainder of this fixture covers privilege, bootstrap, and restore paths.# An existing mirrored backup with matching contents but a stale mode must be
# normalized before launcher coverage is reported as successful.
NONEXEC_BACKUP_ROOT="$TMP/nonexec-mirrored-backup"
NONEXEC_BACKUP_BIN="$NONEXEC_BACKUP_ROOT/usr/bin"
mkdir -p "$NONEXEC_BACKUP_BIN"
printf '#!/bin/sh\nprintf "NONEXEC-MIRROR\\n"\n' > "$NONEXEC_BACKUP_BIN/steam"
chmod 0755 "$NONEXEC_BACKUP_BIN/steam"
LS_LAUNCHER_DIRS=("$NONEXEC_BACKUP_BIN")
LS_BACKUP_ROOT="$NONEXEC_BACKUP_ROOT/backup"
LS_STEAM_ROOT="$STUB_ROOT"
NONEXEC_MIRRORED="$(ls_backup_path "$NONEXEC_BACKUP_BIN/steam")"
mkdir -p "$(dirname "$NONEXEC_MIRRORED")"
cp "$NONEXEC_BACKUP_BIN/steam" "$NONEXEC_MIRRORED"
chmod 0644 "$NONEXEC_MIRRORED"
check "non-executable mirrored backup is normalized" "yes" \
  "$(ls_install_shims && [ -x "$NONEXEC_MIRRORED" ] && echo yes || echo no)"
check "normalized mirrored backup preserves its original" "NONEXEC-MIRROR" \
  "$(sh "$NONEXEC_MIRRORED" | cut -d' ' -f1)"

# A shim left by an earlier install must not send an Install Steam stub through
# the wrapper before the canonical Steam symlink has been bootstrapped.
LEGACY_HOME="$TMP/legacy-shim-home"
LEGACY_BIN="$TMP/legacy-shim-usr/games"
LEGACY_BACKUP="$LEGACY_HOME/.local/share/SLSsteam/system-launcher-backup/usr/games/steam.orig"
mkdir -p "$LEGACY_HOME/.local/share/SLSsteam/path" "$LEGACY_BIN" \
  "$(dirname "$LEGACY_BACKUP")"
cat > "$LEGACY_HOME/.local/share/SLSsteam/path/steam" <<'EOF'
#!/bin/sh
printf 'WRAPPER-LEGACY bin=%s args=%s\n' "${SLSM_STEAM_BIN:-none}" "$*"
EOF
chmod +x "$LEGACY_HOME/.local/share/SLSsteam/path/steam"
printf '#!/bin/sh\nprintf "LEGACY-INSTALL-STUB %s\\n" "$*"\n' > "$LEGACY_BACKUP"
chmod +x "$LEGACY_BACKUP"
ls_shim_content "$LEGACY_BACKUP" > "$LEGACY_BIN/steam"
chmod +x "$LEGACY_BIN/steam"
check "existing shim keeps pre-bootstrap stub vanilla" "LEGACY-INSTALL-STUB" \
  "$(HOME="$LEGACY_HOME" sh "$LEGACY_BIN/steam" | cut -d' ' -f1)"
LEGACY_BACKUP_AWAY="$LEGACY_BACKUP.away"
mv "$LEGACY_BACKUP" "$LEGACY_BACKUP_AWAY"
check "pre-bootstrap shim without backup never invokes wrapper" "no" \
  "$(HOME="$LEGACY_HOME" sh "$LEGACY_BIN/steam" 2>&1 | grep -q 'WRAPPER-LEGACY' && echo yes || echo no)"
mv "$LEGACY_BACKUP_AWAY" "$LEGACY_BACKUP"
mkdir -p "$LEGACY_HOME/.steam"
ln -s "$STUB_ROOT" "$LEGACY_HOME/.steam/steam"
check "existing shim uses wrapper after bootstrap" \
  "WRAPPER-LEGACY bin=$(ls_alias_for_backup "$LEGACY_BACKUP") args=-silent" \
  "$(HOME="$LEGACY_HOME" sh "$LEGACY_BIN/steam" -silent)"
check "existing shim heals its missing exec alias in place" "$LEGACY_BACKUP" \
  "$(readlink -f "$(ls_alias_for_backup "$LEGACY_BACKUP")")"

# A managed shim accidentally left in the backup tree is not a runnable
# original, whether the wrapper is available or not.
MANAGED_HOME="$TMP/managed-backup-home"
MANAGED_BIN="$TMP/managed-backup-bin"
MANAGED_BACKUP="$MANAGED_HOME/.local/share/SLSsteam/system-launcher-backup/usr/games/steam.orig"
mkdir -p "$MANAGED_HOME/.local/share/SLSsteam/path" "$MANAGED_BIN" \
  "$(dirname "$MANAGED_BACKUP")" "$MANAGED_HOME/.steam"
cat > "$MANAGED_HOME/.local/share/SLSsteam/path/steam" <<'EOF'
#!/bin/sh
printf 'WRAPPER-MANAGED bin=%s args=%s\n' "${SLSM_STEAM_BIN:-none}" "$*"
EOF
chmod +x "$MANAGED_HOME/.local/share/SLSsteam/path/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nprintf "MANAGED-BACKUP\\n"\n' > "$MANAGED_BACKUP"
chmod +x "$MANAGED_BACKUP"
ln -s "$STUB_ROOT" "$MANAGED_HOME/.steam/steam"
ls_shim_content "$MANAGED_BACKUP" > "$MANAGED_BIN/steam"
chmod +x "$MANAGED_BIN/steam"
check "shim rejects managed backup before wrapper export" \
  "WRAPPER-MANAGED bin=none args=-silent" \
  "$(HOME="$MANAGED_HOME" sh "$MANAGED_BIN/steam" -silent)"
mv "$MANAGED_HOME/.local/share/SLSsteam/path/steam" "$TMP/managed-wrapper-away"
check "shim does not execute managed backup directly" "BOOTSTRAPPED-STEAMSH" \
  "$(HOME="$MANAGED_HOME" sh "$MANAGED_BIN/steam" | cut -d' ' -f1)"

# A bootstrapped but non-executable regular file is not a valid distro launcher
# candidate; setup must not capture or replace it while scanning for shims.
NONEXEC_CAND_ROOT="$TMP/nonexec-candidate-root"
NONEXEC_CAND_BIN="$TMP/nonexec-candidate-bin"
mkdir -p "$NONEXEC_CAND_ROOT" "$NONEXEC_CAND_BIN"
printf '#!/bin/sh\nexit 0\n' > "$NONEXEC_CAND_ROOT/steam.sh"
printf '#!/bin/sh\nprintf "NONEXEC-CANDIDATE\\n"\n' > "$NONEXEC_CAND_BIN/steam"
chmod 0755 "$NONEXEC_CAND_ROOT/steam.sh"
chmod 0644 "$NONEXEC_CAND_BIN/steam"
LS_LAUNCHER_DIRS=("$NONEXEC_CAND_BIN")
LS_BACKUP_ROOT="$NONEXEC_CAND_ROOT/backup"
LS_STEAM_ROOT="$NONEXEC_CAND_ROOT"
nonexec_candidate_before="$(sha256sum "$NONEXEC_CAND_BIN/steam" | awk '{print $1}')"
check "non-executable launcher candidate is rejected" "no" \
  "$(ls_install_shims && echo yes || echo no)"
check "non-executable launcher candidate remains byte-identical" "$nonexec_candidate_before" \
  "$(sha256sum "$NONEXEC_CAND_BIN/steam" | awk '{print $1}')"
check "non-executable launcher candidate is not wrapped" "no" \
  "$(ls_is_our_shim "$NONEXEC_CAND_BIN/steam" && echo yes || echo no)"

# Production resolution must use Valve's canonical symlink. A real directory
# at ~/.steam/steam can contain an executable steam.sh in a partial/foreign
# layout, so it must not qualify unless the path itself is the expected symlink.
REAL_HOME="$TMP/real-home"
mkdir -p "$REAL_HOME/.steam/steam"
printf '#!/bin/sh\nexit 0\n' > "$REAL_HOME/.steam/steam/steam.sh"
chmod +x "$REAL_HOME/.steam/steam/steam.sh"
HOME="$REAL_HOME"
export HOME
unset LS_STEAM_ROOT
check "real Steam directory without symlink is rejected" "no" \
  "$(ls_steam_bootstrapped && echo yes || echo no)"
ln -s "$STUB_ROOT" "$REAL_HOME/.steam/steam-link"
rm -rf "$REAL_HOME/.steam/steam"
mv "$REAL_HOME/.steam/steam-link" "$REAL_HOME/.steam/steam"
check "canonical Steam symlink is accepted" "yes" \
  "$(ls_steam_bootstrapped && echo yes || echo no)"

# An executable directory named steam.sh is not a bootstrapped Steam root.
DIR_ROOT="$TMP/directory-steam-root"
mkdir -p "$DIR_ROOT/steam.sh"
chmod 0755 "$DIR_ROOT/steam.sh"
LS_STEAM_ROOT="$DIR_ROOT"
check "steam.sh directory is not bootstrapped" "no" \
  "$(ls_steam_bootstrapped && echo yes || echo no)"
rmdir "$DIR_ROOT/steam.sh"
LS_STEAM_ROOT="$STUB_ROOT"

# A partial install must not claim launcher coverage: both detected launchers
# need verified shims before setup can persist policy=launcher.
PARTIAL_ROOT="$TMP/partial"
mkdir -p "$PARTIAL_ROOT/good" "$PARTIAL_ROOT/bad"
printf '#!/bin/sh\nprintf GOOD\n' > "$PARTIAL_ROOT/good/steam"
printf '#!/bin/sh\nprintf BAD\n' > "$PARTIAL_ROOT/bad/steam"
chmod 0755 "$PARTIAL_ROOT/good/steam" "$PARTIAL_ROOT/bad/steam"
chmod 0555 "$PARTIAL_ROOT/bad/steam"
LS_LAUNCHER_DIRS=("$PARTIAL_ROOT/good" "$PARTIAL_ROOT/bad")
LS_BACKUP_ROOT="$PARTIAL_ROOT/backup"
LS_SUDO=""
check "partial launcher install reports failure" "no" \
  "$(ls_install_shims && echo yes || echo no)"
check "partial install keeps the good shim" "yes" \
  "$(ls_is_our_shim "$PARTIAL_ROOT/good/steam" && echo yes || echo no)"
check "partial install leaves the failed launcher vanilla" "no" \
  "$(ls_is_our_shim "$PARTIAL_ROOT/bad/steam" && echo yes || echo no)"

# A captured backup must never itself be another managed shim. Treating it as a
# valid original would let the launcher shim recurse through the wrapper and
# incorrectly report a successful system-launcher installation.
BAD_BACKUP_ROOT="$TMP/tagged-backup"
BAD_STEAM_ROOT="$BAD_BACKUP_ROOT/steam-root"
mkdir -p "$BAD_BACKUP_ROOT/usr/bin" "$BAD_BACKUP_ROOT/backup/usr/bin" "$BAD_STEAM_ROOT"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec /broken/tagged\n' \
  > "$BAD_BACKUP_ROOT/usr/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec /broken/original\n' \
  > "$BAD_BACKUP_ROOT/backup/usr/bin/steam.orig"
printf '#!/bin/sh\nexit 0\n' > "$BAD_STEAM_ROOT/steam.sh"
chmod 0755 "$BAD_BACKUP_ROOT/usr/bin/steam" \
  "$BAD_BACKUP_ROOT/backup/usr/bin/steam.orig" "$BAD_STEAM_ROOT/steam.sh"
LS_LAUNCHER_DIRS=("$BAD_BACKUP_ROOT/usr/bin")
LS_BACKUP_ROOT="$BAD_BACKUP_ROOT/backup"
LS_STEAM_ROOT="$BAD_STEAM_ROOT"
check "tagged mirrored backup rejects reinstall" "no" \
  "$(ls_install_shims && echo yes || echo no)"
check "tagged mirrored backup leaves shim in place" "yes" \
  "$(ls_is_our_shim "$BAD_BACKUP_ROOT/usr/bin/steam" && echo yes || echo no)"

# A legacy flat steam.orig is ambiguous once an existing shim already has its
# own mirrored original. It must not be assigned to a different vanilla path.
STALE_ROOT="$TMP/stale-flat"
STALE_STEAM_ROOT="$STALE_ROOT/steam-root"
mkdir -p "$STALE_ROOT/usr/bin" "$STALE_ROOT/usr/games" \
  "$STALE_ROOT/backup/${STALE_ROOT#/}/usr/games" "$STALE_STEAM_ROOT"
printf '#!/bin/sh\nprintf "FRESH-BIN\\n"\n' > "$STALE_ROOT/usr/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/stale-games\n' \
  "$STALE_ROOT/backup/${STALE_ROOT#/}/usr/games/steam.orig" \
  > "$STALE_ROOT/usr/games/steam"
printf '#!/bin/sh\nprintf "MIRRORED-GAMES\\n"\n' > \
  "$STALE_ROOT/backup/${STALE_ROOT#/}/usr/games/steam.orig"
printf '#!/bin/sh\nprintf "STALE-FLAT\\n"\n' > "$STALE_ROOT/backup/steam.orig"
printf '#!/bin/sh\nexit 0\n' > "$STALE_STEAM_ROOT/steam.sh"
chmod 0755 "$STALE_ROOT/usr/bin/steam" "$STALE_ROOT/usr/games/steam" \
  "$STALE_ROOT/backup/${STALE_ROOT#/}/usr/games/steam.orig" \
  "$STALE_ROOT/backup/steam.orig" "$STALE_STEAM_ROOT/steam.sh"
LS_LAUNCHER_DIRS=("$STALE_ROOT/usr/bin" "$STALE_ROOT/usr/games")
LS_BACKUP_ROOT="$STALE_ROOT/backup"
LS_STEAM_ROOT="$STALE_STEAM_ROOT"
check "stale flat backup is not reassigned to vanilla launcher" "FRESH-BIN" \
  "$(ls_install_shims && sh "$(ls_backup_path "$STALE_ROOT/usr/bin/steam")" | cut -d' ' -f1)"
check "stale flat backup remains available" "yes" \
  "$([ -f "$STALE_ROOT/backup/steam.orig" ] && echo yes || echo no)"

# A legacy flat steam.orig cannot safely restore two same-named launchers:
# retain both shims rather than applying one original to both paths.
FLAT_ROOT="$TMP/flat-ambiguous"
mkdir -p "$FLAT_ROOT/bin" "$FLAT_ROOT/games" "$FLAT_ROOT/backup"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec /broken/bin\n' > "$FLAT_ROOT/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec /broken/games\n' > "$FLAT_ROOT/games/steam"
printf '#!/bin/sh\nprintf FLAT-ORIGINAL\n' > "$FLAT_ROOT/backup/steam.orig"
chmod 0755 "$FLAT_ROOT/bin/steam" "$FLAT_ROOT/games/steam" "$FLAT_ROOT/backup/steam.orig"
LS_LAUNCHER_DIRS=("$FLAT_ROOT/bin" "$FLAT_ROOT/games")
LS_BACKUP_ROOT="$FLAT_ROOT/backup"
check "ambiguous flat restore reports failure" "no" \
  "$(ls_restore_shims && echo yes || echo no)"
check "ambiguous flat bin shim is retained" "yes" \
  "$(ls_is_our_shim "$FLAT_ROOT/bin/steam" && echo yes || echo no)"
check "ambiguous flat games shim is retained" "yes" \
  "$(ls_is_our_shim "$FLAT_ROOT/games/steam" && echo yes || echo no)"

# A legacy flat backup is unambiguous when it belongs to the one existing shim,
# even if a second launcher path was replaced by the package before reinstall.
MIG_ROOT="$TMP/flat-reinstall"
MIG_STEAM_ROOT="$MIG_ROOT/steam-root"
mkdir -p "$MIG_ROOT/usr/bin" "$MIG_ROOT/usr/games" "$MIG_ROOT/backup" "$MIG_STEAM_ROOT"
printf '#!/bin/sh\nprintf "FRESH-BIN\\n"\n' > "$MIG_ROOT/usr/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec "%s/steam.orig" "$@"\n' \
  "$MIG_ROOT/backup" > "$MIG_ROOT/usr/games/steam"
printf '#!/bin/sh\nprintf "LEGACY-GAMES\\n"\n' > "$MIG_ROOT/backup/steam.orig"
printf '#!/bin/sh\nexit 0\n' > "$MIG_STEAM_ROOT/steam.sh"
chmod 0755 "$MIG_ROOT/usr/bin/steam" "$MIG_ROOT/usr/games/steam" \
  "$MIG_ROOT/backup/steam.orig" "$MIG_STEAM_ROOT/steam.sh"
LS_LAUNCHER_DIRS=("$MIG_ROOT/usr/bin" "$MIG_ROOT/usr/games")
LS_BACKUP_ROOT="$MIG_ROOT/backup"
LS_STEAM_ROOT="$MIG_STEAM_ROOT"
check "reinstall adopts the one shim's legacy backup" "yes" \
  "$(ls_install_shims && echo yes || echo no)"
check "legacy backup moves to the shim's mirrored path" "LEGACY-GAMES" \
  "$(sh "$(ls_backup_path "$MIG_ROOT/usr/games/steam")" | cut -d' ' -f1)"
check "reinstall captures the replaced launcher separately" "FRESH-BIN" \
  "$(sh "$(ls_backup_path "$MIG_ROOT/usr/bin/steam")" | cut -d' ' -f1)"

# Reinstall must also reconcile one mirrored shim and one legacy shim when the
# flat backup is explicitly referenced by only the legacy path.
MIX_REINSTALL_ROOT="$TMP/mixed-reinstall"
MIX_REINSTALL_STEAM_ROOT="$MIX_REINSTALL_ROOT/steam-root"
MIX_REINSTALL_BIN="$MIX_REINSTALL_ROOT/usr/bin"
MIX_REINSTALL_GAMES="$MIX_REINSTALL_ROOT/usr/games"
MIX_REINSTALL_BACKUP="$MIX_REINSTALL_ROOT/backup"
mkdir -p "$MIX_REINSTALL_BIN" "$MIX_REINSTALL_GAMES" "$MIX_REINSTALL_STEAM_ROOT"
LS_LAUNCHER_DIRS=("$MIX_REINSTALL_BIN" "$MIX_REINSTALL_GAMES")
LS_BACKUP_ROOT="$MIX_REINSTALL_BACKUP"
LS_STEAM_ROOT="$MIX_REINSTALL_STEAM_ROOT"
MIX_REINSTALL_MIRRORED="$(ls_backup_path "$MIX_REINSTALL_BIN/steam")"
MIX_REINSTALL_FLAT="$MIX_REINSTALL_BACKUP/steam.orig"
mkdir -p "$(dirname "$MIX_REINSTALL_MIRRORED")"
printf '#!/bin/sh\nprintf "MIX-REINSTALL-BIN\\n"\n' > "$MIX_REINSTALL_MIRRORED"
printf '#!/bin/sh\nprintf "MIX-REINSTALL-GAMES\\n"\n' > "$MIX_REINSTALL_FLAT"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/mixed-bin\n' \
  "$MIX_REINSTALL_MIRRORED" > "$MIX_REINSTALL_BIN/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/mixed-games\n' \
  "$MIX_REINSTALL_FLAT" > "$MIX_REINSTALL_GAMES/steam"
printf '#!/bin/sh\nexit 0\n' > "$MIX_REINSTALL_STEAM_ROOT/steam.sh"
chmod 0755 "$MIX_REINSTALL_BIN/steam" "$MIX_REINSTALL_GAMES/steam" \
  "$MIX_REINSTALL_MIRRORED" "$MIX_REINSTALL_FLAT" "$MIX_REINSTALL_STEAM_ROOT/steam.sh"
check "mixed mirrored and legacy reinstall succeeds" "yes" \
  "$(ls_install_shims && echo yes || echo no)"
check "mixed reinstall preserves mirrored original" "MIX-REINSTALL-BIN" \
  "$(sh "$MIX_REINSTALL_MIRRORED" | cut -d' ' -f1)"
check "mixed reinstall migrates legacy original" "MIX-REINSTALL-GAMES" \
  "$(sh "$(ls_backup_path "$MIX_REINSTALL_GAMES/steam")" | cut -d' ' -f1)"
check "mixed reinstall consumes legacy flat backup" "no" \
  "$([ -e "$MIX_REINSTALL_FLAT" ] && echo yes || echo no)"

# A launcher only root can write keeps its captured original in root's own tree,
# so the privilege command never works below the user's home, where the user
# could swap any folder for a link. A mirror an older install left in the home
# is not touched.
PRIV_ROOT="$TMP/privileged-backup"
PRIV_BIN="$PRIV_ROOT/usr/bin"
PRIV_BACKUP_ROOT="$PRIV_ROOT/backup"
PRIV_SYSTEM_ROOT="$PRIV_ROOT/var-lib/launcher-backup"
PRIV_MIRRORED="$PRIV_BACKUP_ROOT/${PRIV_BIN#/}/steam.orig"
PRIV_SYSTEM_MIRRORED="$PRIV_SYSTEM_ROOT/${PRIV_BIN#/}/steam.orig"
PRIV_LOG="$PRIV_ROOT/privilege.log"
mkdir -p "$PRIV_BIN" "$(dirname "$PRIV_MIRRORED")" "$PRIV_ROOT/steam-root"
cat > "$PRIV_ROOT/fake-sudo" <<'EOF'
#!/bin/sh
printf '%s\n' "$1" >> "$SLS_PRIV_LOG"
# One line per call, also for an `sh -c` script.
printf '%s\n' "$(printf '%s' "$*" | tr '\n' ' ')" >> "$SLS_PRIV_LOG.argv"
cmd="$1"
shift
"$cmd" "$@"
EOF
chmod +x "$PRIV_ROOT/fake-sudo"
printf '#!/bin/sh\nprintf "PRIV-NEW\\n"\n' > "$PRIV_BIN/steam"
printf '#!/bin/sh\nprintf "PRIV-OLD\\n"\n' > "$PRIV_MIRRORED"
printf '#!/bin/sh\nexit 0\n' > "$PRIV_ROOT/steam-root/steam.sh"
chmod 0755 "$PRIV_ROOT/steam-root/steam.sh"
chmod 0555 "$PRIV_BIN/steam"
chmod 0644 "$PRIV_MIRRORED"
LS_LAUNCHER_DIRS=("$PRIV_BIN")
LS_BACKUP_ROOT="$PRIV_BACKUP_ROOT"
LS_SYSTEM_BACKUP_ROOT="$PRIV_SYSTEM_ROOT"
LS_STEAM_ROOT="$PRIV_ROOT/steam-root"
LS_SUDO="$PRIV_ROOT/fake-sudo"
export SLS_PRIV_LOG="$PRIV_LOG"
check "a launcher only root can write is wrapped" "yes" \
  "$(ls_install_shims && ls_is_our_shim "$PRIV_BIN/steam" && echo yes || echo no)"
check "its captured original is in root's tree and the shim runs it from there" "PRIV-NEW:yes" \
  "$(sh "$PRIV_SYSTEM_MIRRORED" | cut -d' ' -f1):$(ls_shim_references_backup "$PRIV_BIN/steam" "$PRIV_SYSTEM_MIRRORED" && echo yes || echo no)"
check "the privilege command names nothing below the user's backup folder" "" \
  "$(grep -F "$PRIV_BACKUP_ROOT" "$PRIV_LOG.argv" || true)"
check "the older home mirror is left as it was" "PRIV-OLD" \
  "$(sh "$PRIV_MIRRORED" | cut -d' ' -f1)"
# A launcher the user cannot write is replaced through the privilege command,
# which takes the new content on its stdin: the temp file and the captured
# original sit in user folders, where the user could swap a file for a link.
RO_ROOT="$TMP/readonly-launcher"
RO_BIN="$RO_ROOT/usr/bin"
mkdir -p "$RO_BIN" "$RO_ROOT/steam-root" "$RO_ROOT/tmp"
printf '#!/bin/sh\nprintf "RO-ORIG\\n"\n' > "$RO_BIN/steam"
printf '#!/bin/sh\nexit 0\n' > "$RO_ROOT/steam-root/steam.sh"
chmod 0555 "$RO_BIN/steam"
chmod 0755 "$RO_ROOT/steam-root/steam.sh"
LS_LAUNCHER_DIRS=("$RO_BIN")
LS_BACKUP_ROOT="$RO_ROOT/backup"
LS_SYSTEM_BACKUP_ROOT="$RO_ROOT/var-lib/launcher-backup"
RO_SYSTEM_MIRRORED="$LS_SYSTEM_BACKUP_ROOT/${RO_BIN#/}/steam.orig"
LS_STEAM_ROOT="$RO_ROOT/steam-root"
export SLS_PRIV_LOG="$RO_ROOT/privilege.log"
check "an unwritable launcher is wrapped through the privilege command" "yes" \
  "$(TMPDIR="$RO_ROOT/tmp" ls_install_shims && ls_is_our_shim "$RO_BIN/steam" && echo yes || echo no)"
check "its captured original is in root's tree" "yes" \
  "$([ -f "$RO_SYSTEM_MIRRORED" ] && echo yes || echo no)"
chmod 0555 "$RO_BIN/steam"
check "an unwritable launcher is restored through the privilege command" "RO-ORIG" \
  "$(TMPDIR="$RO_ROOT/tmp" ls_restore_shims && sh "$RO_BIN/steam")"
check "both launcher writes went through the privilege command" "2" \
  "$(grep -c '^install ' "$SLS_PRIV_LOG.argv")"
check "the privilege command reads launcher content only from stdin" "" \
  "$(grep '^install ' "$SLS_PRIV_LOG.argv" | grep -v '^install -m 0755 /dev/stdin ' || true)"
check "a restore from root's tree removes that copy and its folders" "no:no" \
  "$([ -e "$RO_SYSTEM_MIRRORED" ] && echo yes || echo no):$([ -e "$RO_ROOT/var-lib" ] && echo yes || echo no)"

# An existing shim that runs a copy an older install kept below the home moves
# it to root's tree, but only a copy root wrote: the opened file must be a
# root-owned regular file nobody else can write. A stat stub reports root as
# the owner of that old copy and its folders (the test cannot create root
# files); the old copy and its alias are then removed, the home backup folder
# itself stays.
MIG_ROOT="$TMP/migrate-home-copy"
MIG_BIN="$MIG_ROOT/usr/bin"
MIG_HOME_ROOT="$MIG_ROOT/home-backup"
MIG_SYSTEM_ROOT="$MIG_ROOT/var-lib/launcher-backup"
MIG_OLD="$MIG_HOME_ROOT/${MIG_BIN#/}/steam.orig"
MIG_NEW="$MIG_SYSTEM_ROOT/${MIG_BIN#/}/steam.orig"
mkdir -p "$MIG_BIN" "$(dirname "$MIG_OLD")" "$MIG_ROOT/steam-root"
printf '#!/bin/sh\nprintf "MIG-ORIG\\n"\n' > "$MIG_OLD"
printf '#!/bin/sh\nexit 0\n' > "$MIG_ROOT/steam-root/steam.sh"
chmod 0755 "$MIG_OLD" "$MIG_ROOT/steam-root/steam.sh"
ln -s steam.orig "$(dirname "$MIG_OLD")/steam"
ls_shim_content "$MIG_OLD" > "$MIG_BIN/steam"
chmod 0555 "$MIG_BIN/steam"
LS_LAUNCHER_DIRS=("$MIG_BIN")
LS_BACKUP_ROOT="$MIG_HOME_ROOT"
LS_SYSTEM_BACKUP_ROOT="$MIG_SYSTEM_ROOT"
LS_STEAM_ROOT="$MIG_ROOT/steam-root"
LS_SUDO="$PRIV_ROOT/fake-sudo"
export SLS_PRIV_LOG="$MIG_ROOT/privilege.log"
check "a home copy root did not write is not moved" "no:yes" \
  "$(ls_install_shims && echo yes || echo no):$(ls_shim_references_backup "$MIG_BIN/steam" "$MIG_OLD" && echo yes || echo no)"
mkdir -p "$MIG_ROOT/stat-root"
printf '#!/bin/sh\nlast=""\nfor a in "$@"; do last="$a"; done\nreal="$(readlink -f -- "$last" 2>/dev/null)"\ncase "$real:$*" in\n\t%s/*%%u*|%s:*%%u*) echo 0; exit 0 ;;\nesac\nexec "%s" "$@"\n' \
  "$MIG_HOME_ROOT/${MIG_BIN#/}" "$MIG_HOME_ROOT/${MIG_BIN#/}" "$(command -v stat)" > "$MIG_ROOT/stat-root/stat"
chmod +x "$MIG_ROOT/stat-root/stat"
check "a home copy root wrote moves to root's tree" "yes:MIG-ORIG" \
  "$(PATH="$MIG_ROOT/stat-root:$PATH" ls_install_shims && ls_shim_references_backup "$MIG_BIN/steam" "$MIG_NEW" && echo yes || echo no):$(sh "$MIG_NEW" 2>/dev/null)"
check "the old home copy and its alias are removed" "no:no" \
  "$([ -e "$MIG_OLD" ] && echo yes || echo no):$([ -L "$(dirname "$MIG_OLD")/steam" ] && echo yes || echo no)"
check "the user's backup folder itself stays" "yes" "$([ -d "$MIG_HOME_ROOT" ] && echo yes || echo no)"
check "the privilege command named no path below the user's backup folder but the checked old copy" "" \
  "$(grep -F "$MIG_HOME_ROOT" "$SLS_PRIV_LOG.argv" | grep -v '^sh -c ' || true)"
LS_SUDO=""
unset SLS_PRIV_LOG

# Restoration must retry a flat legacy candidate after mirrored shims have been
# restored, rather than depending on launcher enumeration order.
MIX_ROOT="$TMP/mixed-restore"
mkdir -p "$MIX_ROOT/usr/bin" "$MIX_ROOT/usr/games" \
  "$MIX_ROOT/backup" "$MIX_ROOT/backup/${MIX_ROOT#/}/usr/games"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s/steam.orig"\nexec /broken/bin\n' \
  "$MIX_ROOT/backup" > "$MIX_ROOT/usr/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s%s/usr/games/steam.orig"\nexec /broken/games\n' \
  "$MIX_ROOT/backup" "$MIX_ROOT" > "$MIX_ROOT/usr/games/steam"
printf '#!/bin/sh\nprintf "MIXED-BIN\\n"\n' > "$MIX_ROOT/backup/steam.orig"
printf '#!/bin/sh\nprintf "MIXED-GAMES\\n"\n' > \
  "$MIX_ROOT/backup/${MIX_ROOT#/}/usr/games/steam.orig"
chmod 0755 "$MIX_ROOT/usr/bin/steam" "$MIX_ROOT/usr/games/steam" \
  "$MIX_ROOT/backup/steam.orig" \
  "$MIX_ROOT/backup/${MIX_ROOT#/}/usr/games/steam.orig"
LS_LAUNCHER_DIRS=("$MIX_ROOT/usr/bin" "$MIX_ROOT/usr/games")
LS_BACKUP_ROOT="$MIX_ROOT/backup"
check "mixed mirrored and legacy restore succeeds" "yes" \
  "$(ls_restore_shims && echo yes || echo no)"
check "mixed restore recovers the flat-backed launcher" "MIXED-BIN" \
  "$(sh "$MIX_ROOT/usr/bin/steam" | cut -d' ' -f1)"
check "mixed restore recovers the mirrored launcher" "MIXED-GAMES" \
  "$(sh "$MIX_ROOT/usr/games/steam" | cut -d' ' -f1)"

# A safe legacy owner must still be restored when a separate shim has no
# recoverable backup; the overall restore remains retryable for that peer.
MIX_UNRES_ROOT="$TMP/mixed-unresolved-restore"
mkdir -p "$MIX_UNRES_ROOT/usr/bin" "$MIX_UNRES_ROOT/usr/games" \
  "$MIX_UNRES_ROOT/backup"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s/steam.orig"\nexec /broken/unresolved-bin\n' \
  "$MIX_UNRES_ROOT/backup" > "$MIX_UNRES_ROOT/usr/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s%s/usr/games/steam.orig"\nexec /broken/unresolved-games\n' \
  "$MIX_UNRES_ROOT/backup" "$MIX_UNRES_ROOT" > "$MIX_UNRES_ROOT/usr/games/steam"
printf '#!/bin/sh\nprintf "MIX-UNRES-BIN\\n"\n' > "$MIX_UNRES_ROOT/backup/steam.orig"
chmod 0755 "$MIX_UNRES_ROOT/usr/bin/steam" \
  "$MIX_UNRES_ROOT/usr/games/steam" "$MIX_UNRES_ROOT/backup/steam.orig"
LS_LAUNCHER_DIRS=("$MIX_UNRES_ROOT/usr/bin" "$MIX_UNRES_ROOT/usr/games")
LS_BACKUP_ROOT="$MIX_UNRES_ROOT/backup"
check "mixed restore reports unresolved peer" "no" \
  "$(ls_restore_shims && echo yes || echo no)"
check "mixed restore recovers exact legacy owner" "MIX-UNRES-BIN" \
  "$(sh "$MIX_UNRES_ROOT/usr/bin/steam" | cut -d' ' -f1)"
check "mixed restore retains unresolved peer shim" "yes" \
  "$(ls_is_our_shim "$MIX_UNRES_ROOT/usr/games/steam" && echo yes || echo no)"

# Reverse enumeration must not let an unresolved shim steal a flat backup that
# belongs to a later shim during reinstall or restoration.
REVERSE_INSTALL_ROOT="$TMP/reverse-mixed-install"
REVERSE_INSTALL_STEAM_ROOT="$REVERSE_INSTALL_ROOT/steam-root"
REVERSE_INSTALL_UNRES="$REVERSE_INSTALL_ROOT/usr/bin"
REVERSE_INSTALL_OWNER="$REVERSE_INSTALL_ROOT/usr/games"
REVERSE_INSTALL_BACKUP="$REVERSE_INSTALL_ROOT/backup"
mkdir -p "$REVERSE_INSTALL_UNRES" "$REVERSE_INSTALL_OWNER" "$REVERSE_INSTALL_STEAM_ROOT"
LS_LAUNCHER_DIRS=("$REVERSE_INSTALL_UNRES" "$REVERSE_INSTALL_OWNER")
LS_BACKUP_ROOT="$REVERSE_INSTALL_BACKUP"
LS_STEAM_ROOT="$REVERSE_INSTALL_STEAM_ROOT"
REVERSE_INSTALL_UNRES_MIRRORED="$(ls_backup_path "$REVERSE_INSTALL_UNRES/steam")"
REVERSE_INSTALL_FLAT="$REVERSE_INSTALL_BACKUP/steam.orig"
mkdir -p "$(dirname "$REVERSE_INSTALL_UNRES_MIRRORED")"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/reverse-unresolved\n' \
  "$REVERSE_INSTALL_UNRES_MIRRORED" > "$REVERSE_INSTALL_UNRES/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/reverse-owner\n' \
  "$REVERSE_INSTALL_FLAT" > "$REVERSE_INSTALL_OWNER/steam"
printf '#!/bin/sh\nprintf "REVERSE-INSTALL-OWNER\\n"\n' > "$REVERSE_INSTALL_FLAT"
printf '#!/bin/sh\nexit 0\n' > "$REVERSE_INSTALL_STEAM_ROOT/steam.sh"
chmod 0755 "$REVERSE_INSTALL_UNRES/steam" "$REVERSE_INSTALL_OWNER/steam" \
  "$REVERSE_INSTALL_FLAT" "$REVERSE_INSTALL_STEAM_ROOT/steam.sh"
check "reverse mixed reinstall remains retryable" "no" \
  "$(ls_install_shims && echo yes || echo no)"
check "reverse reinstall does not assign flat backup to peer" "no" \
  "$([ -e "$REVERSE_INSTALL_UNRES_MIRRORED" ] && echo yes || echo no)"
check "reverse reinstall restores the later exact owner" "REVERSE-INSTALL-OWNER" \
  "$(sh "$(ls_backup_path "$REVERSE_INSTALL_OWNER/steam")" | cut -d' ' -f1)"

REVERSE_RESTORE_ROOT="$TMP/reverse-mixed-restore"
REVERSE_RESTORE_UNRES="$REVERSE_RESTORE_ROOT/usr/bin"
REVERSE_RESTORE_OWNER="$REVERSE_RESTORE_ROOT/usr/games"
REVERSE_RESTORE_BACKUP="$REVERSE_RESTORE_ROOT/backup"
mkdir -p "$REVERSE_RESTORE_UNRES" "$REVERSE_RESTORE_OWNER" "$REVERSE_RESTORE_BACKUP"
LS_LAUNCHER_DIRS=("$REVERSE_RESTORE_UNRES" "$REVERSE_RESTORE_OWNER")
LS_BACKUP_ROOT="$REVERSE_RESTORE_BACKUP"
REVERSE_RESTORE_UNRES_MIRRORED="$(ls_backup_path "$REVERSE_RESTORE_UNRES/steam")"
REVERSE_RESTORE_FLAT="$REVERSE_RESTORE_BACKUP/steam.orig"
mkdir -p "$(dirname "$REVERSE_RESTORE_UNRES_MIRRORED")"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/reverse-restore-unresolved\n' \
  "$REVERSE_RESTORE_UNRES_MIRRORED" > "$REVERSE_RESTORE_UNRES/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/reverse-restore-owner\n' \
  "$REVERSE_RESTORE_FLAT" > "$REVERSE_RESTORE_OWNER/steam"
printf '#!/bin/sh\nprintf "REVERSE-RESTORE-OWNER\\n"\n' > "$REVERSE_RESTORE_FLAT"
chmod 0755 "$REVERSE_RESTORE_UNRES/steam" "$REVERSE_RESTORE_OWNER/steam" \
  "$REVERSE_RESTORE_FLAT"
check "reverse mixed restore remains retryable" "no" \
  "$(ls_restore_shims && echo yes || echo no)"
check "reverse restore retains unresolved first shim" "yes" \
  "$(ls_is_our_shim "$REVERSE_RESTORE_UNRES/steam" && echo yes || echo no)"
check "reverse restore recovers the later exact owner" "REVERSE-RESTORE-OWNER" \
  "$(sh "$REVERSE_RESTORE_OWNER/steam" | cut -d' ' -f1)"

# A managed mirrored backup must be rejected before any copy; a valid flat
# backup explicitly referenced by the shim remains a safe fallback.
INVALID_ROOT="$TMP/invalid-mirrored-restore"
mkdir -p "$INVALID_ROOT/usr/bin" "$INVALID_ROOT/backup" \
  "$INVALID_ROOT/backup/${INVALID_ROOT#/}/usr/bin"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s/steam.orig"\nexec /broken/invalid-mirror\n' \
  "$INVALID_ROOT/backup" > "$INVALID_ROOT/usr/bin/steam"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nexec /broken/captured\n' \
  > "$INVALID_ROOT/backup/${INVALID_ROOT#/}/usr/bin/steam.orig"
printf '#!/bin/sh\nprintf "INVALID-FLAT\\n"\n' > "$INVALID_ROOT/backup/steam.orig"
chmod 0755 "$INVALID_ROOT/usr/bin/steam" \
  "$INVALID_ROOT/backup/${INVALID_ROOT#/}/usr/bin/steam.orig" \
  "$INVALID_ROOT/backup/steam.orig"
LS_LAUNCHER_DIRS=("$INVALID_ROOT/usr/bin")
LS_BACKUP_ROOT="$INVALID_ROOT/backup"
check "managed mirrored backup is not installed" "INVALID-FLAT" \
  "$(ls_restore_shims && sh "$INVALID_ROOT/usr/bin/steam" | head -1)"
check "managed mirrored backup is not left live" "no" \
  "$(ls_is_our_shim "$INVALID_ROOT/usr/bin/steam" && echo yes || echo no)"

# An existing shim may only be refreshed from the backup path it explicitly
# references. An executable mirrored file with the same basename is not enough.
ASSOCIATION_ROOT="$TMP/association-mirror"
ASSOCIATION_BACKUP="$ASSOCIATION_ROOT/backup"
ASSOCIATION_LAUNCHER="$ASSOCIATION_ROOT/usr/bin/steam"
ASSOCIATION_MIRROR="$ASSOCIATION_BACKUP/${ASSOCIATION_LAUNCHER#/}.orig"
mkdir -p "$(dirname "$ASSOCIATION_LAUNCHER")" \
  "$(dirname "$ASSOCIATION_MIRROR")" "$ASSOCIATION_ROOT/steam-root"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s/steam.orig"\nexec /broken/association\n' \
  "$ASSOCIATION_BACKUP" > "$ASSOCIATION_LAUNCHER"
printf '#!/bin/sh\nprintf "STALE-ASSOCIATION-MIRROR\\n"\n' > "$ASSOCIATION_MIRROR"
printf '#!/bin/sh\nexit 0\n' > "$ASSOCIATION_ROOT/steam-root/steam.sh"
chmod 0755 "$ASSOCIATION_LAUNCHER" "$ASSOCIATION_MIRROR" \
  "$ASSOCIATION_ROOT/steam-root/steam.sh"
LS_LAUNCHER_DIRS=("$(dirname "$ASSOCIATION_LAUNCHER")")
LS_BACKUP_ROOT="$ASSOCIATION_BACKUP"
LS_STEAM_ROOT="$ASSOCIATION_ROOT/steam-root"
check "unassociated mirrored backup rejects existing shim" "no" \
  "$(if ls_install_shims; then echo yes; else echo no; fi)"
check "unassociated mirrored backup does not rewrite shim" "yes" \
  "$(grep -Fqx -- "SLSM_ORIG=\"$ASSOCIATION_BACKUP/steam.orig\"" \
      "$ASSOCIATION_LAUNCHER" && echo yes || echo no)"
check "unassociated mirrored backup remains stale" "STALE-ASSOCIATION-MIRROR" \
  "$(sh "$ASSOCIATION_MIRROR" | cut -d' ' -f1)"

# Even an exactly referenced legacy backup must not overwrite an existing
# mirrored candidate whose association is unknown. Keep both files and the
# current shim unchanged so a later run can resolve the conflict safely.
CONFLICT_ROOT="$TMP/association-conflict"
CONFLICT_BACKUP="$CONFLICT_ROOT/backup"
CONFLICT_LAUNCHER="$CONFLICT_ROOT/usr/bin/steam"
CONFLICT_MIRROR="$CONFLICT_BACKUP/${CONFLICT_LAUNCHER#/}.orig"
CONFLICT_LEGACY="$CONFLICT_BACKUP/steam.orig"
mkdir -p "$(dirname "$CONFLICT_LAUNCHER")" \
  "$(dirname "$CONFLICT_MIRROR")" "$CONFLICT_ROOT/steam-root"
printf '#!/bin/sh\n# slsteam-moon system launcher shim\nSLSM_ORIG="%s"\nexec /broken/association-conflict\n' \
  "$CONFLICT_LEGACY" > "$CONFLICT_LAUNCHER"
printf '#!/bin/sh\nprintf "LEGACY-ASSOCIATION-CONFLICT\\n"\n' > "$CONFLICT_LEGACY"
printf '#!/bin/sh\nprintf "STALE-ASSOCIATION-CONFLICT\\n"\n' > "$CONFLICT_MIRROR"
printf '#!/bin/sh\nexit 0\n' > "$CONFLICT_ROOT/steam-root/steam.sh"
chmod 0755 "$CONFLICT_LAUNCHER" "$CONFLICT_LEGACY" "$CONFLICT_MIRROR" \
  "$CONFLICT_ROOT/steam-root/steam.sh"
LS_LAUNCHER_DIRS=("$(dirname "$CONFLICT_LAUNCHER")")
LS_BACKUP_ROOT="$CONFLICT_BACKUP"
LS_STEAM_ROOT="$CONFLICT_ROOT/steam-root"
check "legacy migration rejects stale mirrored conflict" "no" \
  "$(if ls_install_shims; then echo yes; else echo no; fi)"
check "legacy conflict leaves shim associated with legacy" "yes" \
  "$(grep -Fqx -- "SLSM_ORIG=\"$CONFLICT_LEGACY\"" \
      "$CONFLICT_LAUNCHER" && echo yes || echo no)"
check "legacy conflict leaves mirror unchanged" "STALE-ASSOCIATION-CONFLICT" \
  "$(sh "$CONFLICT_MIRROR" | cut -d' ' -f1)"
check "legacy conflict retains legacy backup" "yes" \
  "$([ -f "$CONFLICT_LEGACY" ] && echo yes || echo no)"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
