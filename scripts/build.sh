#!/usr/bin/env bash
# Build slsteam-moon binaries.
#
# Two modes:
#   --host       Use the host toolchain (fast, dev iteration).
#                Binary's glibc requirement matches your system, so it
#                may not run on older distros than yours.
#   --portable   Build inside an Ubuntu 22.04 container (Podman or
#                Docker). The resulting binary works on any distro with
#                glibc >= 2.34 (Ubuntu 22.04+, Debian 12+, Fedora 36+,
#                most modern distros). This is what release builds use.
#
# Outputs:
#   bin/SLSsteam.so
#   bin/pattern-refresh
#
# Usage:
#   scripts/build.sh                 # defaults to --portable
#   scripts/build.sh --host
#   scripts/build.sh --portable
#   scripts/build.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

MODE="portable"
for arg in "$@"; do
	case "$arg" in
		--host)     MODE="host" ;;
		--portable) MODE="portable" ;;
		-h|--help)
			sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
	esac
done

case "$MODE" in
	host)     "$SCRIPT_DIR/_build-host.sh" ;;
	portable) "$SCRIPT_DIR/_build-portable.sh" ;;
esac
