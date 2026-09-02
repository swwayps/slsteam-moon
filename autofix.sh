#!/usr/bin/env bash
# ============================================================================
# slsteam-moon auto-fix
#
# Runs when Lumen detects that slsteam-moon did NOT inject into the current
# Steam session (the "Buy" button shows on games added via LuaTools). It:
#   1. downloads the LATEST slsteam-moon (Lumen) release asset,
#   2. runs its setup.sh install — which stops Steam, installs the library and
#      repairs EVERY *steam*.desktop launcher on the system (prompting for the
#      sudo password here in the terminal for the system-wide entries),
#   3. relaunches Steam through the injected wrapper.
#
# Self-contained: fetched + run via `curl -fsSL <raw>/autofix.sh | bash`, so it
# must not depend on any sibling file. Only touches slsteam-moon (not Lumen or
# the LuaTools plugin).
#
# Served from the repo's raw branch URL, so fixes go live without a release cut.
# ============================================================================
set -u

REPO="swwayps/slsteam-moon"
# Asset name matcher. jq's test() sees the parsed .name (no surrounding quotes),
# so it uses '.*'; the grep fallback scans raw JSON, so it uses '[^"]*' to avoid
# crossing a quote. Both use '[.]' for a literal dot (a bare '\.' is not a valid
# jq string escape and a raw '"' inside a jq "..." string breaks the parse).
ASSET_RE_JQ='^slsteam-moon-linux-.*-lumen[.]zip$'
ASSET_RE_GREP='slsteam-moon-linux-[^"]*-lumen[.]zip"'
MIRROR_MANIFEST="https://cdn.jsdelivr.net/gh/swwayps/jsdelivr@main/manifest.json"
SLSDIR="$HOME/.local/share/SLSsteam"
WRAPPER="$SLSDIR/path/steam"

# ── pretty output (degrades to plain when not a TTY) ────────────────────────
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != "dumb" ]; then
	BOLD=$'\033[1m'; NC=$'\033[0m'
	BLUE=$'\033[38;5;75m'; GREEN=$'\033[38;5;114m'; RED=$'\033[38;5;203m'; YEL=$'\033[38;5;221m'
else
	BOLD=""; NC=""; BLUE=""; GREEN=""; RED=""; YEL=""
fi
info()  { echo -e "${BLUE}→${NC} $1"; }
ok()    { echo -e "${GREEN}✓${NC} $1"; }
warn()  { echo -e "${YEL}⚠${NC} $1"; }
err()   { echo -e "${RED}✗${NC} $1" >&2; }
pause() { read -rp "Press Enter to close this window… " _ 2>/dev/null || true; }
die()   { err "$1"; pause; exit 1; }

resolve_github_asset() {
	local body="$1"
	if command -v jq >/dev/null 2>&1; then
		printf '%s' "$body" | jq -r \
			'[.[] | select(.draft==false and .prerelease==false) | .assets[]?
			  | select(.name|test("'"$ASSET_RE_JQ"'")) | .browser_download_url] | .[0] // empty'
	elif command -v python3 >/dev/null 2>&1; then
		printf '%s' "$body" | python3 -c '
import json, re, sys
pattern = re.compile(sys.argv[1])
for release in json.load(sys.stdin):
    if release.get("draft") or release.get("prerelease"):
        continue
    for asset in release.get("assets") or []:
        if pattern.fullmatch(asset.get("name", "")):
            print(asset.get("browser_download_url", ""))
            raise SystemExit
' "$ASSET_RE_JQ"
	else
		printf '%s' "$body" \
			| grep -oE '"browser_download_url":[[:space:]]*"[^"]*'"$ASSET_RE_GREP" \
			| sed -E 's/.*"(https[^"]+)"$/\1/' | head -n1
	fi
}

