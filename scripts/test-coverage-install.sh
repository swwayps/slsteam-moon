#!/usr/bin/env bash
# setup.sh's install step puts the desktop-coverage helpers next to the wrapper.
# That package.sh ships them is checked on the real zip by test-package.sh.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
check() { # desc expected actual
  if [ "$2" = "$3" ]; then printf 'ok   - %s\n' "$1"
  else printf 'FAIL - %s (want=[%s] got=[%s])\n' "$1" "$2" "$3"; fail=1; fi
}
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# A release tree with the files install_slssteam copies.
REL="$TMP/release"
mkdir -p "$REL/bin" "$REL/tools" "$TMP/home"
printf 'stub' > "$REL/bin/SLSsteam.so"
cp "$HERE/tools/desktop-coverage.lib.sh" "$HERE/tools/launcher-shim.lib.sh" \
   "$HERE/tools/desktop-guardian-units.lib.sh" "$REL/tools/"
cp "$HERE/ensure-desktop-coverage.sh" "$REL/"
installed="$(cd "$REL" && HOME="$TMP/home" SLS_SETUP_LIB_ONLY=1 bash -c '
	source "$1/setup.sh" >/dev/null 2>&1
	install_slssteam >/dev/null 2>&1 || exit 1
	cd "$SLSDIR" && stat -c "%n:%a" desktop-coverage.lib.sh launcher-shim.lib.sh \
		desktop-guardian-units.lib.sh ensure-desktop-coverage.sh 2>&1
	[ -f SLSsteam.so ] && echo SLSsteam.so
' bash "$HERE" | tr '\n' ' ')"
check "install puts the helpers and the library next to the wrapper" \
  "desktop-coverage.lib.sh:644 launcher-shim.lib.sh:644 desktop-guardian-units.lib.sh:644 ensure-desktop-coverage.sh:755 SLSsteam.so " \
  "$installed"

[ "$fail" = 0 ] && echo "ALL PASS" || echo "FAILURES"
exit "$fail"
