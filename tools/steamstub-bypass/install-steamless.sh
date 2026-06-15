#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# install-steamless.sh — fetch Steamless from GitHub releases and
# place it where run-steamless.sh expects it. Idempotent.
#
# Default install dir matches the path that run-steamless.sh probes
# when STEAMLESS_HOME is not set:
#   <script dir>/../steamless-bin
#
# Usage:
#   install-steamless.sh                        # next to this script
#   install-steamless.sh --user-local           # ~/.local/share/SLSsteam/steamless-bin
#   install-steamless.sh --target <absolute>    # explicit destination
#
# --target wins over --user-local if both are passed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="$SCRIPT_DIR/../steamless-bin"

while [ $# -gt 0 ]; do
    case "$1" in
        --user-local)
            TARGET_DIR="$HOME/.local/share/SLSsteam/steamless-bin"
            shift
            ;;
        --target)
            if [ -z "${2:-}" ]; then
                echo "[install-steamless] --target needs a path" >&2
                exit 2
            fi
            TARGET_DIR="$2"
            shift 2
            ;;
        *)
            echo "[install-steamless] unknown arg: $1" >&2
            exit 2
            ;;
    esac
done

mkdir -p "$TARGET_DIR"

if [ -f "$TARGET_DIR/Steamless.CLI.exe" ] && [ -f "$TARGET_DIR/Steamless.API.dll" ]; then
    echo "[install-steamless] already installed at $TARGET_DIR"
    exit 0
fi

# Resolve latest release URL from the GitHub API.  Pick the first asset
# whose name looks like a Steamless zip (avoid grabbing an unrelated or
# source-archive zip if upstream adds more assets later).
echo "[install-steamless] resolving latest Steamless release..."
API_JSON="$(curl -fsSL https://api.github.com/repos/atom0s/Steamless/releases/latest || true)"
ASSET_URL="$(
    printf '%s' "$API_JSON" \
    | grep -oE 'https://[^"]+/releases/download/[^"]+\.zip' \
    | grep -iE 'steamless' \
    | head -1
)"
# Fallback: any download asset ending in .zip.
if [ -z "$ASSET_URL" ]; then
    ASSET_URL="$(
        printf '%s' "$API_JSON" \
        | grep -oE 'https://[^"]+/releases/download/[^"]+\.zip' \
        | head -1
    )"
fi
if [ -z "$ASSET_URL" ]; then
    echo "[install-steamless] could not resolve a release asset URL" >&2
    echo "[install-steamless] (GitHub API rate limit or no internet?)" >&2
    exit 1
fi
echo "[install-steamless] downloading $ASSET_URL"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/steamless.zip" "$ASSET_URL"
unzip -q -o "$TMP/steamless.zip" -d "$TMP/extract"

# Move artifacts. Layout is:
#   Steamless.CLI.exe   (and .config)
#   Steamless.exe       (and .config; we keep but don't use)
#   Plugins/*.dll
cp -r "$TMP/extract/." "$TARGET_DIR/"

# run-steamless.sh expects Steamless.API.dll alongside the CLI exe;
# upstream ships it inside Plugins/. Mirror it up.
if [ ! -f "$TARGET_DIR/Steamless.API.dll" ] \
   && [ -f "$TARGET_DIR/Plugins/Steamless.API.dll" ]; then
    cp "$TARGET_DIR/Plugins/Steamless.API.dll" "$TARGET_DIR/Steamless.API.dll"
fi

# Validate the unpack actually produced a usable CLI — otherwise callers
# would think Steamless is installed when it isn't.
if [ ! -f "$TARGET_DIR/Steamless.CLI.exe" ]; then
    echo "[install-steamless] download succeeded but Steamless.CLI.exe is missing" >&2
    echo "[install-steamless] (unexpected asset layout)" >&2
    exit 1
fi

echo "[install-steamless] installed at $TARGET_DIR"

# Soft check: Steamless runs under Wine (or a Proton runtime).  We
# don't require either at install time because run-steamless.sh
# resolves them lazily, but a friendly heads-up saves debugging
# later when nothing is available.
if ! command -v wine >/dev/null 2>&1 \
   && ! ls -d "$HOME/.steam/steam/steamapps/common/Proton"* >/dev/null 2>&1 \
   && ! ls -d "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Proton"* >/dev/null 2>&1; then
    echo "[install-steamless] note: no system 'wine' and no installed Proton found."
    echo "                         Install one before launching Steam-Stub-wrapped apps."
    echo "                         Steamless requires .NET-on-Wine to run; the wrapper"
    echo "                         helper falls back to a Proton runtime if 'wine' is"
    echo "                         missing, but at least one must be present."
fi
