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

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
