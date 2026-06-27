#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
#
# run-steamless.sh — wrapper-integration helper.  Invokes the
# bundled Steamless build under Wine to process a Windows
# executable. Designed to be called by the SLSsteam .so right
# before Steam launches a configured-AdditionalApps binary.
#
# Mirrors Accela's direct-Wine invocation pattern (cleaner than
# `proton run` for this case).
#
# Usage:
#   run-steamless.sh <exe-path>            # full unpack + swap
#   run-steamless.sh --prewarm             # initialise the Wine prefix
#                                          #   only; no exe required
#
# The --prewarm form is meant to be called once at SLSsteam startup
# in the background so the slow first-time Wine boot (~30s) completes
# before the user clicks Play. Subsequent unpacks finish in ~3s
# because the prefix is already initialised and the wineserver is
# still warm.
#
# Exit codes:
#   0  helper succeeded (or prewarm finished); when run with an exe
#      argument, original.exe.original.exe is the backup and the
#      processed binary now lives at original.exe
#   1  invalid arguments
#   2  exe doesn't carry the wrapper signature — already in target
#      shape, no work needed
#   3  no usable Wine binary (system wine missing, no Proton installed)
#   4  Steamless run failed (compile/dependency/timeout)
#   5  Steamless ran but produced no .unpacked.exe
#   6  rename step failed
#
# Environment variables (optional):
#   STEAMLESS_HOME    where Steamless.CLI.exe + Plugins/ live;
#                     default: <script dir>/../steamless-bin
#   WINE_BIN          override wine binary path (default: probe Proton then PATH)
#   WINE_PREFIX       override prefix path
#                     (default: ~/.local/share/SLSsteam/steamless-prefix)
#   QUIET             non-empty -> suppress informational stdout
#                     (errors still go to stderr)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREWARM=0
EXE_PATH=""

case "${1:-}" in
    --prewarm)
        PREWARM=1
        ;;
    *)
        EXE_PATH="${1:-}"
        ;;
esac

log()  { [ -z "${QUIET:-}" ] && echo "[steamless-bypass] $*"; return 0; }
warn() { echo "[steamless-bypass] WARN: $*" >&2; }
die()  { echo "[steamless-bypass] ERROR: $*" >&2; exit "${2:-1}"; }

# ── 1. validate args ────────────────────────────────────────────────────
if [ "$PREWARM" -eq 0 ]; then
    [ -n "$EXE_PATH" ] || die "usage: $0 <exe-path>  |  $0 --prewarm" 1
    [ -f "$EXE_PATH" ] || die "exe not found: $EXE_PATH" 1

    # Refuse our own artefacts so a buggy caller can't recurse on the
    # backup we left behind (`<exe>.original.exe`) or the transient
    # output (`<exe>.unpacked.exe`). Treated as a no-op success so
    # batch callers don't bail.
    case "$(basename "$EXE_PATH")" in
        *.original.exe|*.unpacked.exe)
            log "exe is a Steamless artefact ($(basename "$EXE_PATH")); skipping"
            exit 2
            ;;
    esac

    # ── 2. probe SteamStub markers ──────────────────────────────────────
    # Two variants we handle:
    #   - v2 (x86): 'VLV\0' magic at file offset 0x40.
    #   - v3 (x86/x64): no fixed offset magic, but the unpacker header
    #     lives in a PE section named ".bind". Reading the section
    #     table is the canonical detection (Steamless does the same).
    # Quick exit if neither marker is present, to avoid the 5-30 s
    # Wine startup cost on plain non-stub'd binaries.
    #
    # Detection is done entirely in python3 (already a hard dependency of
    # this script) so it depends on nothing beyond a base install. An
    # earlier version read the v2 magic via `dd | xxd`; `xxd` ships in
    # vim-common and is absent from minimal installs (e.g. Fedora), so
    # under `set -euo pipefail` the missing tool aborted the whole helper
    # with rc=127 before any signature was examined — silently disabling
    # DRM removal for every SteamStub title on those distros.
    stub_kind="$(python3 - "$EXE_PATH" <<'PY'
import struct, sys

