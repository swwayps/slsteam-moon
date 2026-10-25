#!/usr/bin/env bash
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AUTOFIX_LIB_ONLY=1 source "$HERE/autofix.sh"

pass=0
fail=0
check() {
	if [ "$2" = "$3" ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		printf 'FAIL: %s (got %s, want %s)\n' "$1" "$2" "$3"
	fi
}

releases='[
  {"tag_name":"v2.9","draft":false,"prerelease":false,"assets":[
    {"name":"slsteam-moon-linux-2.9-lumen.zip.uploading","browser_download_url":"https://github.invalid/temp"}
  ]},
  {"tag_name":"v2.8","draft":false,"prerelease":false,"assets":[
    {"name":"slsteam-moon-linux-2.8-lumen.zip","browser_download_url":"https://github.invalid/v2.8.zip"}
  ]}
]'
check "temporary asset is ignored" "$(resolve_github_asset "$releases")" \
	"https://github.invalid/v2.8.zip"

manifest='{
  "schema": 1,
  "components": {
    "slsteam-moon": {
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "url": "https://cdn.jsdelivr.net/gh/swwayps/jsdelivr@0123456789012345678901234567890123456789/releases/slsteam-moon/v2.9/hash/slsteam-moon-linux-2.9-lumen.zip"
    }
  }
}'
entry="$(resolve_mirror_entry "$manifest")"
IFS=$'\t' read -r mirror_url mirror_sha <<< "$entry"
check "mirror URL parsed" "$mirror_url" \
	"https://cdn.jsdelivr.net/gh/swwayps/jsdelivr@0123456789012345678901234567890123456789/releases/slsteam-moon/v2.9/hash/slsteam-moon-linux-2.9-lumen.zip"
check "mirror digest parsed" "$mirror_sha" \
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
check "commit-pinned mirror entry accepted" \
	"$(valid_mirror_entry "$mirror_url" "$mirror_sha" && echo yes || echo no)" "yes"
check "branch-pinned mirror entry rejected" \
	"$(valid_mirror_entry "https://cdn.jsdelivr.net/gh/swwayps/jsdelivr@main/file.zip" "$mirror_sha" && echo yes || echo no)" "no"

# A GitHub download runs its setup.sh only after it matches the "<asset>.sha256"
# sidecar. Offline: curl is a stub serving the releases JSON, the archive and
# $STUB_SIDECAR, and refusing everything else (the mirror included).
work="$(mktemp -d)"
mkdir -p "$work/bin" "$work/pkg/slsteam-moon-2.8"
printf 'touch "$MARKER"\n' > "$work/pkg/slsteam-moon-2.8/setup.sh"
( cd "$work/pkg" && zip -qr "$work/asset.zip" . )
cat > "$work/bin/curl" <<'SH'
#!/bin/sh
url=; out=; next=
for a; do
	[ -n "$next" ] && { out="$a"; next=; continue; }
	case "$a" in -o) next=1 ;; https://*) url="$a" ;; esac
done
case "$url" in
	https://api.github.com/*) printf '%s' "$STUB_RELEASES" ;;
	*.zip) cp "$STUB_ZIP" "$out" ;;
	*.zip.sha256) [ -n "$STUB_SIDECAR" ] || exit 22; printf '%s\n' "$STUB_SIDECAR" ;;
	*) exit 22 ;;
esac
SH
chmod +x "$work/bin/curl"
run_autofix() {
	rm -f "$work/marker"
	env -u DISPLAY -u WAYLAND_DISPLAY HOME="$work" PATH="$work/bin:$PATH" \
		MARKER="$work/marker" STUB_ZIP="$work/asset.zip" STUB_SIDECAR="$1" \
		STUB_RELEASES='[{"draft":false,"prerelease":false,"assets":[{"name":"slsteam-moon-linux-2.8-lumen.zip","browser_download_url":"https://github.invalid/v2.8/slsteam-moon-linux-2.8-lumen.zip"}]}]' \
		bash "$HERE/autofix.sh" < /dev/null > "$work/log" 2>&1
	echo "rc=$? setup=$([ -e "$work/marker" ] && echo ran || echo skipped)"
}
digest="$(sha256sum "$work/asset.zip" | cut -d' ' -f1)"
check "matching sidecar installs" \
	"$(run_autofix "$digest  slsteam-moon-linux-2.8-lumen.zip")" "rc=0 setup=ran"
check "mismatched sidecar stops before setup" \
	"$(run_autofix "$(printf '%064d' 0)  slsteam-moon-linux-2.8-lumen.zip")" "rc=1 setup=skipped"
check "malformed sidecar stops before setup" "$(run_autofix "not a digest")" "rc=1 setup=skipped"
check "missing sidecar installs unverified" "$(run_autofix "")" "rc=0 setup=ran"
rm -rf "$work"

printf 'autofix release: %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
