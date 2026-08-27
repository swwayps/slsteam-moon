#!/usr/bin/env bash
# The embed scripts turn res/ data files into compiled C++, so their contents are
# code. embed-config.sh wrapped res/config.yaml in R"(...)" — a `)"` in the YAML
# closes the literal and whatever follows is compiled. embed-version.sh
# interpolated res/version.txt straight into an initialiser, so "0; /* ... */"
# compiled too. Neither validated its input, and neither is the kind of thing a
# review of a data-file change looks for.
#
# Run from the repo root:  bash scripts/test-embed-guards.sh
set -u
fails=0
checks=0
check() {
	checks=$((checks + 1))
	if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Both scripts read from ./res and write into ./src, so run them in a sandbox.
mkdir -p "$TMP/res" "$TMP/src"
cp "$REPO/embed-config.sh" "$REPO/embed-version.sh" "$TMP/"

run_config() {
	printf '%s' "$1" > "$TMP/res/config.yaml"
	rm -f "$TMP/src/config_default.hpp"
	(cd "$TMP" && bash embed-config.sh >/dev/null 2>&1)
}
run_version() {
	printf '%s' "$1" > "$TMP/res/version.txt"
	rm -f "$TMP/src/version.hpp"
	(cd "$TMP" && bash embed-version.sh >/dev/null 2>&1)
}

# ── embed-config.sh ─────────────────────────────────────────────────────────
check "C1 an ordinary config is embedded" \
	'run_config "AdditionalApps:
  - 480
LogLevel: 2"'
check "C1b the header is generated" '[ -f "$TMP/src/config_default.hpp" ]'
check "C1c the content is present verbatim" \
	'grep -q "LogLevel: 2" "$TMP/src/config_default.hpp"'
check "C1d a custom raw-string delimiter is used" \
	'grep -q "R\"SLSCFG(" "$TMP/src/config_default.hpp"'

# A config containing the DEFAULT delimiter's terminator is harmless now, because
# the delimiter is not the default one.
check "C2 a config containing )\" is still embedded" \
	'run_config "Note: this has a )\" inside it"'
check "C2b it is embedded as data, not code" \
	'grep -q "this has a )\" inside it" "$TMP/src/config_default.hpp"'

# A config containing the ACTUAL terminator must be refused outright rather than
# producing a header that compiles the rest of the file as C++.
check "C3 a config carrying the real terminator is refused" \
	'! run_config "Evil: )SLSCFG\"; int pwn(){return 1;} const char* x = R\"SLSCFG("'
check "C3b no header is written when refused" \
	'[ ! -f "$TMP/src/config_default.hpp" ]'

check "C4 a carriage return is refused" \
	'! run_config "$(printf "Key: value\r\n")"'
check "C5 a missing config file is refused" \
	'rm -f "$TMP/res/config.yaml"; ! (cd "$TMP" && bash embed-config.sh >/dev/null 2>&1)'

# ── embed-version.sh ────────────────────────────────────────────────────────
check "V1 a plain integer version is embedded" 'run_version "28"'
check "V1b the constant is written" \
	'grep -q "constexpr uint64_t VERSION = 28ULL;" "$TMP/src/version.hpp"'
check "V2 surrounding whitespace is tolerated" 'run_version "  28
"'
check "V2b and still yields the same constant" \
	'grep -q "VERSION = 28ULL;" "$TMP/src/version.hpp"'

check "V3 an injected statement is refused" \
	'! run_version "0; int pwn(){return 1;} constexpr int y = 0"'
check "V3b no header is written when refused" '[ ! -f "$TMP/src/version.hpp" ]'
check "V4 a hex literal is refused" '! run_version "0x1c"'
check "V5 a version expression is refused" '! run_version "1+1"'
check "V6 an empty version is refused" '! run_version ""'
check "V7 a version that cannot fit a uint64_t is refused" \
	'! run_version "111111111111111111111111111"'
check "V8 a missing version file is refused" \
	'rm -f "$TMP/res/version.txt"; ! (cd "$TMP" && bash embed-version.sh >/dev/null 2>&1)'

# ── the generated headers actually compile ──────────────────────────────────
if command -v g++ >/dev/null 2>&1; then
	run_config "AdditionalApps:
  - 480
Note: keeps a )\" and a \$dollar and a \`tick\`"
	run_version "28"
	cat > "$TMP/compile.cpp" <<'EOF'
#include "src/config_default.hpp"
#include "src/version.hpp"
#include <cstdio>
int main() {
	std::printf("%llu %zu\n", (unsigned long long)VERSION,
	            sizeof(defaultConfig) ? __builtin_strlen(defaultConfig) : 0);
	return 0;
}
EOF
	check "G1 the generated headers compile together" \
		'(cd "$TMP" && g++ -std=c++20 -I. compile.cpp -o compile 2>/dev/null)'
	check "G1b and report the embedded version" \
		'(cd "$TMP" && ./compile | cut -d" " -f1 | grep -qx 28)'
fi

echo
echo "$checks check(s), $fails failure(s)"
[ "$fails" -eq 0 ] || exit 1
