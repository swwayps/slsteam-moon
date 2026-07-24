#!/usr/bin/env bash
# Package the release zip that GitHub releases distribute.
#
# Produces:
#   dist/slsteam-moon-linux-<version>.zip
#
# Layout inside the zip (matches what consumers and luatools-moon
# expect: a single top-level slsteam-moon-<version>/ directory containing
# setup.sh and bin/, and the assets setup.sh references):
#
#   slsteam-moon-<version>/
#     bin/
#       SLSsteam.so
#       library-inject.so
#     docs/
#       LICENSE/                   (third-party + slsteam-moon licenses)
#     res/
#       config.yaml                (default config copied to ~/.config/SLSsteam)
#     setup.sh                     (installer the user runs)
#     tools/
#       steamstub-bypass/          (helper called by setup.sh install_steamstub)
#         install-steamless.sh
#         run-steamless.sh
#         scan-all.sh
#       steamless-bin/             (Steamless kit, bundled verbatim — see
#                                   docs/LICENSE/steamless; enables offline
#                                   SteamStub DRM removal, no download)
#         Steamless.CLI.exe
#         Plugins/*.dll
#
# Requires: bin/SLSsteam.so + bin/library-inject.so (run scripts/build.sh first).
#
# Usage:
#   scripts/package.sh                      # version from res/version.txt
#   scripts/package.sh --version 2.0        # explicit version
#   scripts/package.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

VERSION=""
while [ $# -gt 0 ]; do
	case "$1" in
		--version) VERSION="${2:-}"; shift 2 ;;
		-h|--help)
			sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
	esac
done

if [ -z "$VERSION" ]; then
	if [ -r res/version.txt ]; then
		VERSION="$(tr -d '[:space:]' < res/version.txt)"
	fi
fi
if [ -z "$VERSION" ]; then
	echo "could not determine version (no --version, no res/version.txt)" >&2
	exit 1
fi

# Sanity-check the binaries are present and look right.
for f in bin/SLSsteam.so bin/library-inject.so; do
	if [ ! -f "$f" ]; then
		echo "missing $f" >&2
		echo "build first:  scripts/build.sh   (or scripts/build.sh --host for dev)" >&2
		exit 1
	fi
done

# Empty/zero-byte binaries would silently produce a broken release zip.
if [ ! -s bin/SLSsteam.so ] || [ ! -s bin/library-inject.so ]; then
	echo "binaries in bin/ are empty -- rebuild before packaging" >&2
	exit 1
fi

PKG_DIR="dist/slsteam-moon-${VERSION}"
ZIP_PATH="dist/slsteam-moon-linux-${VERSION}.zip"

echo "==> staging $PKG_DIR"
rm -rf "$PKG_DIR" "$ZIP_PATH"
mkdir -p "$PKG_DIR/bin" "$PKG_DIR/docs" "$PKG_DIR/res" "$PKG_DIR/tools"

cp bin/SLSsteam.so       "$PKG_DIR/bin/"
cp bin/library-inject.so "$PKG_DIR/bin/"
cp setup.sh              "$PKG_DIR/"
cp res/config.yaml       "$PKG_DIR/res/"
cp -r docs/LICENSE       "$PKG_DIR/docs/"
cp -r tools/steamstub-bypass "$PKG_DIR/tools/"

# Desktop-coverage helpers: reconciliation engine, user-unit/drop-in manager and
# thin CLI. setup.sh installs all three under $SLSDIR.
cp tools/desktop-coverage.lib.sh       "$PKG_DIR/tools/desktop-coverage.lib.sh"
cp tools/desktop-guardian-units.lib.sh "$PKG_DIR/tools/desktop-guardian-units.lib.sh"
cp ensure-desktop-coverage.sh          "$PKG_DIR/ensure-desktop-coverage.sh"

# Strip any build/IDE artefacts that may live alongside steamstub-bypass.
find "$PKG_DIR/tools/steamstub-bypass" -mindepth 1 \
	! -name '*.sh' \
	-type f -delete 2>/dev/null || true

# Bundle the Steamless kit verbatim so SteamStub DRM removal works
# offline — no install-time GitHub download (the silent failure mode
# behind "Application load error 6").  Steamless is CC BY-NC-ND 4.0:
# we ship it unmodified and attribute it (docs/LICENSE/steamless).
if [ ! -f tools/steamless-bin/Steamless.CLI.exe ]; then
	echo "missing tools/steamless-bin/Steamless.CLI.exe -- the Steamless kit" >&2
	echo "must be committed so releases are self-contained." >&2
	exit 1
fi
cp -r tools/steamless-bin "$PKG_DIR/tools/"

echo "==> zipping $ZIP_PATH"
( cd dist && zip -qr -9 "slsteam-moon-linux-${VERSION}.zip" "slsteam-moon-${VERSION}" )

# Show a quick listing so callers can eyeball the contents.
echo
unzip -l "$ZIP_PATH"
echo
echo "==> ready: $ZIP_PATH ($(du -h "$ZIP_PATH" | cut -f1))"
