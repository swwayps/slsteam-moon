# desktop-coverage.lib.sh — pure, sourceable helpers to find and patch every
# *steam*.desktop so it launches through the slsteam-moon wrapper, and to blind
# those patches against Steam's restore. No top-level side effects: only
# function defs + defaults, so it is safe to source from setup.sh, the wrapper,
# and tests. Callers set DC_TAG and WRAPPER (or accept the defaults below).
: "${DC_TAG:=X-SLSteamMoon-Patched=true}"
# Marks an entry we CREATED (a seeded autostart override) rather than patched in
# place. Such files get no backup and are DELETED (not restored) on uninstall,
# because the user never had them.
: "${DC_SEED_TAG:=X-SLSteamMoon-Seeded=true}"
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
	# A seeded override (we created it; the user had no such file) must never get
	# a backup, so a re-patch on a later run doesn't turn it into a "restore to
	# vanilla" on uninstall. dc_restore_one deletes seeded files outright.
	if ! grep -qxF "$DC_SEED_TAG" "$f" 2>/dev/null; then
		[ -f "$bak" ] || $S cp -- "$f" "$bak" 2>/dev/null
	fi
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

# dc_patch_shortcut <shortcut> — patch an EXISTING desktop shortcut in place as a
# regular, trusted, executable file so the DE renders it as "Steam". We do NOT
# create one where the user had none, and we do NOT use a symlink (GNOME shows a
# symlinked .desktop as the raw filename + an untrusted link emblem). Steam may
# restore a vanilla copy on a re-bootstrap; the per-launch/Lumen re-assert
# re-patches it then.
dc_patch_shortcut() {
	local sc="$1"
	[ -e "$sc" ] || return 0          # never create a shortcut the user lacked
	if [ -L "$sc" ]; then              # migrate a legacy symlink we may have made
		[ -f "$sc.slssteam-backup" ] && { rm -f "$sc"; cp -- "$sc.slssteam-backup" "$sc" 2>/dev/null; } || rm -f "$sc"
		[ -e "$sc" ] || return 0
	fi
	case "$(dc_classify "$sc")" in
		launcher|patched|stub)
			dc_patch_one "$sc"
			chmod 0755 "$sc" 2>/dev/null || true
			command -v gio >/dev/null 2>&1 && gio set "$sc" metadata::trusted true >/dev/null 2>&1 || true
			;;
	esac
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
		# A legacy root-owned 0711 entry (the old `chmod +x` bug) is unreadable by
		# us, so dc_classify would misread it as "unrelated" and skip the
		# migration. Make it readable first (we set 0644 anyway). With $S=sudo this
		# fixes a system entry; without sudo it only succeeds on our own files.
		[ -r "$f" ] || $S chmod 0644 "$f" 2>/dev/null
		case "$(dc_classify "$f")" in
			launcher) dc_patch_one "$f" "$S" ;;
			# Already tagged: re-run anyway so a legacy install is MIGRATED —
			# dc_patch_one is idempotent and (re)asserts 0644 + strips the Valve
			# shebang + keeps the wrapper Exec. This is what fixes the old 0711
			# entry (the Cinnamon "Steam vanished" bug) on an update.
			patched)  dc_patch_one "$f" "$S" ;;
			stub) [ "${DC_STEAM_INSTALLED:-0}" = 1 ] && dc_patch_one "$f" "$S" ;;
			*) : ;;
		esac
	done
}

