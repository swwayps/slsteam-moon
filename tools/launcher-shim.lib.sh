# launcher-shim.lib.sh — safe, sourceable helpers for the distro Steam launcher.
#
# This wraps package-owned launchers (/usr/bin/steam, /usr/games/steam, or
# /usr/local/bin/steam). It deliberately never touches Steam's own steam.sh,
# which is verified and re-extracted by Steam's bootstrapper.
#
# Callers may override LS_* variables before sourcing this file. No top-level
# command mutates the filesystem.
: "${LS_TAG:=# slsteam-moon system launcher shim}"
: "${LS_SLSDIR:=$HOME/.local/share/SLSsteam}"
: "${LS_BACKUP_ROOT:=$LS_SLSDIR/system-launcher-backup}"
: "${LS_SUDO:=}"

if ! declare -p LS_LAUNCHER_DIRS >/dev/null 2>&1; then
	LS_LAUNCHER_DIRS=(/usr/bin /usr/games /usr/local/bin)
fi

# Resolve the initialized Steam data root. Production callers use Valve's
# canonical ~/.steam/steam symlink; tests and diagnostics may inject a root
# directly so eligibility never depends on the host's Steam installation.
ls_steam_root() {
	local root
	if [ -n "${LS_STEAM_ROOT:-}" ]; then
		printf '%s\n' "$LS_STEAM_ROOT"
		return 0
	fi
	# In production, Valve owns this path as a symlink into the actual Steam
	# installation. Do not accept a merely matching directory: a partial or
	# foreign tree can contain an executable steam.sh without being bootstrapped.
	[ -L "$HOME/.steam/steam" ] || return 1
	root="$(readlink -e -q "$HOME/.steam/steam" 2>/dev/null || true)"
	[ -n "$root" ] || return 1
	printf '%s\n' "$root"
}

# A distro launcher is eligible only after Steam has bootstrapped its real data
# root. This is the guard that keeps Debian's pre-bootstrap `Install Steam`
# package stub usable: an absent/non-executable steam.sh means no replacement.
ls_steam_bootstrapped() {
	local root
	root="$(ls_steam_root)" || return 1
	[ -d "$root" ] && [ -f "$root/steam.sh" ] && [ -x "$root/steam.sh" ]
}

ls_launcher_eligible() {
	[ -f "$1" ] && [ -x "$1" ] && ls_steam_bootstrapped
}

ls_detect_launchers() {
	local dir launcher
	for dir in "${LS_LAUNCHER_DIRS[@]}"; do
		launcher="${dir%/}/steam"
		[ -f "$launcher" ] && printf '%s\n' "$launcher"
	done
}

ls_is_our_shim() {
	[ -f "$1" ] && head -3 "$1" 2>/dev/null | grep -qF "$LS_TAG"
}

ls_is_injected_wrapper() {
	local candidate="$1" wrapper="$HOME/.local/share/SLSsteam/path/steam"
	local resolved_candidate resolved_wrapper
	resolved_candidate="$(readlink -f "$candidate" 2>/dev/null || true)"
	resolved_wrapper="$(readlink -f "$wrapper" 2>/dev/null || true)"
	[ -n "$resolved_candidate" ] && [ -n "$resolved_wrapper" ] && \
		[ "$resolved_candidate" = "$resolved_wrapper" ]
}

# The generated shim records the exact original it captured. Older installs did
# not emit SLSM_ORIG, but embedded the same absolute path in their literal
# fallback command; accept that historical form only when it matches exactly.
ls_shim_references_backup() {
	local launcher="$1" backup="$2"
	[ -f "$launcher" ] || return 1
	grep -Fqx -- "SLSM_ORIG=\"$backup\"" "$launcher" 2>/dev/null && return 0
	grep -Fqx -- "exec \"$backup\" \"\$@\"" "$launcher" 2>/dev/null
}

ls_backup_is_usable() {
	[ -f "$1" ] && [ -x "$1" ] || return 1
	! ls_is_our_shim "$1" || return 1
	! ls_is_injected_wrapper "$1"
}

# Run backup-tree mutations with the configured privilege prefix. The live
# launcher writer has its own privilege handling; this helper covers mirrored
# backup migration, refresh, and mode normalization as well.
ls_run_privileged() {
	if [ -n "${LS_SUDO:-}" ]; then
		$LS_SUDO "$@"
	else
		"$@"
	fi
}

ls_wrapped_paths() {
	local launcher
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" && printf '%s\n' "$launcher"
	done < <(ls_detect_launchers)
}

ls_detected_launcher_count() {
	local launcher count=0
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		count=$((count + 1))
	done < <(ls_detect_launchers)
	printf '%s\n' "$count"
}

