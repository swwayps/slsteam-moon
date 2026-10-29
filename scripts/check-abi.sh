#!/usr/bin/env bash
# Reject a release binary that cannot run on the project's glibc 2.34 floor,
# so a host build is never packaged as a portable artifact. --x86-64 also
# requires an ELF64 x86-64 image (SLSsteam.so for the steamrt64 client).
# Usage: check-abi.sh <binary> [--x86-64]
set -euo pipefail
BINARY="${1:?usage: check-abi.sh <binary> [--x86-64]}"
READELF_BIN="${READELF:-readelf}"
if [ ! -s "$BINARY" ]; then
	echo "binary is missing or empty: $BINARY" >&2
	exit 1
fi
if [ ! -x "$READELF_BIN" ] && ! command -v "$READELF_BIN" >/dev/null 2>&1; then
	echo "readelf is required to verify the ABI of $BINARY" >&2
	exit 1
fi
if [ "${2:-}" = --x86-64 ]; then
	if ! header="$("$READELF_BIN" --file-header "$BINARY" 2>&1)"; then
		echo "could not inspect the ELF header of $BINARY" >&2
		exit 1
	fi
	if ! grep -Eq 'Class:[[:space:]]+ELF64' <<< "$header" \
	   || ! grep -Eq 'Machine:[[:space:]]+Advanced Micro Devices X86-64' <<< "$header"; then
		echo "$BINARY is not an ELF64 x86-64 binary" >&2
		exit 1
	fi
fi
if ! info="$("$READELF_BIN" --version-info "$BINARY" 2>&1)"; then
	echo "could not inspect the GLIBC requirements of $BINARY" >&2
	exit 1
fi
VERSIONS="$(sed -n 's/.*Name: GLIBC_\([0-9][0-9]*\)\.\([0-9][0-9]*\).*/\1 \2/p' <<< "$info")"
if [ -z "$VERSIONS" ]; then
	echo "$BINARY exposes no verifiable GLIBC requirements" >&2
	exit 1
fi
while read -r major minor; do
	if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -gt 34 ]; }; then
		echo "$BINARY requires GLIBC_${major}.${minor}; maximum allowed is GLIBC_2.34" >&2
		exit 1
	fi
done <<< "$VERSIONS"
echo "$BINARY is compatible with GLIBC_2.34"
