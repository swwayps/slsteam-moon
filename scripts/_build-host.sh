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

# SLSsteam.so is built with -m32, which needs the 32-bit C++ runtime.
if command -v g++ >/dev/null 2>&1 \
   && ! echo 'int main() {}' | g++ -m32 -x c++ - -o /dev/null >/dev/null 2>&1; then
	missing+=("32-bit C++ toolchain (g++-multilib, or the distro's i686 libstdc++ and glibc dev packages)")
fi

if [ ${#missing[@]} -gt 0 ]; then
	echo "missing dependencies:" >&2
	for d in "${missing[@]}"; do echo "  - $d" >&2; done
	cat >&2 <<-EOF

	SLSsteam.so needs a -m32 toolchain and the curl headers; bin/pattern-refresh
	links the native libcurl and libcrypto; the -m32 tests link the 32-bit
	libcrypto.

	Install on Ubuntu/Debian:
	  sudo dpkg --add-architecture i386
	  sudo apt update
	  sudo apt install g++-multilib make libcurl4-openssl-dev libcurl4-openssl-dev:i386 libssl-dev libssl-dev:i386

	Install on Arch:
	  sudo pacman -S multilib/lib32-gcc-libs multilib/lib32-openssl multilib/lib32-curl

	Or build inside a container instead:  scripts/build.sh --portable
	EOF
	exit 1
fi

make clean
make
echo "==> built: bin/SLSsteam.so, bin/pattern-refresh"
