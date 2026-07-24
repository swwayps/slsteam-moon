#!/usr/bin/env bash
# Sourceable renderers and lifecycle helpers for the desktop guardian user units.
# The coverage library owns effective XDG directory resolution.
_DGU_LIB_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || return 1
# shellcheck source=desktop-coverage.lib.sh
. "$_DGU_LIB_DIR/desktop-coverage.lib.sh" || return 1
unset _DGU_LIB_DIR

: "${DGU_UNIT_DIR:=$(dc_config_home)/systemd/user}"
: "${DGU_SYSTEMCTL:=systemctl}"

DGU_SERVICE=slsteam-desktop-guardian.service
DGU_PATH=slsteam-desktop-guardian.path
DGU_TIMER=slsteam-desktop-guardian.timer
DGU_UNIT_SENTINEL='# X-SLSteamMoon-GuardianUnit=true'

dgu_log() {
	printf 'desktop-guardian-units: %s\n' "$*" >&2
}

dgu_service_content() {
	cat <<EOF
$DGU_UNIT_SENTINEL
[Unit]
Description=Reconcile Steam desktop launch coverage

[Service]
Type=oneshot
ExecStart=%h/.local/share/SLSsteam/ensure-desktop-coverage.sh --guardian
EOF
}

dgu_timer_content() {
	cat <<EOF
$DGU_UNIT_SENTINEL
[Unit]
Description=Periodic Steam desktop launch coverage reconciliation

[Timer]
OnStartupSec=30s
OnUnitActiveSec=5min
Persistent=true
Unit=slsteam-desktop-guardian.service

[Install]
WantedBy=timers.target
EOF
}
dgu_path_content() {
	local application_output autostart_output desktop dir index
	local -a applications autostarts paths
	local -A seen=()

	application_output="$(dc_application_dirs)" || return 1
	autostart_output="$(dc_autostart_dirs)" || return 1
	desktop="$(dc_desktop_dir)" || return 1
	mapfile -t applications <<< "$application_output"
	mapfile -t autostarts <<< "$autostart_output"

	for index in "${!applications[@]}"; do
		dir="${applications[$index]}"
		[ -n "$dir" ] || continue
		if [ "$index" -eq 0 ] || { [ -d "$dir" ] && [ -r "$dir" ]; }; then
			paths+=("$dir")
		fi
	done
	for index in "${!autostarts[@]}"; do
		dir="${autostarts[$index]}"
		[ -n "$dir" ] || continue
		if [ "$index" -eq 0 ] || { [ -d "$dir" ] && [ -r "$dir" ]; }; then
			paths+=("$dir")
		fi
	done
	[ -n "$desktop" ] && paths+=("$desktop")

	cat <<EOF
$DGU_UNIT_SENTINEL
[Unit]
Description=Watch desktop launch sources for reconciliation

[Path]
EOF
	for dir in "${paths[@]}"; do
		[ -n "${seen["$dir"]+set}" ] && continue
		seen["$dir"]=1
		printf 'PathChanged=%s\n' "$dir"
	done
	cat <<'EOF'
Unit=slsteam-desktop-guardian.service

[Install]
WantedBy=default.target
EOF
}

# Return 0 after replacement, 1 when unchanged, or 2 on a write error.
dgu_write_changed() {
	local name="$1" content="$2" destination temporary
	destination="$DGU_UNIT_DIR/$name"
	if [ -e "$destination" ] || [ -L "$destination" ]; then
		[ -f "$destination" ] || return 2
		grep -qxF "$DGU_UNIT_SENTINEL" "$destination" 2>/dev/null || return 2
	fi
	temporary="$(mktemp "$DGU_UNIT_DIR/.${name}.tmp.XXXXXX")" || return 2
	if ! printf '%s\n' "$content" > "$temporary"; then
		rm -f -- "$temporary"
		return 2
	fi
	chmod 0644 "$temporary" 2>/dev/null || true
	if [ -f "$destination" ] && cmp -s -- "$temporary" "$destination"; then
		rm -f -- "$temporary"
		return 1
	fi
	if mv -fT -- "$temporary" "$destination"; then
		return 0
	fi
	rm -f -- "$temporary"
	return 2
}
dgu_systemctl() {
	local description="$1"
	shift
	dgu_log "systemctl --user $description"
	if ! "$DGU_SYSTEMCTL" --user "$@"; then
		dgu_log "$description failed"
		return 1
	fi
	return 0
}