resolve_mirror_entry() {
	local body="$1"
	if command -v jq >/dev/null 2>&1; then
		printf '%s' "$body" | jq -r \
			'if .schema == 1 then .components["slsteam-moon"] // {} else {} end
			 | [.url // "", .sha256 // ""] | @tsv'
	elif command -v python3 >/dev/null 2>&1; then
		printf '%s' "$body" | python3 -c '
import json, sys
data = json.load(sys.stdin)
entry = data.get("components", {}).get("slsteam-moon", {}) if data.get("schema") == 1 else {}
print("{}\t{}".format(entry.get("url", ""), entry.get("sha256", "")))
'
	else
		local compact object url sha
		compact="$(printf '%s' "$body" | tr -d '\r\n')"
		object="$(printf '%s' "$compact" | sed -nE \
			's/.*"slsteam-moon"[[:space:]]*:[[:space:]]*\{([^{}]*)\}.*/\1/p')"
		url="$(printf '%s' "$object" | sed -nE \
			's/.*"url"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p')"
		sha="$(printf '%s' "$object" | sed -nE \
			's/.*"sha256"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p')"
		printf '%s\t%s\n' "$url" "$sha"
	fi
}

valid_mirror_entry() {
	[[ "$1" =~ ^https://cdn\.jsdelivr\.net/gh/swwayps/jsdelivr@[0-9a-f]{40}/releases/slsteam-moon/.+[.]zip$ ]] \
		&& [[ "$2" =~ ^[0-9a-f]{64}$ ]]
}

if [ "${AUTOFIX_LIB_ONLY:-0}" = 1 ]; then
	return 0 2>/dev/null || exit 0
fi

echo -e "${BOLD}${BLUE}◯  slsteam-moon auto-fix${NC}"
echo

command -v curl >/dev/null 2>&1 || die "curl is required but not installed."

# ── 1. resolve the latest -lumen release asset ──────────────────────────────
info "Finding the latest slsteam-moon release"
API="https://api.github.com/repos/${REPO}/releases?per_page=50"
RELEASES_JSON="$(curl -fsSL --connect-timeout 15 --retry 3 --retry-delay 2 \
	-H 'Accept: application/vnd.github+json' "$API" 2>/dev/null || true)"
URL="$(resolve_github_asset "$RELEASES_JSON" 2>/dev/null || true)"

MIRROR_JSON="$(curl -fsSL --connect-timeout 15 --retry 3 --retry-delay 2 \
	-H 'Accept: application/json' "$MIRROR_MANIFEST" 2>/dev/null || true)"
MIRROR_ENTRY="$(resolve_mirror_entry "$MIRROR_JSON" 2>/dev/null || true)"
IFS=$'\t' read -r MIRROR_URL MIRROR_SHA <<< "$MIRROR_ENTRY"
if ! valid_mirror_entry "${MIRROR_URL:-}" "${MIRROR_SHA:-}"; then
	MIRROR_URL=""
	MIRROR_SHA=""
fi

EXPECTED_SHA=""
if [ -z "${URL:-}" ] && [ -n "$MIRROR_URL" ]; then
	URL="$MIRROR_URL"
	EXPECTED_SHA="$MIRROR_SHA"
fi
[ -n "${URL:-}" ] || die "Could not reach GitHub or the jsDelivr release mirror."
ok "Found: $URL"

# ── 2. download + extract ───────────────────────────────────────────────────
TMP="$(mktemp -d)" || die "Could not create a temp directory."
trap 'rm -rf "${TMP:-}"' EXIT
ZIP="$TMP/slsteam-moon.zip"

info "Downloading"
if ! curl -fL --connect-timeout 15 --retry 3 --retry-delay 2 "$URL" -o "$ZIP"; then
	if [ -z "$MIRROR_URL" ] || [ "$URL" = "$MIRROR_URL" ]; then
		die "Download failed on GitHub and the jsDelivr mirror."
	fi
	warn "GitHub download failed; trying jsDelivr"
	rm -f "$ZIP"
	curl -fL --connect-timeout 15 --retry 3 --retry-delay 2 \
		"$MIRROR_URL" -o "$ZIP" || die "Download failed on GitHub and the jsDelivr mirror."
	URL="$MIRROR_URL"
	EXPECTED_SHA="$MIRROR_SHA"
fi
if [ -n "$EXPECTED_SHA" ]; then
	printf '%s  %s\n' "$EXPECTED_SHA" "$ZIP" | sha256sum -c - >/dev/null \
		|| die "The mirrored archive failed its integrity check."
fi

info "Extracting"
if command -v unzip >/dev/null 2>&1; then
	unzip -qo "$ZIP" -d "$TMP/extracted" || die "Extraction failed."
elif command -v python3 >/dev/null 2>&1; then
	python3 - "$ZIP" "$TMP/extracted" <<'PY' || die "Extraction failed."
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "r") as zf:
    zf.extractall(sys.argv[2])
PY
else
	die "Neither unzip nor python3 is available to extract the archive."
fi

SETUP="$(find "$TMP/extracted" -maxdepth 2 -name setup.sh -type f | head -n1)"
[ -n "$SETUP" ] || die "setup.sh not found in the release archive."
EXTRACT_ROOT="$(dirname "$SETUP")"

# ── 3. install (stops Steam, patches every *steam*.desktop, sudo for system) ─
echo
info "Installing slsteam-moon (this stops Steam and may ask for your password)"
echo
chmod +x "$SETUP" 2>/dev/null || true
( cd "$EXTRACT_ROOT" && bash "$SETUP" install ) || die "slsteam-moon setup failed."
echo
ok "slsteam-moon installed"

# ── 4. relaunch Steam through the injected wrapper ──────────────────────────
STEAM_LAUNCH=""
if [ -x "$WRAPPER" ]; then
	STEAM_LAUNCH="$WRAPPER"
elif command -v steam >/dev/null 2>&1; then
	STEAM_LAUNCH="steam"   # PATH resolves to the wrapper after install
fi

if [ -z "$STEAM_LAUNCH" ]; then
	warn "Could not find the Steam launcher — start Steam yourself to finish."
elif [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
	warn "No graphical session detected — start Steam yourself to finish."
else
	info "Restarting Steam with slsteam-moon active"
	setsid nohup "$STEAM_LAUNCH" >/dev/null 2>&1 < /dev/null &
	ok "Steam is starting. Give it a moment to load."
fi

echo
ok "Done. Your LuaTools games should now show Install/Play instead of Buy."
pause
