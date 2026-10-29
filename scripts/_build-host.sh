#!/usr/bin/env bash
# Host build implementation. Invoked by scripts/build.sh --host.
# Compiles in place using the host's toolchain. Fast for development;
# binary glibc minimum equals your system's glibc.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

echo "==> host build (glibc $(ldd --version | head -1 | awk '{print $NF}'))"

missing=()
command -v g++  >/dev/null 2>&1 || missing+=("g++")
command -v make >/dev/null 2>&1 || missing+=("make")

command -v pkg-config >/dev/null 2>&1 || missing+=("pkg-config")

if [ ${#missing[@]} -gt 0 ]; then
	echo "missing dependencies:" >&2
	for d in "${missing[@]}"; do echo "  - $d" >&2; done
	cat >&2 <<-EOF

	Install on Ubuntu/Debian:
	  sudo apt install g++ make pkg-config libssl-dev libcurl4-openssl-dev

	Install on Arch:
	  sudo pacman -S gcc make pkgconf openssl curl

	Or build inside a container instead:  scripts/build.sh --portable
	EOF
	exit 1
fi

make clean
make
echo "==> built: bin/SLSsteam.so, bin/pattern-refresh"
