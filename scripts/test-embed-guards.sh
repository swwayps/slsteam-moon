#!/usr/bin/env bash
# embed-config.sh turns res/config.yaml into compiled C++, so its contents are
# code: it wrapped the YAML in R"(...)", and a `)"` in the YAML closed the
# literal and compiled whatever followed. It did not validate its input, and
# that is not the kind of thing a review of a data-file change looks for.
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

# The script reads from ./res and writes into ./src, so run it in a sandbox.
mkdir -p "$TMP/res" "$TMP/src"
cp "$REPO/embed-config.sh" "$TMP/"

run_config() {
	printf '%s' "$1" > "$TMP/res/config.yaml"
	rm -f "$TMP/src/config_default.hpp"
	(cd "$TMP" && bash embed-config.sh >/dev/null 2>&1)
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

# ── the generated header actually compiles ──────────────────────────────────
if command -v g++ >/dev/null 2>&1; then
	run_config "AdditionalApps:
  - 480
Note: keeps a )\" and a \$dollar and a \`tick\`"
	cat > "$TMP/compile.cpp" <<'EOF'
#include "src/config_default.hpp"
#include <cstdio>
int main() {
	std::printf("%zu\n", __builtin_strlen(defaultConfig));
	return 0;
}
EOF
	check "G1 the generated header compiles" \
		'(cd "$TMP" && g++ -std=c++20 -I. compile.cpp -o compile 2>/dev/null)'
	check "G1b and keeps the embedded text" \
		'(cd "$TMP" && [ "$(./compile)" -gt 0 ])'
fi

# ---------------------------------------------------------------------------
# The terminator check must hold for an input LARGER than a pipe buffer. When it
# was written as `printf '%s' "$CONFIG" | grep -qF ...` under `set -o pipefail`,
# grep exited on the match, printf died on SIGPIPE, the pipeline reported 141 and
# the `if` read a found terminator as not-found. Past ~64 KiB the guard silently
# stopped firing, which is the one case where a config is most likely to be
# machine-generated and least likely to be eyeballed.
# ---------------------------------------------------------------------------
{
	printf ')SLSCFG"\n'
	# 200 KB of filler, comfortably past every pipe-buffer size.
	for _ in $(seq 1 2000); do
		printf 'x%.0s' $(seq 1 100)
		printf '\n'
	done
} > "$TMP/res/config.yaml"
rm -f "$TMP/src/config_default.hpp"
(cd "$TMP" && bash embed-config.sh >/dev/null 2>&1)
rc=$?
check "C6 a large config carrying the terminator is refused" '[ "$rc" -ne 0 ]'
check "C6b no header is written for the large refused config" \
	'[ ! -f "$TMP/src/config_default.hpp" ]'

# ...and a large BENIGN config is still embedded, so the guard is not just
# refusing everything big.
{
	printf 'AdditionalApps:\n'
	for _ in $(seq 1 2000); do
		printf '  - 480 # '
		printf 'y%.0s' $(seq 1 100)
		printf '\n'
	done
} > "$TMP/res/config.yaml"
rm -f "$TMP/src/config_default.hpp"
(cd "$TMP" && bash embed-config.sh >/dev/null 2>&1)
rc=$?
check "C7 a large benign config is still embedded" '[ "$rc" -eq 0 ]'
check "C7b the large header is written" '[ -f "$TMP/src/config_default.hpp" ]'
check "C7c the large header is not truncated" \
	'[ "$(wc -c < "$TMP/src/config_default.hpp")" -gt 200000 ]'

echo
echo "$checks check(s), $fails failure(s)"
[ "$fails" -eq 0 ] || exit 1