ls_wrapped_launcher_count() {
	local launcher count=0
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" || continue
		count=$((count + 1))
	done < <(ls_detect_launchers)
	printf '%s\n' "$count"
}

ls_legacy_backup_is_unambiguous() {
	local legacy="$1" references=0 launcher
	ls_backup_is_usable "$legacy" || return 1
	# A flat backup is safe only when exactly one surviving shim explicitly
	# references this exact file. Other mirrored shims do not make that owner
	# ambiguous; they are unrelated paths that can be reconciled first.
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" || continue
		ls_shim_references_backup "$launcher" "$legacy" || continue
		references=$((references + 1))
	done < <(ls_detect_launchers)
	[ "$references" -eq 1 ]
}

ls_legacy_backup_is_unambiguous_restore() {
	local legacy="$1" references=0 launcher
	ls_backup_is_usable "$legacy" || return 1
	# Ambiguity is candidate-specific: unrelated surviving shims do not own this
	# flat backup. Reject only when zero or multiple shims reference this path.
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" || continue
		ls_shim_references_backup "$launcher" "$legacy" || continue
		references=$((references + 1))
	done < <(ls_detect_launchers)
	[ "$references" -eq 1 ]
}

ls_backup_path() {
	local launcher="$1" relative
	case "$launcher" in
		/*) relative="${launcher#/}" ;;
		*)  relative="$launcher" ;;
	esac
	printf '%s/%s.orig\n' "${LS_BACKUP_ROOT%/}" "$relative"
}

ls_legacy_backup_path() {
	printf '%s/%s.orig\n' "${LS_BACKUP_ROOT%/}" "$(basename -- "$1")"
}

# Valve's distro launcher (bin_steam.sh, which /usr/bin/steam symlinks to on
# Fedora/Nobara) derives its whole identity from its own argv[0]:
#
#     STEAMPACKAGE="${0##*/}"; case "$STEAMPACKAGE" in steam) ;; steambeta) ;;
#         *) log "Unknown Steam package '$STEAMPACKAGE'"; exit 1 ;;
#
# So a captured original may never be executed as `steam.orig`: the launch dies
# before Steam opens its logger, i.e. Steam silently does not start and nothing
# is written to any log. Names Valve accepts, plus the name-agnostic data-dir
# steam.sh.
ls_name_is_launcher_safe() {
	case "${1##*/}" in
		steam|steambeta|bin_steam.sh|steam.sh) return 0 ;;
	esac
	return 1
}

# The `steam`-named alias that sits next to a captured original and points at
# it. Executing the alias gives the captured launcher the argv[0] it requires
# while `<name>.orig` stays the authoritative backup for restoration.
ls_alias_for_backup() {
	local backup="$1" dir
	dir="$(dirname -- "$backup")"
	printf '%s/steam\n' "${dir%/}"
}

ls_backup_alias_path() {
	ls_alias_for_backup "$(ls_backup_path "$1")"
}

# Create/refresh the alias. Best-effort by design: when it cannot be created the
# shim degrades to launching through the wrapper (or steam.sh) instead of
# executing a launcher under a name it rejects.
ls_refresh_backup_alias() {
	local backup="$1" alias target
	[ -f "$backup" ] || return 1
	alias="$(ls_alias_for_backup "$backup")"
	[ "$alias" != "$backup" ] || return 0
	[ ! -d "$alias" ] || return 1
	target="$(basename -- "$backup")"
	if [ "$(readlink -- "$alias" 2>/dev/null || true)" != "$target" ]; then
		ls_run_privileged ln -sfn -- "$target" "$alias" 2>/dev/null || return 1
	fi
	[ -x "$alias" ] || return 1
	[ "$(readlink -f "$alias" 2>/dev/null || true)" = \
	  "$(readlink -f "$backup" 2>/dev/null || true)" ]
}

# Drop the alias once its captured original is no longer needed. Only ever
# removes a symlink, never a real file.
ls_remove_backup_alias() {
	local backup="$1" alias
	alias="$(ls_alias_for_backup "$backup")"
	[ "$alias" != "$backup" ] || return 0
	[ -L "$alias" ] || return 0
	ls_run_privileged rm -f -- "$alias" 2>/dev/null || true
	return 0
}

