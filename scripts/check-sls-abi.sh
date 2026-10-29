#!/usr/bin/env bash
# Reject SLSsteam binaries that cannot run on the project's glibc 2.34 floor.
# This keeps a host build from being packaged as a portable release artifact.

set -euo pipefail

BINARY="${1:-bin/SLSsteam.so}"
READELF_BIN="${READELF:-readelf}"

if [ ! -s "$BINARY" ]; then
	echo "SLSsteam binary is missing or empty: $BINARY" >&2
	exit 1
fi
if [ ! -x "$READELF_BIN" ] && ! command -v "$READELF_BIN" >/dev/null 2>&1; then
	echo "readelf is required to verify the SLSsteam ABI" >&2
	exit 1
fi

HEADER_OUTPUT="$(mktemp)"
VERSION_OUTPUT="$(mktemp)"
cleanup() {
	rm -f "$HEADER_OUTPUT" "$VERSION_OUTPUT"
}
trap cleanup EXIT

if ! "$READELF_BIN" --file-header "$BINARY" >"$HEADER_OUTPUT" 2>&1; then
	echo "could not inspect SLSsteam ELF header: $BINARY" >&2
	exit 1
fi
if ! grep -Eq 'Class:[[:space:]]+ELF64' "$HEADER_OUTPUT" \
   || ! grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64' "$HEADER_OUTPUT"; then
	echo "$BINARY is not an ELF64 x86-64 binary" >&2
	exit 1
fi

if ! "$READELF_BIN" --version-info "$BINARY" >"$VERSION_OUTPUT" 2>&1; then
	echo "could not inspect SLSsteam GLIBC requirements: $BINARY" >&2
	exit 1
fi

VERSIONS="$(sed -n \
	's/.*Name: GLIBC_\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2/p' \
	"$VERSION_OUTPUT")"
if [ -z "$VERSIONS" ]; then
	echo "SLSsteam exposes no verifiable GLIBC requirements: $BINARY" >&2
	exit 1
fi

while read -r major minor; do
	case "$major:$minor" in
		*[!0-9:]*|:*)
			echo "invalid GLIBC requirement in $BINARY" >&2
			exit 1
			;;
	esac
	if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 34 ]; }; then
		echo "$BINARY requires GLIBC_${major}.${minor}; maximum allowed is GLIBC_2.34" >&2
		exit 1
	fi
done <<< "$VERSIONS"

echo "SLSsteam ABI is compatible with ELF64 x86-64 and GLIBC_2.34"
