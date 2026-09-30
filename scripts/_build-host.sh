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

# 32-bit headers are required (-m32). Sniff for either pkg-config layout.
if ! [ -d /usr/lib/i386-linux-gnu/pkgconfig ] && ! [ -d /usr/lib32/pkgconfig ]; then
	missing+=("32-bit dev libs (libssl-dev:i386, libcurl4-openssl-dev:i386, or distro equivalent)")
fi

if [ ${#missing[@]} -gt 0 ]; then
	echo "missing dependencies:" >&2
	for d in "${missing[@]}"; do echo "  - $d" >&2; done
	cat >&2 <<-EOF

	Install on Ubuntu/Debian:
	  sudo dpkg --add-architecture i386
	  sudo apt update
	  sudo apt install g++-multilib make libssl-dev:i386 libcurl4-openssl-dev:i386

	Install on Arch:
	  sudo pacman -S multilib/lib32-gcc-libs multilib/lib32-openssl multilib/lib32-curl

	Or build inside a container instead:  scripts/build.sh --portable
	EOF
	exit 1
fi

# Make sure 32-bit pkg-config sees our libs.
if [ -d /usr/lib/i386-linux-gnu/pkgconfig ]; then
	export PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig
elif [ -d /usr/lib32/pkgconfig ]; then
	export PKG_CONFIG_PATH=/usr/lib32/pkgconfig
fi

make clean
make
echo "==> built: bin/SLSsteam.so, bin/pattern-refresh"