ls_shim_content() {
	local backup="$1" alias="${2:-}"
	[ -n "$alias" ] || alias="$(ls_alias_for_backup "$backup")"
	cat <<EOF
#!/bin/sh
$LS_TAG
# Managed by slsteam-moon. Run the uninstaller to restore the package launcher.
#
# TRUST NOTE. This script is root-owned and sits on the system PATH, but the
# wrapper it delegates to lives in a user's home directory and is writable by
# that user. That inversion is only safe while the two identities agree, so the
# wrapper must be a plain file (not a symlink) OWNED BY THE EFFECTIVE ACCOUNT
# with no group or other write bit before it is executed. An invocation that
# arrives elevated while \$HOME still names an unprivileged user's directory
# (sudo -E, sudoers env_keep += HOME, a helper that inherits the environment)
# therefore finds a wrapper owned by someone else, refuses it, and falls through
# to the packaged launcher instead of running a user-writable script as root.
# The account's home is resolved from the password database when the invocation
# carries no usable \$HOME of its own.
SLSM_USER="\$(id -un 2>/dev/null || true)"
SLSM_EUID="\$(id -u 2>/dev/null || true)"
SLSM_PW_HOME=""
if [ -n "\$SLSM_USER" ]; then
	SLSM_PW_HOME="\$(getent passwd "\$SLSM_USER" 2>/dev/null | cut -d: -f6)"
fi
SLSM_ENV_HOME=""
case "\${HOME:-}" in
	/*) SLSM_ENV_HOME="\$HOME" ;;
esac
slsm_wrapper_trusted() {
	[ -n "\$1" ] || return 1
	[ -L "\$1" ] && return 1
	[ -f "\$1" ] || return 1
	[ -x "\$1" ] || return 1
	_slsm_owner="\$(stat -c '%u' "\$1" 2>/dev/null || true)"
	[ -n "\$_slsm_owner" ] || return 1
	[ -n "\$SLSM_EUID" ] || return 1
	[ "\$_slsm_owner" = "\$SLSM_EUID" ] || return 1
	_slsm_perm="\$(stat -c '%A' "\$1" 2>/dev/null || true)"
	[ -n "\$_slsm_perm" ] || return 1
	# -rwxrwxrwx: character 6 is the group write bit, character 9 the other one.
	[ "\$(printf '%s' "\$_slsm_perm" | cut -c6)" = "w" ] && return 1
	[ "\$(printf '%s' "\$_slsm_perm" | cut -c9)" = "w" ] && return 1
	return 0
}
# \$HOME is what every ordinary launch carries; the password-database home is only
# consulted when the invocation arrives without one. WHICH directory is used is
# not the safety property — slsm_wrapper_trusted is. Under an elevated invocation
# the wrapper found in an unprivileged user's home is owned by that user rather
# than by the effective account, so it is refused and the packaged launcher runs.
SLSM_HOME="\$SLSM_ENV_HOME"
[ -n "\$SLSM_HOME" ] || SLSM_HOME="\$SLSM_PW_HOME"
SLSM_WRAPPER=""
if [ -n "\$SLSM_HOME" ] \\
   && slsm_wrapper_trusted "\$SLSM_HOME/.local/share/SLSsteam/path/steam"; then
	SLSM_WRAPPER="\$SLSM_HOME/.local/share/SLSsteam/path/steam"
fi
SLSM_ORIG="$backup"
SLSM_ORIG_EXEC="$alias"
SLSM_TAG="$LS_TAG"
SLSM_BOOTSTRAP_ROOT=""
SLSM_BOOTSTRAPPED=0
if [ -n "\$SLSM_HOME" ] && [ -L "\$SLSM_HOME/.steam/steam" ]; then
	SLSM_BOOTSTRAP_ROOT="\$(readlink -e "\$SLSM_HOME/.steam/steam" 2>/dev/null || true)"
	if [ -n "\$SLSM_BOOTSTRAP_ROOT" ] && \
	   [ -f "\$SLSM_BOOTSTRAP_ROOT/steam.sh" ] && \
	   [ -x "\$SLSM_BOOTSTRAP_ROOT/steam.sh" ]; then
		SLSM_BOOTSTRAPPED=1
	fi
fi
SLSM_ORIG_USABLE=0
SLSM_ORIG_REAL=""
SLSM_WRAPPER_REAL=""
if [ -f "\$SLSM_ORIG" ] && [ -x "\$SLSM_ORIG" ]; then
	SLSM_ORIG_REAL="\$(readlink -f "\$SLSM_ORIG" 2>/dev/null || true)"
	SLSM_WRAPPER_REAL="\$(readlink -f "\$SLSM_WRAPPER" 2>/dev/null || true)"
	if [ -n "\$SLSM_ORIG_REAL" ] && \
	   [ "\$SLSM_ORIG_REAL" != "\$SLSM_WRAPPER_REAL" ] && \
	   ! head -3 "\$SLSM_ORIG" 2>/dev/null | grep -qF "\$SLSM_TAG"; then
		SLSM_ORIG_USABLE=1
	fi
fi
# Valve's launcher derives STEAMPACKAGE from its own argv[0] and aborts with
# "Unknown Steam package" under any other name, so the captured original is
# never executed as "*.orig": run it through its \`steam\`-named alias. The alias
# lives in our own tree, so recreate it in place when an older install or a
# partial restore left it missing. Only when no safe name can be obtained at all
# is the captured launcher skipped in favour of steam.sh.
SLSM_LAUNCH=""
slsm_shim_alias_ready() {
	[ -x "\$SLSM_ORIG_EXEC" ] || return 1
	[ "\$(readlink -f "\$SLSM_ORIG_EXEC" 2>/dev/null || true)" = "\$SLSM_ORIG_REAL" ]
}
if [ "\$SLSM_ORIG_USABLE" = 1 ]; then
	if ! slsm_shim_alias_ready; then
		if [ ! -e "\$SLSM_ORIG_EXEC" ] || [ -L "\$SLSM_ORIG_EXEC" ]; then
			ln -sfn -- "\${SLSM_ORIG##*/}" "\$SLSM_ORIG_EXEC" 2>/dev/null || true
		fi
	fi
	if slsm_shim_alias_ready; then
		SLSM_LAUNCH="\$SLSM_ORIG_EXEC"
	else
		case "\${SLSM_ORIG##*/}" in
			steam|steambeta|bin_steam.sh|steam.sh) SLSM_LAUNCH="\$SLSM_ORIG" ;;
		esac
	fi
