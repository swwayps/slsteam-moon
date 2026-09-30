#!/usr/bin/env bash
# One-shot release builder: portable build + zip the result.
#
# Equivalent to:
#   scripts/build.sh --portable
#   scripts/package.sh
#
# Output: dist/slsteam-moon-linux-<version>.zip
#
# Usage:
#   scripts/release.sh --version 2.0  # forwarded to package.sh
#   scripts/release.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
	sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
	exit 0
fi

"$SCRIPT_DIR/build.sh" --portable
"$SCRIPT_DIR/package.sh" "$@"
