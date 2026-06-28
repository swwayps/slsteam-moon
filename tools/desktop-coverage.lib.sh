# desktop-coverage.lib.sh — pure, sourceable helpers to find and patch every
# *steam*.desktop so it launches through the slsteam-moon wrapper, and to blind
# those patches against Steam's restore. No top-level side effects: only
# function defs + defaults, so it is safe to source from setup.sh, the wrapper,
# and tests. Callers set DC_TAG and WRAPPER (or accept the defaults below).
: "${DC_TAG:=X-SLSteamMoon-Patched=true}"
: "${WRAPPER:=$HOME/.local/share/SLSsteam/path/steam}"

# dc_classify <file> -> echoes one of: launcher | stub | patched | unrelated
# launcher  = a real Steam launcher entry we should patch
# stub      = the "Install Steam" installer entry (patch only when Steam present)
# patched   = already carries our tag
# unrelated = not a steam launcher we recognise
dc_classify() {
	f="$1"
	[ -f "$f" ] || { echo unrelated; return; }
	if grep -q "$DC_TAG" "$f" 2>/dev/null; then echo patched; return; fi
	if grep -q "^Name=Install Steam" "$f" 2>/dev/null; then echo stub; return; fi
	# A launcher: a primary Exec= whose launcher token is steam-ish. Match a
	# /steam launcher path or a bare `steam` token; reject things like
	# `/usr/bin/steamy` (no word boundary after steam).
	if grep -qiE '^Exec=([^=]* )?(/[^ ]*/)?steam( |$)' "$f" 2>/dev/null; then
		echo launcher; return
	fi
	echo unrelated
}

# dc_strip_preheader <file> — drop any line before the first [Desktop Entry]
# (e.g. Valve's `#!/usr/bin/env xdg-open`) so the entry is spec-valid and wins
# XDG precedence. No-op if it already starts with [Desktop Entry].
dc_strip_preheader() {
	f="$1"
	grep -q '^\[Desktop Entry\]' "$f" 2>/dev/null || return 0
	[ "$(head -1 "$f" 2>/dev/null)" = "[Desktop Entry]" ] && return 0
	tmp="$(mktemp)" || return 1
	awk 'seen{print;next} /^\[Desktop Entry\]/{seen=1;print}' "$f" > "$tmp" 2>/dev/null \
		&& cat "$tmp" > "$f"
	rm -f "$tmp"
}