fi
if [ -n "\$SLSM_WRAPPER" ] && [ "\$SLSM_BOOTSTRAPPED" = 1 ]; then
	if [ -n "\$SLSM_LAUNCH" ]; then
		export SLSM_STEAM_BIN="\$SLSM_LAUNCH"
	fi
	exec "\$SLSM_WRAPPER" "\$@"
fi
if [ -n "\$SLSM_LAUNCH" ]; then
	exec "\$SLSM_LAUNCH" "\$@"
fi
for _s in "\$SLSM_HOME/.local/share/Steam/steam.sh" \
          "\$SLSM_HOME/.steam/steam/steam.sh" \
          "\$SLSM_HOME/.steam/debian-installation/steam.sh"; do
	[ -f "\$_s" ] && [ -x "\$_s" ] && exec "\$_s" "\$@"
done
echo "steam: no usable launcher found (slsteam-moon shim)" >&2
exit 127
EOF
}

ls_write_shim() {
	local launcher="$1" backup="$2" temp privilege rc
	if [ -w "$launcher" ]; then
		privilege=""
	else
		privilege="$LS_SUDO"
		[ -n "$privilege" ] || return 1
	fi
	temp="$(mktemp 2>/dev/null)" || return 1
	if ! ls_shim_content "$backup" > "$temp"; then
		rm -f -- "$temp"
		return 1
	fi
	if [ -n "$privilege" ]; then
		$privilege install -m 0755 "$temp" "$launcher" 2>/dev/null
		rc=$?
	else
		install -m 0755 "$temp" "$launcher" 2>/dev/null
		rc=$?
	fi
	rm -f -- "$temp"
	[ "$rc" -eq 0 ] || return 1
	ls_is_our_shim "$launcher"
}