# Manager operations are independent and best-effort so unavailable user
# systemd never invalidates the mandatory desktop coverage layer.
dgu_enable_units() {
	dgu_systemctl daemon-reload daemon-reload || true
	dgu_systemctl "enable --now $DGU_PATH $DGU_TIMER" \
		enable --now "$DGU_PATH" "$DGU_TIMER" || true
	if [ "${DGU_NO_SERVICE_START:-0}" != 1 ]; then
		dgu_systemctl "start $DGU_SERVICE" start "$DGU_SERVICE" || true
	fi
	return 0
}

# Return 0 when unit bytes changed (and the manager was notified), 1 when all
# files already match, or 2 when rendering/writing failed.
dgu_install_units() {
	local service_content path_content timer_content status
	local changed=0 errors=0

	service_content="$(dgu_service_content)" || {
		dgu_log "cannot render $DGU_SERVICE"
		return 2
	}
	path_content="$(dgu_path_content)" || {
		dgu_log "cannot render $DGU_PATH"
		return 2
	}
	timer_content="$(dgu_timer_content)" || {
		dgu_log "cannot render $DGU_TIMER"
		return 2
	}
	# Fail closed before publishing any sibling when an existing same-name unit
	# is not owned by this project (or is not a regular file).
	local unit destination
	for unit in "$DGU_SERVICE" "$DGU_PATH" "$DGU_TIMER"; do
		destination="$DGU_UNIT_DIR/$unit"
		if [ -e "$destination" ] || [ -L "$destination" ]; then
			if [ ! -f "$destination" ] \
			   || ! grep -qxF "$DGU_UNIT_SENTINEL" "$destination" 2>/dev/null; then
				dgu_log "preserving foreign unit: $destination"
				return 2
			fi
		fi
	done
	if ! mkdir -p -- "$DGU_UNIT_DIR"; then
		dgu_log "cannot create unit directory: $DGU_UNIT_DIR"
		return 2
	fi

	if dgu_write_changed "$DGU_SERVICE" "$service_content"; then
		status=0
	else
		status=$?
	fi
	case "$status" in 0) changed=1 ;; 1) : ;; *) errors=1 ;; esac
	if dgu_write_changed "$DGU_PATH" "$path_content"; then
		status=0
	else
		status=$?
	fi
	case "$status" in 0) changed=1 ;; 1) : ;; *) errors=1 ;; esac
	if dgu_write_changed "$DGU_TIMER" "$timer_content"; then
		status=0
	else
		status=$?
	fi
	case "$status" in 0) changed=1 ;; 1) : ;; *) errors=1 ;; esac

	[ "$errors" -eq 0 ] || return 2
	if [ "$changed" -eq 1 ]; then
		dgu_enable_units
		return 0
	fi
	return 1
}
dgu_remove_units() {
	local unit path changed=0 errors=0
	local -a owned=()
	for unit in "$DGU_SERVICE" "$DGU_PATH" "$DGU_TIMER"; do
		path="$DGU_UNIT_DIR/$unit"
		[ -f "$path" ] || continue
		grep -qxF "$DGU_UNIT_SENTINEL" "$path" 2>/dev/null || continue
		owned+=("$path")
	done
	[ "${#owned[@]}" -gt 0 ] || return 1

	# Stop triggers while their service file still exists, then remove only files
	# carrying this project's ownership sentinel.
	dgu_systemctl "disable --now $DGU_PATH $DGU_TIMER" \
		disable --now "$DGU_PATH" "$DGU_TIMER" || true
	for path in "${owned[@]}"; do
		if rm -f -- "$path"; then
			changed=1
		else
			dgu_log "cannot remove unit: $path"
			errors=1
		fi
	done
	if [ "$changed" -eq 1 ]; then
		dgu_systemctl daemon-reload daemon-reload || true
		dgu_systemctl reset-failed reset-failed || true
	fi
	[ "$errors" -eq 0 ] || return 2
	return 0
}