def detect(path):
    with open(path, 'rb') as f:
        head = f.read(4096)
    # v2 (x86): 'VLV\0' magic at file offset 0x40.
    if head[0x40:0x44] == b'VLV\x00':
        return 'v2'
    # v3 (x86/x64): no fixed-offset magic; the unpacker header lives in
    # a PE section named '.bind'. Walking the section table is the
    # canonical detection (Steamless does the same).
    if head[:2] != b'MZ':
        return 'none'
    e_lfanew = struct.unpack_from('<I', head, 0x3c)[0]
    if e_lfanew + 0x18 > len(head) or head[e_lfanew:e_lfanew+4] != b'PE\x00\x00':
        return 'none'
    nsec    = struct.unpack_from('<H', head, e_lfanew + 6)[0]
    optsize = struct.unpack_from('<H', head, e_lfanew + 0x14)[0]
    sec_off = e_lfanew + 0x18 + optsize
    if sec_off + nsec * 40 > len(head):
        with open(path, 'rb') as f:
            head = f.read(sec_off + nsec * 40 + 16)
    for i in range(nsec):
        name = head[sec_off + i*40 : sec_off + i*40 + 8].rstrip(b'\x00')
        if name == b'.bind':
            return 'v3'
    return 'none'

try:
    print(detect(sys.argv[1]))
except Exception:
    print('none')
PY
)"
    case "$stub_kind" in
        v2)
            log "SteamStub v2 (VLV) signature detected — proceeding"
            ;;
        v3)
            log "SteamStub v3 (.bind section) detected — proceeding"
            ;;
        *)
            log "exe is not SteamStub-wrapped (no VLV magic, no .bind section); skipping"
            exit 2
            ;;
    esac
fi

# ── 3. resolve Steamless home (skipped in --prewarm) ───────────────────
if [ "$PREWARM" -eq 0 ]; then
    STEAMLESS_HOME="${STEAMLESS_HOME:-$SCRIPT_DIR/../steamless-bin}"
    STEAMLESS_CLI="$STEAMLESS_HOME/Steamless.CLI.exe"
    [ -f "$STEAMLESS_CLI" ] || die "Steamless.CLI.exe not found at $STEAMLESS_CLI" 4

    # Steamless's CLI exe loads Steamless.API.dll from the working
    # directory. Plugins live in Plugins/. If the API dll is only in
    # Plugins/, copy it up so the CLI can find it. Idempotent.
    if [ ! -f "$STEAMLESS_HOME/Steamless.API.dll" ] \
       && [ -f "$STEAMLESS_HOME/Plugins/Steamless.API.dll" ]; then
        cp "$STEAMLESS_HOME/Plugins/Steamless.API.dll" "$STEAMLESS_HOME/Steamless.API.dll"
    fi
fi

# ── 4. locate wine binary ───────────────────────────────────────────────
# Order: explicit override > Proton-experimental in standard Steam paths >
#        Proton in compatibilitytools.d > system wine.
find_wine() {
    if [ -n "${WINE_BIN:-}" ] && [ -x "$WINE_BIN" ]; then
        echo "$WINE_BIN"; return
    fi

    local candidates=()
    for steam_root in \
        "$HOME/.steam/steam" \
        "$HOME/.steam/debian-installation" \
        "$HOME/.local/share/Steam"
    do
        candidates+=( "$steam_root/steamapps/common/Proton - Experimental/files/bin/wine" )
        candidates+=( "$steam_root/steamapps/common/Proton"*/files/bin/wine )
        candidates+=( "$steam_root/compatibilitytools.d/"*/files/bin/wine )
        candidates+=( "$steam_root/compatibilitytools.d/"*/dist/bin/wine )
    done
    # System-wide compat tools (e.g. CachyOS ships Proton here).
    candidates+=( "/usr/share/steam/compatibilitytools.d/"*/files/bin/wine )
    candidates+=( "/usr/share/steam/compatibilitytools.d/"*/dist/bin/wine )
    candidates+=( "$(command -v wine 2>/dev/null || true)" )

    for c in "${candidates[@]}"; do
        if [ -n "$c" ] && [ -x "$c" ]; then
            echo "$c"; return
        fi
    done
    return 1
}

WINE_BIN="$(find_wine)" || die "no usable Wine binary found" 3
log "using wine: $WINE_BIN"

# Wine and wineserver have to come from the same install or the
# version mismatch crashes the prefix. Always pair them.
WINE_DIR="$(dirname "$WINE_BIN")"
WINESERVER_BIN="$WINE_DIR/wineserver"
[ -x "$WINESERVER_BIN" ] || warn "wineserver not at $WINESERVER_BIN; relying on PATH"

# ── 5. set up environment ───────────────────────────────────────────────
WINE_PREFIX="${WINE_PREFIX:-$HOME/.local/share/SLSsteam/steamless-prefix}"
mkdir -p "$WINE_PREFIX"

