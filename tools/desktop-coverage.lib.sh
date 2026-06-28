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
	local f="$1"
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
	local f="$1" tmp
	grep -q '^\[Desktop Entry\]' "$f" 2>/dev/null || return 0
	[ "$(head -1 "$f" 2>/dev/null)" = "[Desktop Entry]" ] && return 0
	tmp="$(mktemp)" || return 1
	awk 'seen{print;next} /^\[Desktop Entry\]/{seen=1;print}' "$f" > "$tmp" 2>/dev/null \
		&& cat "$tmp" > "$f"
	rm -f "$tmp"
}

# dc_rewrite_exec <file> — rewrite EVERY Exec= line so the launcher token (first
# word that isn't `env` or a VAR=val assignment) becomes $WRAPPER, keeping args.
# Launcher-path-agnostic (/usr/games/steam, /usr/bin/steam, bare steam, env
# prefixes). awk avoids sed path-escaping pitfalls. Modifies in place.
dc_rewrite_exec() {
	local f="$1" tmp
	tmp="$(mktemp)" || return 1
	WRAPPER="$WRAPPER" awk '
		/^Exec=/ {
			rest = substr($0, 6); n = split(rest, t, " ")
			swapped = 0; out = "Exec="
			for (i = 1; i <= n; i++) {
				if (!swapped && t[i] != "env" && index(t[i], "=") == 0) {
					t[i] = ENVIRON["WRAPPER"]; swapped = 1
				}
				out = out t[i] (i < n ? " " : "")
			}
			print out; next
		}
		{ print }
	' "$f" > "$tmp" 2>/dev/null && cat "$tmp" > "$f"
	rm -f "$tmp"
}

# dc_patch_one <file> [sudo] — back up once, strip pre-header, rewrite Exec to
# the wrapper, drop a stale tag, insert the tag after [Desktop Entry], write back
# as a regular 0644 file (replacing a symlink). Only commits if an Exec now runs
# the wrapper, so we never tag a file we failed to rewrite. $2="sudo" for system
# files. Returns 0 on patch, 1 on no-op/failure.
dc_patch_one() {
	local f="$1" S="${2:-}" bak tmp
	bak="$f.slssteam-backup"
	[ -f "$f" ] || return 1
	[ -f "$bak" ] || $S cp -- "$f" "$bak" 2>/dev/null
	tmp="$(mktemp)" || return 1
	cat "$f" > "$tmp" 2>/dev/null
	dc_strip_preheader "$tmp"
	dc_rewrite_exec "$tmp"
	if ! grep -qF "Exec=$WRAPPER" "$tmp" 2>/dev/null; then rm -f "$tmp"; return 1; fi
	# drop stale tag, then insert one line after the first [Desktop Entry]
	grep -vxF "$DC_TAG" "$tmp" > "$tmp.2" 2>/dev/null && mv "$tmp.2" "$tmp"
	awk -v tag="$DC_TAG" '
		!done && /^\[Desktop Entry\]/ { print; print tag; done=1; next } { print }
	' "$tmp" > "$tmp.2" 2>/dev/null && mv "$tmp.2" "$tmp"
	$S cp --remove-destination -- "$tmp" "$f" 2>/dev/null
	$S chmod 0644 "$f" 2>/dev/null || true
	rm -f "$tmp"
	return 0
}

# dc_symlink_shortcut <shortcut> <menu_entry> — back up a real shortcut once,
# then replace it with a symlink to the patched menu entry. bin_steam.sh skips
# symlinks ([ ! -L ]), so Steam never restores the vanilla copy. No-op if it is
# already the symlink we want.
dc_symlink_shortcut() {
	local sc="$1" target="$2"
	[ -e "$target" ] || return 1
	if [ -L "$sc" ] && [ "$(readlink "$sc")" = "$target" ]; then return 0; fi
	[ -e "$sc" ] && [ ! -L "$sc" ] && [ ! -f "$sc.slssteam-backup" ] && cp -- "$sc" "$sc.slssteam-backup" 2>/dev/null
	mkdir -p "$(dirname "$sc")" 2>/dev/null
	ln -sfn "$target" "$sc" 2>/dev/null
}

# Overridable roots (tests inject fakes; real callers leave them at defaults).
: "${DC_HOME:=$HOME}"
: "${DC_SYS_APPS:=/usr/share/applications}"
: "${DC_SYS_AUTOSTART:=/etc/xdg/autostart}"
# Command used to write system-owned files. Default "sudo"; tests set it empty.
: "${DC_SUDO:=sudo}"

# dc_desktop_dir — honour XDG_DESKTOP_DIR from user-dirs.dirs, else ~/Desktop.
dc_desktop_dir() {
	local d="$DC_HOME/Desktop"
	if [ -f "$DC_HOME/.config/user-dirs.dirs" ]; then
		# shellcheck disable=SC1090
		. "$DC_HOME/.config/user-dirs.dirs" 2>/dev/null || true
		[ -n "${XDG_DESKTOP_DIR:-}" ] && d="$XDG_DESKTOP_DIR"
	fi
	echo "$d"
}

# dc_patch_glob <sudo> <dir> — patch every *steam*.desktop in <dir>: launchers
# always; the Install-Steam stub only when DC_STEAM_INSTALLED=1.
dc_patch_glob() {
	local S="$1" dir="$2" f
	[ -d "$dir" ] || return 0
	for f in "$dir"/*steam*.desktop; do
		[ -e "$f" ] || continue
		case "$(dc_classify "$f")" in
			launcher) dc_patch_one "$f" "$S" ;;
			stub) [ "${DC_STEAM_INSTALLED:-0}" = 1 ] && dc_patch_one "$f" "$S" ;;
			*) : ;;
		esac
	done
}

# dc_run [--user|--system] — patch all known *steam*.desktop locations. --user
# (default) does user-owned dirs only (no sudo). --system additionally patches
# the system menu dir + stub (caller must provide sudo rights). Always blinds the
# desktop shortcut via symlink to the user menu entry.
dc_run() {
	local mode="${1:---user}" menu
	menu="$DC_HOME/.local/share/applications/steam.desktop"
	dc_patch_glob "" "$DC_HOME/.local/share/applications"
	dc_patch_glob "" "$DC_HOME/.config/autostart"
	if [ "$mode" = "--system" ]; then
		dc_patch_glob "$DC_SUDO" "$DC_SYS_APPS"
		dc_patch_glob "$DC_SUDO" "$DC_SYS_AUTOSTART"
	fi
	[ -f "$menu" ] && dc_symlink_shortcut "$(dc_desktop_dir)/steam.desktop" "$menu"
	return 0
}