# Generated XDG-autostart unit drop-ins. These are user-owned and independent
# from the guardian's own service/path/timer files.
: "${DGU_SYSTEMD_ESCAPE:=systemd-escape}"
DGU_AUTOSTART_DROPIN=slsteam-guardian.conf
DGU_AUTOSTART_SENTINEL='# X-SLSteamMoon-AutostartDropIn=true'

dgu_autostart_unit_name() {
	local id="$1" stem escaped
	case "$id" in *.desktop) stem="${id%.desktop}" ;; *) return 1 ;; esac
	[ -n "$stem" ] || return 1
	if command -v "$DGU_SYSTEMD_ESCAPE" >/dev/null 2>&1; then
		# app-<escaped-id>@autostart.service is not a conventional systemd
		# template instance (the literal @autostart suffix belongs to the XDG
		# generator), so escape only the desktop ID and compose the unit name.
		escaped="$("$DGU_SYSTEMD_ESCAPE" "$stem")" || return 1
		[ -n "$escaped" ] || return 1
		printf 'app-%s@autostart.service\n' "$escaped"
		return 0
	fi
	[[ "$stem" =~ ^[A-Za-z0-9_.-]+$ ]] || return 1
	escaped="${stem//-/\\x2d}"
	printf 'app-%s@autostart.service\n' "$escaped"
}

# Print preserved arguments from the primary Desktop Entry Exec. The launcher,
# optional env prefix/assignments and Desktop field codes are intentionally
# omitted because the generated service executes the wrapper directly.
dgu_autostart_args() {
	local file="$1" line in_entry=0 exec_text args code
	local -a parts=()
	[ -f "$file" ] || return 1
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
			'[Desktop Entry]') in_entry=1 ;;
			'['*']') [ "$in_entry" = 1 ] && break ;;
			Exec=*)
				[ "$in_entry" = 1 ] || continue
				exec_text="${line#Exec=}"
				mapfile -t parts < <(_dc_exec_scan parts "$exec_text")
				[ "${#parts[@]}" -ge 3 ] || return 1
				args="${parts[2]}"
				for code in '%f' '%F' '%u' '%U' '%i' '%c' '%k' '%%'; do
					args="${args//"$code"/}"
				done
				args="$(printf '%s' "$args" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
				printf '%s\n' "$args"
				return 0
				;;
		esac
	done < "$file"
	return 1
}

dgu_systemd_exec_arg() {
	local value="$1" escaped
	if [[ "$value" =~ ^[-A-Za-z0-9_./:@+]+$ ]]; then
		printf '%s' "$value"
		return
	fi
	escaped="${value//\\/\\\\}"
	escaped="${escaped//\"/\\\"}"
	escaped="${escaped//%/%%}"
	printf '"%s"' "$escaped"
}

dgu_autostart_dropin_content() {
	local file="$1" args command
	args="$(dgu_autostart_args "$file")" || return 1
	command="$(dgu_systemd_exec_arg "$WRAPPER")" || return 1
	printf '%s\n[Service]\nExecStart=\n' "$DGU_AUTOSTART_SENTINEL"
	if [ -n "$args" ]; then
		printf 'ExecStart=%s %s\n' "$command" "$args"
	else
		printf 'ExecStart=%s\n' "$command"
	fi
}