ls_install_one_shim() {
	local launcher="$1" backup legacy migrated=0
	# Never capture or replace a package installer stub before Steam has created
	# its real data root. The caller can rerun after bootstrap on the same path.
	ls_launcher_eligible "$launcher" || return 1
	backup="$(ls_backup_path "$launcher")"
	legacy="$(ls_legacy_backup_path "$launcher")"
	ls_run_privileged mkdir -p "$(dirname "$backup")" 2>/dev/null || return 1

	# Migrate the pre-mirrored flat backup before an already-installed shim can
	# be mistaken for the current package launcher. Consume the flat source only
	# after the mirrored copy is safely in place so a failed migration is retryable.
	if [ ! -e "$backup" ] && ls_backup_is_usable "$legacy" && \
	   ls_shim_references_backup "$launcher" "$legacy" && \
	   ls_legacy_backup_is_unambiguous "$legacy"; then
		if ! ls_run_privileged mv -- "$legacy" "$backup" 2>/dev/null; then
			ls_run_privileged cp -- "$legacy" "$backup" 2>/dev/null || return 1
			ls_run_privileged rm -f -- "$legacy" 2>/dev/null || return 1
		fi
		ls_run_privileged chmod 0755 "$backup" 2>/dev/null || return 1
		migrated=1
	fi

	if ls_is_our_shim "$launcher"; then
		# Refresh an existing shim only from the exact backup path embedded in
		# that shim. An executable mirrored file is not authoritative merely
		# because it has the expected basename.
		if [ "$migrated" -eq 0 ] && ! ls_shim_references_backup "$launcher" "$backup"; then
			# A legacy reference is safe only when the initial migration moved
			# that exact file into an absent mirror. If migration did not happen,
			# an existing mirror is an association conflict; do not overwrite it.
			return 1
		fi
		ls_run_privileged chmod 0755 "$backup" 2>/dev/null || return 1
		ls_backup_is_usable "$backup" || return 1
		# Best effort: without the alias the shim routes through the wrapper (or
		# steam.sh) instead of executing the captured launcher under a name it
		# would reject, so coverage survives an alias failure.
		ls_refresh_backup_alias "$backup" || true
		ls_write_shim "$launcher" "$backup"
		return $?
	fi

	# A package update can replace our shim with a new genuine launcher. Keep the
	# captured original in sync, but never copy a shim into the backup.
	if [ ! -f "$backup" ] || ! cmp -s "$launcher" "$backup" 2>/dev/null; then
		ls_run_privileged cp -- "$launcher" "$backup" 2>/dev/null || return 1
	fi
	# The launcher candidate was executable, so its captured original must stay
	# executable even when a mirrored backup predates the current install.
	ls_run_privileged chmod 0755 "$backup" 2>/dev/null || return 1
	ls_backup_is_usable "$backup" || return 1
	ls_refresh_backup_alias "$backup" || true
	ls_write_shim "$launcher" "$backup"
}

ls_install_shims() {
	local launcher detected=0 failures=0
	# Existing shims must be reconciled first. This lets an unambiguous flat
	# backup be assigned to the one surviving shim before a vanilla path is
	# captured as a new launcher.
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		detected=$((detected + 1))
		ls_is_our_shim "$launcher" || continue
		if ! ls_install_one_shim "$launcher"; then
			failures=$((failures + 1))
		fi
	done < <(ls_detect_launchers)
	while IFS= read -r launcher; do
		[ -n "$launcher" ] || continue
		ls_is_our_shim "$launcher" && continue
		if ! ls_install_one_shim "$launcher"; then
			failures=$((failures + 1))
		fi
	done < <(ls_detect_launchers)
	[ "$detected" -gt 0 ] && [ "$failures" -eq 0 ]
}

ls_restore_one() {
	local launcher="$1" backup="$2" privilege
	if [ -w "$launcher" ]; then
		privilege=""
	else
		privilege="$LS_SUDO"
		[ -n "$privilege" ] || return 1
	fi
	if [ -n "$privilege" ]; then
		$privilege install -m 0755 "$backup" "$launcher" 2>/dev/null || return 1
	else
		install -m 0755 "$backup" "$launcher" 2>/dev/null || return 1
	fi
	ls_is_our_shim "$launcher" && return 1
	# The launcher is genuine again; its exec alias has no purpose left.
	ls_remove_backup_alias "$backup"
	return 0
}

ls_restore_shims() {
	local launcher backup legacy left progress
	while :; do
		left=0
		progress=0
		while IFS= read -r launcher; do
			[ -n "$launcher" ] || continue
			ls_is_our_shim "$launcher" || continue
			backup="$(ls_backup_path "$launcher")"
			# Never install a managed or otherwise unusable mirrored file over the
			# live launcher. If one exists, clear it and try an exactly-associated
			# legacy flat backup instead.
			if [ -f "$backup" ] && \
			   { ! ls_backup_is_usable "$backup" || \
			     ! ls_shim_references_backup "$launcher" "$backup"; }; then
				backup=""
			fi
			if [ -z "$backup" ] || [ ! -f "$backup" ]; then
				legacy="$(ls_legacy_backup_path "$launcher")"
				if ls_legacy_backup_is_unambiguous_restore "$legacy" && \
				   ls_shim_references_backup "$launcher" "$legacy"; then
					backup="$legacy"
				else
					backup=""
				fi
			fi
			if [ -n "$backup" ] && [ -f "$backup" ] && \
			   ls_backup_is_usable "$backup" && ls_restore_one "$launcher" "$backup"; then
				progress=1
				continue
			fi
			left=1
		done < <(ls_detect_launchers)
		[ "$left" -eq 0 ] && return 0
		# A mirrored restore can make a previously ambiguous flat backup
		# unambiguous. Re-scan until no shim remains or no progress is possible.
		[ "$progress" -eq 1 ] || return 1
	done
}