# If using Proton, point the dynamic linker at Proton's bundled libs.
# Otherwise let the system loader resolve it. The Proton lib paths
# follow the standard layout: <proton_root>/files/{lib,lib64}.
if [[ "$WINE_BIN" == */Proton*/files/bin/wine ]]; then
    PROTON_FILES="$(dirname "$(dirname "$WINE_DIR")")"
    LD_LIBRARY_PATH="$PROTON_FILES/lib64:$PROTON_FILES/lib:${LD_LIBRARY_PATH:-}"
    WINEDLLPATH="$PROTON_FILES/lib64/wine:$PROTON_FILES/lib/wine"
    export LD_LIBRARY_PATH WINEDLLPATH
    log "configured Proton library paths"
fi

export WINEPREFIX="$WINE_PREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINESERVER="${WINESERVER:-$WINESERVER_BIN}"
export WINEARCH="${WINEARCH:-win64}"

# Steamless is a .NET app and renders no HTML, so disable Wine's Gecko
# (mshtml) package — otherwise a fresh prefix pops the "install Gecko"
# dialog, which hangs a headless run.  Leave Mono (mscoree) at its
# default so the .NET runtime still loads.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mshtml=}"

# Different Steam runtime ships overlapping wineservers. Kill any
# stragglers before kicking off our own — version mismatch otherwise
# crashes immediately.
pkill -9 -f wineserver 2>/dev/null || true

# ── 6. lazy prefix init (~30s on first run) ─────────────────────────────
if [ ! -f "$WINEPREFIX/system.reg" ]; then
    log "initializing Wine prefix at $WINEPREFIX (one-time, ~30s)"
    "$WINE_BIN" wineboot --init >/dev/null 2>&1 || die "wineboot --init failed" 4
    "$WINESERVER" -w 2>/dev/null || true
fi

# Prewarm path stops here — the prefix is now initialised, the wineserver
# is warm, and the next call with a real exe path can run Steamless
# immediately.
if [ "$PREWARM" -eq 1 ]; then
    log "prewarm complete"
    exit 0
fi

# ── 7. invoke Steamless ─────────────────────────────────────────────────
# Steamless writes <exe>.unpacked.exe next to the input. Wine paths
# need to be drive-mapped — Z:\ maps to /. Everything else is just
# slash-flipping.
WIN_PATH="Z:${EXE_PATH//\//\\}"
log "running Steamless on $EXE_PATH"

cd "$STEAMLESS_HOME"

# 90s timeout is generous; Skyrim's 37 MB exe takes ~3s on a warm prefix.
# Steamless CLI exit codes: 0 = unpacked OK, 1 = no SteamStub DRM present
# (benign — treat as "nothing to do"), >1 = real failure.
set +e
timeout 90 "$WINE_BIN" Steamless.CLI.exe \
        --quiet --realign --recalcchecksum -f "$WIN_PATH" \
        > /tmp/steamless-bypass.$$.log 2>&1
sl_rc=$?
set -e

if [ "$sl_rc" -eq 1 ] && [ ! -f "$EXE_PATH.unpacked.exe" ]; then
    log "Steamless reports no SteamStub DRM; nothing to do"
    rm -f /tmp/steamless-bypass.$$.log
    exit 2
fi
if [ "$sl_rc" -ne 0 ]; then
    warn "Steamless invocation failed (rc=$sl_rc); log follows:"
    sed 's/^/  /' /tmp/steamless-bypass.$$.log >&2
    rm -f /tmp/steamless-bypass.$$.log
    die "Steamless failed" 4
fi
[ -z "${QUIET:-}" ] && cat /tmp/steamless-bypass.$$.log
rm -f /tmp/steamless-bypass.$$.log

UNPACKED="$EXE_PATH.unpacked.exe"
[ -f "$UNPACKED" ] || die "no unpacked output at $UNPACKED" 5

# ── 8. atomic-ish rename ────────────────────────────────────────────────
BACKUP="$EXE_PATH.original.exe"
log "swapping unpacked into place (backup: $BACKUP)"
if [ -f "$BACKUP" ]; then
    rm -f "$BACKUP"
fi
mv "$EXE_PATH" "$BACKUP" || die "backup move failed" 6
mv "$UNPACKED" "$EXE_PATH" || {
    warn "swap-in failed; rolling back"
    mv "$BACKUP" "$EXE_PATH"
    die "swap failed" 6
}
chmod +x "$EXE_PATH"

# Marker so we don't reprocess the same exe forever.
touch "$EXE_PATH.steamless_done"

log "done — $EXE_PATH processed"
exit 0