# Print the first supported autostart source for each desktop ID in XDG
# precedence order. Merely mentioning Steam in Name= is never sufficient.
dgu_autostart_sources() {
	local dir file id lower args seen=''
	while IFS= read -r dir; do
		[ -d "$dir" ] || continue
		for file in "$dir"/*; do
			[ -f "$file" ] || continue
			id="$(basename -- "$file")"
			lower="$(printf '%s' "$id" | tr '[:upper:]' '[:lower:]')"
			case "$lower" in *.desktop) : ;; *) continue ;; esac
			printf '%s\n' "$seen" | grep -qxF "$id" 2>/dev/null && continue
			grep -q '^Name=Install Steam$' "$file" 2>/dev/null && continue
			args="$(dgu_autostart_args "$file")" || continue
			seen="${seen}${seen:+
}$id"
			printf '%s\n' "$file"
		done
	done < <(dc_autostart_dirs)
}

# Return 0 changed, 1 unchanged/foreign, 2 failure.
dgu_write_autostart_dropin() {
	local destination="$1" content="$2" directory temporary
	directory="$(dirname -- "$destination")"
	if [ -e "$destination" ] && ! grep -qxF "$DGU_AUTOSTART_SENTINEL" "$destination" 2>/dev/null; then
		dgu_log "preserving foreign drop-in: $destination"
		return 1
	fi
	mkdir -p -- "$directory" || return 2
	[ ! -d "$destination" ] || return 2
	temporary="$(mktemp "$directory/.${DGU_AUTOSTART_DROPIN}.tmp.XXXXXX")" || return 2
	if ! printf '%s\n' "$content" > "$temporary"; then
		rm -f -- "$temporary"
		return 2
	fi
	chmod 0644 "$temporary" 2>/dev/null || true
	if [ -f "$destination" ] && cmp -s -- "$temporary" "$destination"; then
		rm -f -- "$temporary"
		return 1
	fi
	if mv -fT -- "$temporary" "$destination"; then return 0; fi
	rm -f -- "$temporary"
	return 2
}

dgu_install_autostart_dropins() {
	local source id unit destination content status stale changed=0 errors=0
	local -A wanted=()
	while IFS= read -r source; do
		[ -n "$source" ] || continue
		id="$(basename -- "$source")"
		unit="$(dgu_autostart_unit_name "$id")" || { errors=1; continue; }
		destination="$DGU_UNIT_DIR/${unit}.d/$DGU_AUTOSTART_DROPIN"
		content="$(dgu_autostart_dropin_content "$source")" || { errors=1; continue; }
		wanted["$destination"]=1
		if dgu_write_autostart_dropin "$destination" "$content"; then status=0; else status=$?; fi
		case "$status" in 0) changed=1 ;; 1) : ;; *) errors=1 ;; esac
	done < <(dgu_autostart_sources)

	# Remove stale files only when they carry this project's sentinel.
	for stale in "$DGU_UNIT_DIR"/app-*@autostart.service.d/"$DGU_AUTOSTART_DROPIN"; do
		[ -f "$stale" ] || continue
		[ -n "${wanted["$stale"]+set}" ] && continue
		grep -qxF "$DGU_AUTOSTART_SENTINEL" "$stale" 2>/dev/null || continue
		if rm -f -- "$stale"; then
			rmdir -- "$(dirname -- "$stale")" 2>/dev/null || true
			changed=1
		else
			errors=1
		fi
	done
	if [ "$changed" -eq 1 ]; then
		dgu_systemctl daemon-reload daemon-reload || true
	fi
	[ "$errors" -eq 0 ] || return 2
	[ "$changed" -eq 1 ] && return 0
	return 1
}

dgu_remove_autostart_dropins() {
	local dropin changed=0 errors=0
	for dropin in "$DGU_UNIT_DIR"/app-*@autostart.service.d/"$DGU_AUTOSTART_DROPIN"; do
		[ -f "$dropin" ] || continue
		grep -qxF "$DGU_AUTOSTART_SENTINEL" "$dropin" 2>/dev/null || continue
		if rm -f -- "$dropin"; then
			rmdir -- "$(dirname -- "$dropin")" 2>/dev/null || true
			changed=1
		else
			errors=1
		fi
	done
	if [ "$changed" -eq 1 ]; then
		dgu_systemctl daemon-reload daemon-reload || true
	fi
	[ "$errors" -eq 0 ] || return 2
	[ "$changed" -eq 1 ] && return 0
	return 1
}