#!/usr/bin/env bash
# Static checks that setup.sh installs the desktop-coverage helper into $SLSDIR
# and that package.sh ships both files in the release zip.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
ck(){ if [ "$2" = "yes" ]; then echo "ok   - $1"; else echo "FAIL - $1"; fail=1; fi; }

ck "package.sh ships the lib" \
   "$(grep -q 'desktop-coverage.lib.sh' "$HERE/scripts/package.sh" && echo yes || echo no)"
ck "package.sh ships the CLI" \
   "$(grep -q 'ensure-desktop-coverage.sh' "$HERE/scripts/package.sh" && echo yes || echo no)"
ck "setup.sh installs the CLI into SLSDIR" \
   "$(grep -q 'ensure-desktop-coverage.sh' "$HERE/setup.sh" && echo yes || echo no)"
ck "setup.sh installs the lib into SLSDIR" \
   "$(grep -q 'desktop-coverage.lib.sh' "$HERE/setup.sh" && echo yes || echo no)"

ck "package.sh ships the guardian unit lib" \
   "$(grep -q 'desktop-guardian-units.lib.sh' "$HERE/scripts/package.sh" && echo yes || echo no)"
ck "setup.sh installs the guardian unit lib into SLSDIR" \
   "$(grep -q 'desktop-guardian-units.lib.sh' "$HERE/setup.sh" && echo yes || echo no)"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