# dc_seed_autostart_override — when the desktop session auto-launches Steam via a
# SYSTEM autostart entry (SteamOS/Bazzite: /etc/xdg/autostart/steam.desktop, often
# read-only) and the user has NO ~/.config/autostart/steam.desktop, seed a
# user-level override with the same basename. By XDG precedence it shadows the
# system entry, so the auto-launch runs through our wrapper. Pure HOME (no sudo),
# so it works on immutable distros. We ONLY seed from an existing system entry —
# never create autostart where the user (and system) had none, so normal desktops
# are unaffected. The seeded file gets NO backup, so dc_restore_one deletes it on
# uninstall (the user never had it) instead of leaving a vanilla copy behind.
dc_seed_autostart_override() {
	local user_as="$DC_HOME/.config/autostart/steam.desktop"
	local sys_as="$DC_SYS_AUTOSTART/steam.desktop"
	# A user entry already exists -> the normal autostart glob patches it in place.
	[ -e "$user_as" ] && return 0
	# Only seed from an existing SYSTEM autostart entry that launches Steam. The
	# match is loose (any Exec mentioning steam) so distro launchers like
	# bazzite-steam / steam-jupiter qualify; skip the "Install Steam" stub.
	[ -f "$sys_as" ] || return 0
	grep -q "^Name=Install Steam" "$sys_as" 2>/dev/null && return 0
	grep -qiE '^Exec=.*steam' "$sys_as" 2>/dev/null || return 0
	mkdir -p "$(dirname "$user_as")" 2>/dev/null || return 0
	cp -- "$sys_as" "$user_as" 2>/dev/null || return 0
	dc_patch_one "$user_as"
	rm -f "$user_as.slssteam-backup"   # seeded, not pre-existing -> restore deletes
	# Mark it seeded so re-patches never create a backup and uninstall deletes it.
	if ! grep -qxF "$DC_SEED_TAG" "$user_as" 2>/dev/null; then
		local tmp; tmp="$(mktemp)" || return 0
		awk -v s="$DC_SEED_TAG" '
			!d && /^\[Desktop Entry\]/ { print; print s; d=1; next } { print }
		' "$user_as" > "$tmp" 2>/dev/null && cat "$tmp" > "$user_as"
		rm -f "$tmp"
	fi
}

# dc_run [--user|--system] — patch all known *steam*.desktop locations. --user
# (default) does user-owned dirs only (no sudo). --system additionally patches
# the system menu dir + stub (caller must provide sudo rights). Always blinds the
# desktop shortcut via symlink to the user menu entry.
dc_run() {
	local mode="${1:---user}" menu
	menu="$DC_HOME/.local/share/applications/steam.desktop"
	dc_patch_glob "" "$DC_HOME/.local/share/applications"
	# Seed a user autostart override from the system entry (SteamOS/Bazzite) BEFORE
	# the autostart glob, so a freshly seeded file is (idempotently) re-patched too.
	dc_seed_autostart_override
	dc_patch_glob "" "$DC_HOME/.config/autostart"
	if [ "$mode" = "--system" ]; then
		dc_patch_glob "$DC_SUDO" "$DC_SYS_APPS"
		dc_patch_glob "$DC_SUDO" "$DC_SYS_AUTOSTART"
	fi
	[ -f "$menu" ] && dc_patch_shortcut "$(dc_desktop_dir)/steam.desktop"
	return 0
}

# dc_restore_one <file> [sudo] — restore from <file>.slssteam-backup if present
# (authoritative even for a legacy unreadable 0711 entry), 0644; else if it
# carries our tag, remove it. A symlink we made is removed and, if a backup
# exists, that backup is restored as the original regular file.
dc_restore_one() {
	local f="$1" S="${2:-}" bak
	bak="$f.slssteam-backup"
	# A seeded override was created by us — the user never had it — so remove it
	# (and any stray backup) rather than restoring a vanilla copy.
	if [ -f "$f" ] && grep -qxF "$DC_SEED_TAG" "$f" 2>/dev/null; then
		$S rm -f -- "$f" "$bak" 2>/dev/null
		return 0
	fi
	if [ -L "$f" ]; then
		$S rm -f -- "$f" 2>/dev/null
		if [ -f "$bak" ]; then $S cp -- "$bak" "$f" 2>/dev/null; $S rm -f -- "$bak" 2>/dev/null; fi
		return 0
	fi
	if [ -f "$bak" ]; then
		$S cp --remove-destination -- "$bak" "$f" 2>/dev/null
		$S chmod 0644 "$f" 2>/dev/null || true
		$S rm -f -- "$bak" 2>/dev/null
		return 0
	fi
	[ -f "$f" ] && grep -q "$DC_TAG" "$f" 2>/dev/null && $S rm -f -- "$f" 2>/dev/null
	return 0
}

# dc_restore_all — reverse dc_run across the same locations (user dirs without
# sudo, system dirs with $DC_SUDO).
dc_restore_all() {
	local d f
	for d in "$DC_HOME/.local/share/applications" "$DC_HOME/.config/autostart"; do
		[ -d "$d" ] || continue
		for f in "$d"/*steam*.desktop; do [ -e "$f" ] || [ -L "$f" ] || continue; dc_restore_one "$f"; done
	done
	dc_restore_one "$(dc_desktop_dir)/steam.desktop"
	for d in "$DC_SYS_APPS" "$DC_SYS_AUTOSTART"; do
		[ -d "$d" ] || continue
		for f in "$d"/*steam*.desktop; do [ -e "$f" ] || [ -L "$f" ] || continue; dc_restore_one "$f" "$DC_SUDO"; done
	done
}
