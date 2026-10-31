#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# test-make-targets.sh — every tools/test_*.cpp is built by a Makefile target,
# so `make test-…` runs it and no test depends on a build line in a comment.
#
# Usage:  scripts/test-make-targets.sh
# Exit:   0 when every test has a target; 1 otherwise.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
missing=0
for test in tools/test_*.cpp; do
	grep -qF "$test" Makefile && continue
	echo "  FAIL - no Makefile target builds $test" >&2
	missing=$((missing + 1))
done
if [ "$missing" -ne 0 ]; then
	echo "$missing test(s) without a Makefile target" >&2
	exit 1
fi
echo "  ok   - every tools/test_*.cpp has a Makefile target"
