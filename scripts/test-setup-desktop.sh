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

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
