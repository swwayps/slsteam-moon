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
: "${DC_HOME:=$HOME}"

# Effective XDG roots. DC_HOME is injectable so callers and tests can operate on
# another user's tree without changing the process HOME.
dc_data_home() {
	case "${XDG_DATA_HOME:-}" in
		/*) printf '%s\n' "$XDG_DATA_HOME" ;;
		*) printf '%s\n' "$DC_HOME/.local/share" ;;
	esac
}

dc_config_home() {
	case "${XDG_CONFIG_HOME:-}" in
		/*) printf '%s\n' "$XDG_CONFIG_HOME" ;;
		*) printf '%s\n' "$DC_HOME/.config" ;;
	esac
}

# dc_colon_dirs <value> <default> <suffix> — print one suffixed path per
# non-empty colon-delimited root, using <default> when <value> is empty.
dc_colon_dirs() {
	local value="$1" def="$2" suffix="$3" d
	[ -n "$value" ] || value="$def"
	while [ -n "$value" ]; do
		case "$value" in
			*:*) d="${value%%:*}"; value="${value#*:}" ;;
			*) d="$value"; value= ;;
		esac
		case "$d" in
			/*) printf '%s/%s\n' "${d%/}" "$suffix" ;;
		esac
	done
}

dc_application_dirs() {
	printf '%s/applications\n' "$(dc_data_home)"
	dc_colon_dirs "${XDG_DATA_DIRS:-}" '/usr/local/share:/usr/share' applications
}

# Same-ID application shadows always live in the effective user data root.
dc_user_app_dir() {
	printf '%s/applications\n' "$(dc_data_home)"
}

# A desktop-file ID is its exact basename, including original letter case.
dc_desktop_id() {
	basename -- "$1"
}

dc_autostart_dirs() {
	printf '%s/autostart\n' "$(dc_config_home)"
	dc_colon_dirs "${XDG_CONFIG_DIRS:-}" '/etc/xdg' autostart
}

# Empty means "$DC_HOME/.local/share/SLSsteam/backup". Tests may override this
# without changing HOME. The mirrored absolute source path below avoids name
# collisions between (for example) user and system steam.desktop files.
: "${DC_BACKUP_ROOT:=}"

dc_backup_root() {
	printf '%s\n' "${DC_BACKUP_ROOT:-$DC_HOME/.local/share/SLSsteam/backup}"
}

# dc_backup_path <original> — central backup path, mirroring the absolute
# original underneath SLSsteam/backup.
dc_backup_path() {
	local f="$1" rel
	case "$f" in /*) rel="${f#/}" ;; *) rel="$f" ;; esac
	printf '%s/%s\n' "$(dc_backup_root)" "$rel"
}

# Copy an original/legacy backup into the central store without ever replacing
# a backup already captured by an earlier run. $3 is "sudo" for a source that
# needs root to read; redirection remains user-owned.
dc_store_backup() {
	local src="$1" original="$2" S="${3:-}" bak
	bak="$(dc_backup_path "$original")"
	[ -f "$bak" ] && return 0
	mkdir -p "$(dirname "$bak")" 2>/dev/null || return 1
	if [ -n "$S" ]; then
		$S cat -- "$src" > "$bak" 2>/dev/null || { rm -f "$bak"; return 1; }
	else
		cp -- "$src" "$bak" 2>/dev/null || { rm -f "$bak"; return 1; }
	fi
	chmod 0644 "$bak" 2>/dev/null || true
}

# dc_migrate_legacy_file <legacy-backup> [sudo] — move one adjacent backup to
# the central mirror. The adjacent file is removed only after its contents are
# safely present centrally.
dc_migrate_legacy_file() {
	local legacy="$1" S="${2:-}" original
	[ -f "$legacy" ] || return 0
	case "$legacy" in
		*.slssteam-backup) original="${legacy%.slssteam-backup}" ;;
		*.slsteam-bak)    original="${legacy%.slsteam-bak}" ;;
		*) return 0 ;;
	esac
	dc_store_backup "$legacy" "$original" "$S" || return 1
	$S rm -f -- "$legacy" 2>/dev/null || return 1
}

dc_migrate_legacy_dir() {
	local dir="$1" S="${2:-}" legacy failed=0
	[ -d "$dir" ] || return 0
	for legacy in \
		"$dir"/*steam*.desktop.slssteam-backup \
		"$dir"/*steam*.desktop.slsteam-bak; do
		[ -f "$legacy" ] || continue
		dc_migrate_legacy_file "$legacy" "$S" || failed=1
	done
	[ "$failed" = 0 ]
}

# dc_migrate_legacy_backups [--user|--system] — runs BEFORE repatching. It also
# catches the critical case where steam.desktop was deleted when autostart was
# disabled but steam.desktop.slssteam-backup was left behind and executable by
# KDE/systemd's XDG autostart generator.
dc_migrate_legacy_backups() {
	local mode="${1:---user}" desktop failed=0
	mkdir -p "$(dc_backup_root)" 2>/dev/null || return 1
	desktop="$(dc_desktop_dir)"
	dc_migrate_legacy_dir "$(dc_data_home)/applications" || failed=1
	dc_migrate_legacy_dir "$(dc_config_home)/autostart" || failed=1
	dc_migrate_legacy_dir "$desktop" || failed=1
	if [ "$mode" = "--system" ]; then
		dc_migrate_legacy_dir "$DC_SYS_APPS" "$DC_SUDO" || failed=1
		dc_migrate_legacy_dir "$DC_SYS_AUTOSTART" "$DC_SUDO" || failed=1
	fi
	[ "$failed" = 0 ]
}

# _dc_exec_scan <mode> <exec-text> — deterministic Desktop Entry Exec scanner.
# It decodes double quotes/backslash escapes while retaining the launcher's raw
# byte boundaries. Modes: supported (status only), rewrite (print replacement),
# wrapper (status only when the decoded executable is exactly $WRAPPER), parts
# (print raw prefix, decoded launcher, and raw suffix on separate lines).
_dc_exec_scan() {
	local mode="$1" exec_text="$2"
	printf '%s\n' "$exec_text" | DC_EXEC_WRAPPER="$WRAPPER" awk -v mode="$mode" '
		function next_char(    esc) {
			if (pos > length(text)) return 0
			ch = substr(text, pos, 1)
			if (ch != "\\") { pos++; return 1 }
			if (pos == length(text)) return -1
			esc = substr(text, pos + 1, 1)
			if (esc == "s") ch = " "
			else if (esc == "n") ch = "\n"
			else if (esc == "t") ch = "\t"
			else if (esc == "r") ch = "\r"
			else if (esc == "\\") ch = "\\"
			else return -1
			pos += 2
			return 1
		}
		function is_space(c) {
			return c == " " || c == "\t" || c == "\n" || c == "\r"
		}
		function scan_token(    before, c, escaped, result, saved) {
			while (pos <= length(text)) {
				before = pos; result = next_char()
				if (result < 0) return -1
				if (!is_space(ch)) { pos = before; break }
			}
			if (pos > length(text)) return 0
			count++; start[count] = pos; decoded[count] = ""; quoted = 0
			while (pos <= length(text)) {
				before = pos; result = next_char()
				if (result <= 0) return -1
				c = ch
				if (quoted && c == "\\") {
					result = next_char()
					if (result <= 0) return -1
					escaped = ch
					if (escaped != "\"" && escaped != "`" && escaped != "$" && escaped != "\\")
						return -1
					decoded[count] = decoded[count] escaped
					continue
				}
				if (c == "\"") { quoted = !quoted; continue }
				if (!quoted && is_space(c)) { pos = before; break }
				if (c == "%") {
					saved = pos
					if (next_char() > 0 && ch == "%") {
						decoded[count] = decoded[count] "%"
						continue
					}
					pos = saved
				}
				decoded[count] = decoded[count] c
			}
			if (quoted) return -1
			finish[count] = pos - 1
			return 1
		}
		function needs_quotes(value) {
			return value ~ /[ \t\n\r"\\><~|&;$*?#()`]/ || index(value, sprintf("%c", 39))
		}
		function encode_token(value,    c, i, encoded, quote) {
			quote = needs_quotes(value)
			encoded = quote ? "\"" : ""
			for (i = 1; i <= length(value); i++) {
				c = substr(value, i, 1)
				if (c == "%") encoded = encoded "%%"
				else if (c == "\n") encoded = encoded "\\n"
				else if (c == "\t") encoded = encoded "\\t"
				else if (c == "\r") encoded = encoded "\\r"
				else if (c == "\\") encoded = encoded "\\\\\\\\"
				else if (quote && (c == "\"" || c == "`" || c == "$"))
					encoded = encoded "\\\\" c
				else encoded = encoded c
			}
			return quote ? encoded "\"" : encoded
		}
		BEGIN {
			if ((getline text) <= 0) exit 1
			wrapper = ENVIRON["DC_EXEC_WRAPPER"]
			pos = 1; count = 0
			while ((result = scan_token()) > 0) { }
			if (result < 0 || count == 0) exit 1
			if (mode == "syntax") exit 0

			launcher = 1
			if (decoded[1] == "env") {
				launcher = 2
				while (launcher <= count && decoded[launcher] ~ /^[A-Za-z_][A-Za-z0-9_]*=/)
					launcher++
			}
			if (launcher > count) exit 1
			launcher_path = decoded[launcher]
			if (index(launcher_path, "/") && substr(launcher_path, 1, 1) != "/") exit 1
			base = launcher_path
			sub(/^.*\//, "", base)
			if (base != "steam" && base != "bazzite-steam" && base != "steam-jupiter")
				exit 1

			prefix = substr(text, 1, start[launcher] - 1)
			suffix = substr(text, finish[launcher] + 1)
			if (mode == "supported") exit 0
			if (mode == "wrapper") exit(decoded[launcher] == wrapper ? 0 : 1)
			if (mode == "rewrite") { print prefix encode_token(wrapper) suffix; exit 0 }
			if (mode == "parts") {
				print prefix; print decoded[launcher]; print suffix; exit 0
			}
			exit 1
		}
	'
}

# dc_exec_supported_launcher <exec-text> — true only for the three native Steam
# launcher basenames, optionally after `env` and valid variable assignments.
dc_exec_supported_launcher() {
	_dc_exec_scan supported "$1" >/dev/null
}

# dc_rewrite_exec_line <exec-text> — print one safely rewritten command while
# preserving everything outside the raw launcher token.
dc_rewrite_exec_line() {
	_dc_exec_scan rewrite "$1"
}

# dc_file_has_wrapper_exec <desktop-file> — true when an Exec line tokenizes to
# the exact wrapper path, including commands with a valid env prefix.
dc_file_has_wrapper_exec() {
	local f="$1" line
	[ -f "$f" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			Exec=*) _dc_exec_scan wrapper "${line#Exec=}" >/dev/null && return 0 ;;
		esac
	done < "$f"
	return 1
}

# _dc_file_exec_syntax_valid <desktop-file> — reject the whole entry before any
# mutation when an Exec line contains malformed or unterminated quoting.
_dc_file_exec_syntax_valid() {
	local f="$1" line
	[ -f "$f" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			Exec=*) _dc_exec_scan syntax "${line#Exec=}" >/dev/null || return 1 ;;
		esac
	done < "$f"
}

# dc_classify <file> -> echoes one of: launcher | stub | patched | unrelated
# launcher  = a real Steam launcher entry we should patch
# stub      = the "Install Steam" installer entry (patch only when Steam present)
# patched   = already carries our tag
# unrelated = not a steam launcher we recognise
dc_classify() {
	local f="$1" line
	[ -f "$f" ] || { echo unrelated; return; }
	if grep -q "$DC_TAG" "$f" 2>/dev/null; then echo patched; return; fi
	if grep -q "^Name=Install Steam" "$f" 2>/dev/null; then echo stub; return; fi
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			Exec=*)
				if dc_exec_supported_launcher "${line#Exec=}"; then
					echo launcher
					return
				fi
				;;
		esac
	done < "$f"
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

# dc_rewrite_exec <file> — rewrite only supported Exec lines, including every
# applicable Desktop Action. Unsupported or malformed lines remain byte-for-byte
# unchanged. The file is updated only after at least one line is rewritten and
# every replacement tokenizes back to the exact wrapper path.
dc_rewrite_exec() {
	local f="$1" tmp line exec_text rewritten changed=0 valid=1
	_dc_file_exec_syntax_valid "$f" || return 1
	tmp="$(mktemp)" || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			Exec=*)
				exec_text="${line#Exec=}"
				if rewritten="$(dc_rewrite_exec_line "$exec_text")"; then
					if _dc_exec_scan wrapper "$rewritten" >/dev/null; then
						printf 'Exec=%s\n' "$rewritten" >> "$tmp"
						changed=1
						continue
					fi
					valid=0
					break
				fi
				;;
		esac
		printf '%s\n' "$line" >> "$tmp"
	done < "$f"
	if [ "$changed" = 1 ] && [ "$valid" = 1 ]; then
		cat "$tmp" > "$f"
		valid=$?
	else
		valid=1
	fi
	rm -f "$tmp"
	return "$valid"
}

# dc_rewrite_installed_stub_exec <file> — Debian/Ubuntu's steam-installer
# desktop entry remains named "Install Steam" after Steam is bootstrapped and
# launches through `sh -c 'STEAM_FRAME_FORCE_CLOSE=1 steam %U'`. Keep generic
# shell wrappers unsupported, but convert this explicitly-classified stub into a
# direct, parser-safe wrapper launch. Preserve the distro's force-close setting.
dc_rewrite_installed_stub_exec() {
	local f="$1" tmp line in_entry=0 replaced=0 template rewritten
	tmp="$(mktemp)" || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			'[Desktop Entry]') in_entry=1 ;;
			'['*']') [ "$in_entry" = 1 ] && in_entry=0 ;;
			Exec=*)
				if [ "$in_entry" = 1 ] && [ "$replaced" = 0 ]; then
					case "${line#Exec=}" in
						*STEAM_FRAME_FORCE_CLOSE=1*)
							template='env STEAM_FRAME_FORCE_CLOSE=1 steam %U'
							;;
						*) template='steam %U' ;;
					esac
					if ! rewritten="$(dc_rewrite_exec_line "$template")" \
					   || ! _dc_exec_scan wrapper "$rewritten" >/dev/null; then
						rm -f "$tmp"
						return 1
					fi
					printf 'Exec=%s\n' "$rewritten" >> "$tmp"
					replaced=1
					continue
				fi
				;;
		esac
		printf '%s\n' "$line" >> "$tmp"
	done < "$f"
	if [ "$replaced" = 1 ] && cat "$tmp" > "$f"; then
		rm -f "$tmp"
		return 0
	fi
	rm -f "$tmp"
	return 1
}

# dc_patch_one <file> [sudo] — back up once, strip pre-header, rewrite Exec to
# the wrapper, drop a stale tag, insert the tag after [Desktop Entry], write back
# as a regular 0644 file (replacing a symlink). Only commits if an Exec now runs
# the wrapper, so we never tag a file we failed to rewrite. $2="sudo" for system
# files. Returns 0 on patch, 1 on no-op/failure.
dc_patch_one() {
	local f="$1" S="${2:-}" bak tmp kind
	bak="$(dc_backup_path "$f")"
	[ -f "$f" ] || return 1
	_dc_file_exec_syntax_valid "$f" || return 1
	kind="$(dc_classify "$f")"
	# A seeded override (we created it; the user had no such file) must never get
	# a backup, so a re-patch on a later run doesn't turn it into a "restore to
	# vanilla" on uninstall. dc_restore_one deletes seeded files outright.
	# Likewise, if an old installation already left the active entry patched but
	# lost its original, never record that patched file as the "original".
	if ! grep -qxF "$DC_SEED_TAG" "$f" 2>/dev/null \
	   && ! grep -qxF "$DC_TAG" "$f" 2>/dev/null; then
		dc_store_backup "$f" "$f" "$S" || return 1
	fi
	tmp="$(mktemp)" || return 1
	cat "$f" > "$tmp" 2>/dev/null
	dc_strip_preheader "$tmp"
	if [ "$kind" = stub ] && ! dc_rewrite_installed_stub_exec "$tmp"; then
		rm -f "$tmp"
		return 1
	fi
	if ! dc_rewrite_exec "$tmp" || ! dc_file_has_wrapper_exec "$tmp"; then
		rm -f "$tmp"
		return 1
	fi
	# drop stale tag, then insert one line after the first [Desktop Entry]
	grep -vxF "$DC_TAG" "$tmp" > "$tmp.2" 2>/dev/null && mv "$tmp.2" "$tmp"
	awk -v tag="$DC_TAG" '
		!done && /^\[Desktop Entry\]/ { print; print tag; done=1; next } { print }
	' "$tmp" > "$tmp.2" 2>/dev/null && mv "$tmp.2" "$tmp"
	# Preserve inode/mtime on converged regular files. Symlinks are still
	# replaced so managed entries remain ordinary files.
	if [ ! -L "$f" ] && cmp -s "$tmp" "$f" 2>/dev/null; then
		$S chmod 0644 "$f" 2>/dev/null || true
		rm -f "$tmp"
		return 0
	fi
	if ! $S cp --remove-destination -- "$tmp" "$f" 2>/dev/null; then
		rm -f "$tmp"
		return 1
	fi
	if ! $S chmod 0644 "$f" 2>/dev/null; then
		rm -f "$tmp"
		return 1
	fi
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
	local sc="$1" bak
	[ -e "$sc" ] || return 0          # never create a shortcut the user lacked
	if [ -L "$sc" ]; then              # migrate a legacy symlink we may have made
		bak="$(dc_backup_path "$sc")"
		[ -f "$bak" ] && { rm -f "$sc"; cp -- "$bak" "$sc" 2>/dev/null; } || rm -f "$sc"
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
: "${DC_SYS_APPS:=/usr/share/applications}"
: "${DC_SYS_AUTOSTART:=/etc/xdg/autostart}"
# Command used to write system-owned files. Default "sudo"; tests set it empty.
: "${DC_SUDO:=sudo}"

# dc_desktop_dir — honour XDG_DESKTOP_DIR from user-dirs.dirs, else ~/Desktop.
# Parse the one supported assignment without sourcing the file. Only literal
# absolute paths and the standard $HOME/${HOME} prefixes are accepted.
dc_desktop_dir() {
	local d="$DC_HOME/Desktop" file line value
	file="$(dc_config_home)/user-dirs.dirs"
	[ -f "$file" ] || { printf '%s\n' "$d"; return; }
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			XDG_DESKTOP_DIR=\"*\") value="${line#XDG_DESKTOP_DIR=\"}"; value="${value%\"}" ;;
			*) continue ;;
		esac
		case "$value" in
			'$HOME') d="$DC_HOME" ;;
			'$HOME/'*) d="$DC_HOME/${value#\$HOME/}" ;;
			'${HOME}') d="$DC_HOME" ;;
			'${HOME}/'*) d="$DC_HOME/${value#\$\{HOME\}/}" ;;
			/*) d="$value" ;;
		esac
		break
	done < "$file"
	printf '%s\n' "$d"
}

# dc_patch_glob <sudo> <dir> — patch every *steam*.desktop in <dir>: launchers
# always; the Install-Steam stub only when DC_STEAM_INSTALLED=1.
dc_patch_glob() {
	local S="$1" dir="$2" f failed=0
	[ -d "$dir" ] || return 0
	for f in "$dir"/*steam*.desktop; do
		[ -e "$f" ] || continue
		# A legacy root-owned 0711 entry (the old `chmod +x` bug) is unreadable by
		# us, so dc_classify would misread it as "unrelated" and skip the
		# migration. Make it readable first (we set 0644 anyway). With $S=sudo this
		# fixes a system entry; without sudo it only succeeds on our own files.
		[ -r "$f" ] || $S chmod 0644 "$f" 2>/dev/null || failed=1
		case "$(dc_classify "$f")" in
			launcher) dc_patch_one "$f" "$S" || failed=1 ;;
			# Already tagged: re-run anyway so a legacy install is MIGRATED —
			# dc_patch_one is idempotent and (re)asserts 0644 + strips the Valve
			# shebang + keeps the wrapper Exec. This is what fixes the old 0711
			# entry (the Cinnamon "Steam vanished" bug) on an update.
			patched) dc_patch_one "$f" "$S" || failed=1 ;;
			stub)
				if [ "${DC_STEAM_INSTALLED:-0}" = 1 ]; then
					dc_patch_one "$f" "$S" || failed=1
				fi
				;;
		esac
	done
	[ "$failed" = 0 ]
}

# _dc_application_donor_kind <file> — classify only parser-eligible desktop
# application donors. Filename/name text alone is never sufficient.
_dc_application_donor_kind() {
	local f="$1" line in_entry=0 supported=0 has_primary_exec=0 entry_name=''
	[ -f "$f" ] || { printf '%s\n' unrelated; return; }
	_dc_file_exec_syntax_valid "$f" \
		|| { printf '%s\n' unrelated; return; }
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			'[Desktop Entry]') in_entry=1 ;;
			'['*']') [ "$in_entry" = 1 ] && break ;;
			Name=*) [ "$in_entry" = 1 ] && entry_name="${line#Name=}" ;;
			Exec=*)
				if [ "$in_entry" = 1 ]; then
					has_primary_exec=1
					if dc_exec_supported_launcher "${line#Exec=}"; then
						supported=1
					fi
				fi
				;;
		esac
	done < "$f"
	# The exact installer identity is authoritative for Debian/Ubuntu's shell
	# stub. It is eligible only after Steam installation has been detected by the
	# caller; seeding publishes a minimal direct launcher rather than copying it.
	if [ "$entry_name" = 'Install Steam' ] && [ "$has_primary_exec" = 1 ]; then
		printf '%s\n' stub
		return
	fi
	[ "$supported" = 1 ] || { printf '%s\n' unrelated; return; }
	printf '%s\n' launcher
}

# _dc_file_has_primary_wrapper_exec <file> — validate the effective launcher of
# the main Desktop Entry, not merely a wrapper-backed Desktop Action.
_dc_file_has_primary_wrapper_exec() {
	local f="$1" line in_entry=0
	[ -f "$f" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			'[Desktop Entry]') in_entry=1 ;;
			'['*']') [ "$in_entry" = 1 ] && return 1 ;;
			Exec=*)
				[ "$in_entry" = 1 ] || continue
				_dc_exec_scan wrapper "${line#Exec=}" >/dev/null
				return $?
				;;
		esac
	done < "$f"
	return 1
}

# _dc_application_source <path> — true for a regular desktop file, with the
# extension matched case-insensitively. Content eligibility is checked later.
_dc_application_source() {
	local name
	[ -f "$1" ] || return 1
	name="$(dc_desktop_id "$1" | tr '[:upper:]' '[:lower:]')"
	case "$name" in *.desktop) return 0 ;; *) return 1 ;; esac
}

# _dc_publish_application_shadow <source> <target> <launcher|stub> — prepare the
# complete shadow in its destination directory, mark ownership before rewrite,
# validate through dc_patch_one, then publish by same-directory atomic rename.
_dc_publish_application_shadow() {
	local source="$1" target="$2" kind="$3" dir tmp tagged
	dir="$(dirname "$target")"
	mkdir -p "$dir" 2>/dev/null || return 1
	tmp="$(mktemp "$dir/.slsteam-shadow.XXXXXX")" || return 1
	if [ "$kind" = stub ]; then
		cat > "$tmp" <<EOF
[Desktop Entry]
$DC_SEED_TAG
Name=Steam
Comment=Application for managing and playing games on Steam
Exec=steam %U
Icon=steam
Terminal=false
Type=Application
Categories=Network;FileTransfer;Game;
MimeType=x-scheme-handler/steam;x-scheme-handler/steamlink;
PrefersNonDefaultGPU=true
EOF
	else
		cp -- "$source" "$tmp" 2>/dev/null || { rm -f -- "$tmp"; return 1; }
		tagged="$tmp.seed"
		awk -v seed="$DC_SEED_TAG" '
			$0 == seed { next }
			!done && /^\[Desktop Entry\]$/ { print; print seed; done=1; next }
			{ print }
		' "$tmp" > "$tagged" 2>/dev/null \
			&& mv -- "$tagged" "$tmp"
		if ! grep -qxF "$DC_SEED_TAG" "$tmp" 2>/dev/null; then
			rm -f -- "$tagged" "$tmp"
			return 1
		fi
	fi
	# The seed tag is already present, so dc_patch_one cannot capture this
	# temporary donor as a user original.
	if ! dc_patch_one "$tmp" || ! _dc_file_has_primary_wrapper_exec "$tmp"; then
		rm -f -- "$tmp" "$(dc_backup_path "$tmp")"
		return 1
	fi
	chmod 0644 "$tmp" 2>/dev/null || { rm -f -- "$tmp"; return 1; }
	# `mv -n` performs the same-directory rename without replacing a user entry
	# that appeared concurrently. A retained temp means the exact-ID target won
	# the race and must follow the pre-existing backup/patch path instead.
	mv -n -- "$tmp" "$target" 2>/dev/null || true
	if [ ! -e "$tmp" ]; then
		return 0
	fi
	if [ -e "$target" ] || [ -L "$target" ]; then
		rm -f -- "$tmp"
		[ -r "$target" ] || chmod 0644 "$target" 2>/dev/null || true
		dc_patch_one "$target" 2>/dev/null
		return $?
	fi
	rm -f -- "$tmp"
	return 1
}

# dc_seed_application_shadows — discover supported donors in every system XDG
# application layer. The first real donor for an ID wins; installer stubs are
# globally ignored when any real donor exists. Existing exact-ID user entries
# retain their metadata and are backed up/patched by dc_patch_one.
dc_seed_application_shadows() {
	local user_dir dir source kind id target have_real=0 seen=''
	user_dir="$(dc_user_app_dir)"

	while IFS= read -r dir; do
		[ "$dir" = "$user_dir" ] && continue
		[ -d "$dir" ] || continue
		for source in "$dir"/*; do
			_dc_application_source "$source" || continue
			[ "$(_dc_application_donor_kind "$source")" = launcher ] \
				&& have_real=1
		done
	done < <(dc_application_dirs)

	while IFS= read -r dir; do
		[ "$dir" = "$user_dir" ] && continue
		[ -d "$dir" ] || continue
		for source in "$dir"/*; do
			_dc_application_source "$source" || continue
			kind="$(_dc_application_donor_kind "$source")"
			case "$kind" in
				launcher) : ;;
				stub)
					[ "$have_real" = 0 ] \
						&& [ "${DC_STEAM_INSTALLED:-0}" = 1 ] || continue
					;;
				*) continue ;;
			esac
			id="$(dc_desktop_id "$source")"
			printf '%s\n' "$seen" | grep -qxF "$id" 2>/dev/null && continue
			seen="${seen}${seen:+
}$id"
			target="$user_dir/$id"
			if [ -e "$target" ] || [ -L "$target" ]; then
				[ -r "$target" ] || chmod 0644 "$target" 2>/dev/null || true
				dc_patch_one "$target" 2>/dev/null || true
			else
				_dc_publish_application_shadow "$source" "$target" "$kind" || true
			fi
		done
	done < <(dc_application_dirs)
	return 0
}

# Reconcile same-ID shadows before the existing broad user application pass.
dc_reconcile_application_entries() {
	dc_seed_application_shadows
	dc_patch_glob "" "$(dc_user_app_dir)"
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
	local user_as="$(dc_config_home)/autostart/steam.desktop"
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
	# Mark it seeded BEFORE patching so dc_patch_one never captures this
	# just-created file as an original.
	if ! grep -qxF "$DC_SEED_TAG" "$user_as" 2>/dev/null; then
		local tmp; tmp="$(mktemp)" || return 0
		awk -v s="$DC_SEED_TAG" '
			!d && /^\[Desktop Entry\]/ { print; print s; d=1; next } { print }
		' "$user_as" > "$tmp" 2>/dev/null && cat "$tmp" > "$user_as"
		rm -f "$tmp"
	fi
	dc_patch_one "$user_as"
	# Defensive cleanup for an interrupted older seeding implementation.
	rm -f "$user_as.slssteam-backup" "$user_as.slsteam-bak" "$(dc_backup_path "$user_as")"
}

# Guardian commands are injectable so tests can observe lock/cache behavior.
: "${DC_FLOCK:=flock}"
: "${DC_UPDATE_DESKTOP_DATABASE:=update-desktop-database}"
: "${DC_KBUILDSYCOCA:=}"

# Guardian mutation results are deliberately distinct from the legacy best-effort
# helpers: 0=changed, 1=unchanged/skipped, 2=failure.
dc_guardian_reset_results() {
	DC_EXAMINED=0
	DC_CHANGED=0
	DC_APP_CHANGED=0
	DC_AUTOSTART_CHANGED=0
	DC_SKIPPED=0
	DC_FAILED=0
}

dc_guardian_record_result() {
	local scope="$1" result="$2"
	DC_EXAMINED=$((DC_EXAMINED + 1))
	case "$result" in
		0)
			DC_CHANGED=$((DC_CHANGED + 1))
			case "$scope" in
				app) DC_APP_CHANGED=$((DC_APP_CHANGED + 1)) ;;
				autostart) DC_AUTOSTART_CHANGED=$((DC_AUTOSTART_CHANGED + 1)) ;;
			esac
			;;
		1) DC_SKIPPED=$((DC_SKIPPED + 1)) ;;
		*) DC_FAILED=$((DC_FAILED + 1)) ;;
	esac
}

# Include content, type, and mode so convergence does not miss permission or
# symlink repairs. The value is used only for before/after comparison.
dc_guardian_fingerprint() {
	local f="$1" kind mode sum
	if [ -L "$f" ]; then kind="link:$(readlink "$f" 2>/dev/null || true)"
	elif [ -f "$f" ]; then kind=file
	else printf '%s\n' missing; return
	fi
	mode="$(stat -c '%a' "$f" 2>/dev/null || printf '?')"
	sum="$(cksum < "$f" 2>/dev/null || printf '?')"
	printf '%s|%s|%s\n' "$kind" "$mode" "$sum"
}

# Patch one mandatory user entry and translate legacy dc_patch_one success into
# the guardian's changed/unchanged/failure contract.
dc_guardian_patch_file() {
	local f="$1" before after
	[ -f "$f" ] || return 1
	before="$(dc_guardian_fingerprint "$f")"
	dc_patch_one "$f" || return 2
	_dc_file_has_primary_wrapper_exec "$f" || return 2
	after="$(dc_guardian_fingerprint "$f")"
	[ "$before" = "$after" ] && return 1
	return 0
}

# A candidate is any recognized launcher/patched entry. A Steam-named entry with
# malformed Exec syntax is also mandatory and fails closed instead of vanishing
# from reconciliation as "unrelated".
dc_guardian_repair_dir() {
	local dir="$1" scope="$2" f name class candidate result
	[ -d "$dir" ] || return 0
	for f in "$dir"/*; do
		[ -f "$f" ] || [ -L "$f" ] || continue
		name="$(dc_desktop_id "$f" | tr '[:upper:]' '[:lower:]')"
		case "$name" in *.desktop) : ;; *) continue ;; esac
		class="$(dc_classify "$f")"
		candidate=0
		case "$class" in
			launcher|patched) candidate=1 ;;
			stub) [ "${DC_STEAM_INSTALLED:-0}" = 1 ] && candidate=1 ;;
			unrelated)
				case "$name" in
					*steam*.desktop) _dc_file_exec_syntax_valid "$f" || candidate=1 ;;
				esac
				;;
		esac
		[ "$candidate" = 1 ] || continue
		dc_guardian_patch_file "$f"; result=$?
		dc_guardian_record_result "$scope" "$result"
	done
}

# Seed only missing application IDs here; all existing user entries are repaired
# by the following directory pass. This preserves the required shadow->repair
# order and avoids counting the same existing entry twice.
dc_guardian_seed_application_shadows() {
	local user_dir dir source kind id target have_real=0 seen='' result
	user_dir="$(dc_user_app_dir)"
	while IFS= read -r dir; do
		[ "$dir" = "$user_dir" ] && continue
		[ -d "$dir" ] || continue
		for source in "$dir"/*; do
			_dc_application_source "$source" || continue
			[ "$(_dc_application_donor_kind "$source")" = launcher ] && have_real=1
		done
	done < <(dc_application_dirs)

	while IFS= read -r dir; do
		[ "$dir" = "$user_dir" ] && continue
		[ -d "$dir" ] || continue
		for source in "$dir"/*; do
			_dc_application_source "$source" || continue
			kind="$(_dc_application_donor_kind "$source")"
			case "$kind" in
				launcher) : ;;
				stub)
					[ "$have_real" = 0 ] && [ "${DC_STEAM_INSTALLED:-0}" = 1 ] || continue
					;;
				*) continue ;;
			esac
			id="$(dc_desktop_id "$source")"
			printf '%s\n' "$seen" | grep -qxF "$id" 2>/dev/null && continue
			seen="${seen}${seen:+
}$id"
			target="$user_dir/$id"
			[ -e "$target" ] || [ -L "$target" ] && continue
			if _dc_publish_application_shadow "$source" "$target" "$kind" \
			   && _dc_file_has_primary_wrapper_exec "$target"; then
				result=0
			else
				result=2
			fi
			dc_guardian_record_result app "$result"
		done
	done < <(dc_application_dirs)
}

# Migrate each user-owned adjacent backup explicitly so a failed migration is
# visible and retryable rather than hidden by the legacy best-effort directory
# helper. Migration changes do not imply an applications cache rebuild.
dc_guardian_migrate_dir() {
	local dir="$1" legacy result
	[ -d "$dir" ] || return 0
	for legacy in "$dir"/*steam*.desktop.slssteam-backup \
	              "$dir"/*steam*.desktop.slsteam-bak; do
		[ -f "$legacy" ] || continue
		if dc_migrate_legacy_file "$legacy"; then result=0; else result=2; fi
		dc_guardian_record_result other "$result"
	done
}

dc_guardian_migrate_legacy() {
	dc_guardian_migrate_dir "$(dc_user_app_dir)"
	dc_guardian_migrate_dir "$(dc_config_home)/autostart"
	dc_guardian_migrate_dir "$(dc_desktop_dir)"
}

# Seed a user autostart shadow only when the existing system entry requires one.
# Validate the resulting primary Exec because the legacy seed helper is purposely
# best-effort for old callers.
dc_guardian_seed_autostart() {
	local user_as sys_as result
	user_as="$(dc_config_home)/autostart/steam.desktop"
	sys_as="$DC_SYS_AUTOSTART/steam.desktop"
	[ -e "$user_as" ] || [ -L "$user_as" ] && return 0
	[ -f "$sys_as" ] || return 0
	grep -q '^Name=Install Steam' "$sys_as" 2>/dev/null && return 0
	grep -qiE '^Exec=.*steam' "$sys_as" 2>/dev/null || return 0
	dc_seed_autostart_override
	if _dc_file_has_primary_wrapper_exec "$user_as"; then result=0; else result=2; fi
	dc_guardian_record_result autostart "$result"
}

dc_guardian_patch_shortcut() {
	local shortcut class before after result
	shortcut="$(dc_desktop_dir)/steam.desktop"
	[ -e "$shortcut" ] || [ -L "$shortcut" ] || return 0
	class="$(dc_classify "$shortcut")"
	case "$class" in
		launcher|patched) : ;;
		stub) [ "${DC_STEAM_INSTALLED:-0}" = 1 ] || return 0 ;;
		*)
			_dc_file_exec_syntax_valid "$shortcut" && return 0
			dc_guardian_record_result other 2
			return 0
			;;
	esac
	before="$(dc_guardian_fingerprint "$shortcut")"
	if dc_patch_one "$shortcut" && _dc_file_has_primary_wrapper_exec "$shortcut"; then
		chmod 0755 "$shortcut" 2>/dev/null || { dc_guardian_record_result other 2; return 0; }
		command -v gio >/dev/null 2>&1 \
			&& gio set "$shortcut" metadata::trusted true >/dev/null 2>&1 || true
		after="$(dc_guardian_fingerprint "$shortcut")"
		if [ "$before" = "$after" ]; then result=1; else result=0; fi
	else
		result=2
	fi
	dc_guardian_record_result other "$result"
}

# Cache tools are optional and best-effort, but when present they always run in
# applications-database then KSycoca order after the changed file is published.
dc_guardian_refresh_cache() {
	local tool
	[ "$DC_APP_CHANGED" -gt 0 ] || return 0
	tool="$DC_UPDATE_DESKTOP_DATABASE"
	if [ -n "$tool" ] && command -v "$tool" >/dev/null 2>&1; then
		"$tool" "$(dc_user_app_dir)" >/dev/null 2>&1 || true
	fi
	if [ -n "$DC_KBUILDSYCOCA" ]; then
		tool="$DC_KBUILDSYCOCA"
		command -v "$tool" >/dev/null 2>&1 \
			&& "$tool" --noincremental >/dev/null 2>&1 || true
	elif command -v kbuildsycoca6 >/dev/null 2>&1; then
		kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
	elif command -v kbuildsycoca5 >/dev/null 2>&1; then
		kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
	fi
}

dc_state_home() {
	case "${XDG_STATE_HOME:-}" in
		/*) printf '%s\n' "$XDG_STATE_HOME" ;;
		*) printf '%s\n' "$DC_HOME/.local/state" ;;
	esac
}

dc_guardian_write_summary() {
	local dir log timestamp
	dir="$(dc_state_home)/slsteam-moon"
	log="$dir/guardian.log"
	mkdir -p "$dir" 2>/dev/null || return 2
	timestamp="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)" || return 2
	printf '%s examined=%s changed=%s app_changed=%s autostart_changed=%s skipped=%s failed=%s\n' \
		"$timestamp" "$DC_EXAMINED" "$DC_CHANGED" "$DC_APP_CHANGED" \
		"$DC_AUTOSTART_CHANGED" "$DC_SKIPPED" "$DC_FAILED" >> "$log" 2>/dev/null \
		|| return 2
}

# dc_guardian_run — serialized, mandatory user-only reconciliation. Lock
# contention is a successful no-op. No system mutation or sudo path is reachable.
dc_guardian_run() {
	local runtime uid lock result status=0
	runtime="${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}"
	uid="${UID:-$(id -u)}"
	lock="${runtime%/}/slsteam-desktop-guardian-${uid}.lock"
	command -v "$DC_FLOCK" >/dev/null 2>&1 || return 2
	exec 9>"$lock" 2>/dev/null || return 2
	if ! "$DC_FLOCK" -n 9; then
		exec 9>&-
		return 0
	fi

	dc_guardian_reset_results
	# Required order: legacy migration, application shadow/repair, autostart
	# shadow/repair, existing shortcut, then cache convergence.
	dc_guardian_migrate_legacy
	dc_guardian_seed_application_shadows
	dc_guardian_repair_dir "$(dc_user_app_dir)" app
	dc_guardian_seed_autostart
	dc_guardian_repair_dir "$(dc_config_home)/autostart" autostart
	dc_guardian_patch_shortcut
	dc_guardian_refresh_cache

	[ "$DC_FAILED" -eq 0 ] || status=2
	if ! dc_guardian_write_summary; then status=2; fi
	# The summary is durable before another trigger may acquire the lock.
	"$DC_FLOCK" -u 9 >/dev/null 2>&1 || true
	exec 9>&-
	return "$status"
}

# dc_run [--user|--system] — patch all known *steam*.desktop locations. --user
# (default) does user-owned dirs only (no sudo). --system additionally patches
# the system menu dir + stub (caller must provide sudo rights). An existing
# desktop shortcut is patched in place; one is never created implicitly.
dc_run() {
	local mode="${1:---user}" menu user_apps user_autostart failed=0
	user_apps="$(dc_data_home)/applications"
	user_autostart="$(dc_config_home)/autostart"
	menu="$user_apps/steam.desktop"
	# Migration must precede every repatch: adjacent .desktop backups are active
	# launch candidates on KDE's systemd XDG-autostart implementation.
	dc_migrate_legacy_backups "$mode" || failed=1
	# Create/repair authoritative same-ID shadows before the legacy filename glob.
	dc_reconcile_application_entries
	# Seed a user autostart override from the system entry (SteamOS/Bazzite) BEFORE
	# the autostart glob, so a freshly seeded file is (idempotently) re-patched too.
	dc_seed_autostart_override
	dc_patch_glob "" "$user_autostart" || failed=1
	if [ "$mode" = "--system" ]; then
		dc_patch_glob "$DC_SUDO" "$DC_SYS_APPS" || failed=1
		dc_patch_glob "$DC_SUDO" "$DC_SYS_AUTOSTART" || failed=1
	fi
	[ -f "$menu" ] && dc_patch_shortcut "$(dc_desktop_dir)/steam.desktop"
	[ "$failed" = 0 ] || return 2
	return 0
}

# dc_restore_one <file> [sudo] — migrate any adjacent legacy backup, then
# restore from the central mirrored backup when present (0644); otherwise remove
# entries carrying our tag. A symlink we made is replaced by its central
# original when available.
dc_restore_one() {
	local f="$1" S="${2:-}" bak
	# Accept and immediately centralize both historical adjacent suffixes.
	dc_migrate_legacy_file "$f.slssteam-backup" "$S" || true
	dc_migrate_legacy_file "$f.slsteam-bak" "$S" || true
	bak="$(dc_backup_path "$f")"
	# A seeded override was created by us — the user never had it — so remove it
	# (and any stray backup) rather than restoring a vanilla copy.
	if [ -f "$f" ] && grep -qxF "$DC_SEED_TAG" "$f" 2>/dev/null; then
		$S rm -f -- "$f" 2>/dev/null || return 1
		rm -f -- "$bak" "$f.slssteam-backup" "$f.slsteam-bak" 2>/dev/null
		return 0
	fi
	if [ -L "$f" ]; then
		$S rm -f -- "$f" 2>/dev/null || return 1
		if [ -f "$bak" ]; then
			if $S cp -- "$bak" "$f" 2>/dev/null; then
				rm -f -- "$bak" 2>/dev/null
			else
				return 1
			fi
		fi
		return 0
	fi
	if [ -f "$bak" ]; then
		if $S cp --remove-destination -- "$bak" "$f" 2>/dev/null; then
			$S chmod 0644 "$f" 2>/dev/null || true
			rm -f -- "$bak" 2>/dev/null
			return 0
		fi
		return 1
	fi
	if [ -f "$f" ] && grep -q "$DC_TAG" "$f" 2>/dev/null; then
		$S rm -f -- "$f" 2>/dev/null || return 1
	fi
	return 0
}

dc_restore_directory() {
	local dir="$1" S="${2:-}" f name failed=0
	[ -d "$dir" ] || return 0
	for f in "$dir"/*; do
		[ -e "$f" ] || [ -L "$f" ] || continue
		name="$(dc_desktop_id "$f" | tr '[:upper:]' '[:lower:]')"
		case "$name" in
			*.desktop) dc_restore_one "$f" "$S" || failed=1 ;;
		esac
	done
	[ "$failed" = 0 ]
}

# Restore the optional system fallback first while user shadows still guarantee
# launch coverage. If that layer cannot be restored, retain all working user
# entries and their backups so a later uninstall can retry safely.
dc_restore_all() {
	local system_failed=0 user_failed=0
	dc_migrate_legacy_backups --system || system_failed=1
	dc_restore_directory "$DC_SYS_APPS" "$DC_SUDO" || system_failed=1
	dc_restore_directory "$DC_SYS_AUTOSTART" "$DC_SUDO" || system_failed=1
	[ "$system_failed" = 0 ] || return 2

	dc_restore_directory "$(dc_user_app_dir)" || user_failed=1
	dc_restore_directory "$(dc_config_home)/autostart" || user_failed=1
	dc_restore_one "$(dc_desktop_dir)/steam.desktop" || user_failed=1
	[ "$user_failed" = 0 ] || return 2
	return 0
}
