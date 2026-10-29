#!/bin/bash
# Check slsteam-moon binary compatibility against the running host.

# Resolve repo root from script location (scripts/check-compat.sh).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

BINARY="./bin/SLSsteam.so"

if [ ! -f "$BINARY" ]; then
	echo "ERROR: $BINARY not found!"
	echo "Build it first: scripts/build.sh   (or scripts/build.sh --host)"
	exit 1
fi

echo "========================================="
echo "  SLSsteam Binary Compatibility Check"
echo "========================================="
echo ""

# Check if it's 64-bit (the steamrt64 client)
FILE_OUTPUT=$(file "$BINARY")
echo "Binary type:"
echo "  $FILE_OUTPUT"
echo ""

if ! echo "$FILE_OUTPUT" | grep -q "64-bit"; then
	echo "⚠️  WARNING: Binary is not 64-bit! The steamrt64 client requires 64-bit."
	echo ""
fi

# Check glibc requirements
if command -v objdump &> /dev/null; then
	echo "GLIBC version requirements:"
	GLIBC_VERSIONS=$(objdump -T "$BINARY" 2>/dev/null | grep -oP 'GLIBC_\K[0-9.]+' | sort -Vru | head -10)
	if [ -z "$GLIBC_VERSIONS" ]; then
		echo "  (none detected)"
	else
		echo "$GLIBC_VERSIONS" | sed 's/^/  GLIBC_/'
	fi
	
	HIGHEST_GLIBC=$(echo "$GLIBC_VERSIONS" | head -1)
	echo ""
	echo "Minimum required: glibc >= $HIGHEST_GLIBC"
else
	echo "⚠️  objdump not found, cannot check GLIBC requirements"
	HIGHEST_GLIBC="unknown"
fi

echo ""

# Check system glibc
SYSTEM_GLIBC=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')
if [ -n "$SYSTEM_GLIBC" ]; then
	echo "Your system:"
	echo "  glibc $SYSTEM_GLIBC"
	echo ""
	
	if [ "$HIGHEST_GLIBC" != "unknown" ]; then
		# Compare versions
		if [ "$(printf '%s\n' "$HIGHEST_GLIBC" "$SYSTEM_GLIBC" | sort -V | head -1)" = "$HIGHEST_GLIBC" ]; then
			echo "✅ Compatible: Your system meets requirements"
		else
			echo "❌ Incompatible: Your system is too old"
			echo ""
			echo "Solutions:"
			echo "  1. Rebuild locally: make clean && make"
			echo "  2. Use a pre-built release from upstream"
		fi
	fi
fi

echo ""

# Check linked libraries
if command -v ldd &> /dev/null; then
	echo "Linked libraries:"
	ldd "$BINARY" 2>&1 | sed 's/^/  /'
	echo ""
fi

# Compatibility matrix
echo "========================================="
echo "  Compatibility Guide"
echo "========================================="
echo ""
echo "Distribution compatibility (for glibc $HIGHEST_GLIBC+):"
echo ""

case "$HIGHEST_GLIBC" in
	2.17)
		echo "  ✅ Ubuntu 14.04+"
		echo "  ✅ Debian 8+"
		echo "  ✅ CentOS 7+"
		echo "  ✅ All modern distros"
		echo ""
		echo "  🎯 MAXIMUM COMPATIBILITY"
		;;
	2.27)
		echo "  ✅ Ubuntu 18.04+"
		echo "  ✅ Debian 10+"
		echo "  ✅ CentOS 8+"
		;;
	2.31)
		echo "  ✅ Ubuntu 20.04+"
		echo "  ✅ Debian 11+"
		echo "  ✅ Fedora 32+"
		;;
	2.35|2.36)
		echo "  ✅ Ubuntu 22.04+"
		echo "  ✅ Debian 12+"
		echo "  ✅ Fedora 36+"
		;;
	2.39)
		echo "  ✅ Ubuntu 24.04+"
		echo "  ✅ Fedora 40+"
		echo "  ⚠️  PopOS 24.04+"
		;;
	2.4*)
		echo "  ⚠️  Very new - Ubuntu 26.04+, cutting-edge distros only"
		echo ""
		echo "  ⚠️  LIMITED COMPATIBILITY"
		echo ""
		echo "  Consider rebuilding with Docker for better portability:"
		echo "    ./scripts/build/build-docker.sh"
		;;
	unknown)
		echo "  (Cannot determine - objdump not available)"
		;;
esac

echo ""

# Build recommendation
if [ "$HIGHEST_GLIBC" != "unknown" ] && [ "$HIGHEST_GLIBC" != "2.17" ]; then
	echo "========================================="
	echo "  Recommendation"
	echo "========================================="
	echo ""
	echo "For maximum compatibility, rebuild with Docker:"
	echo ""
	echo "  ./scripts/build/build-docker.sh"
	echo ""
	echo "This produces a binary that works on glibc 2.17+ (2012+)"
	echo ""
fi
