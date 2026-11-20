#!/usr/bin/env bash
# Trust boundary of the system launcher shim.
#
# The shim is installed root-owned at /usr/bin/steam (or /usr/games/steam) and is
# on the system PATH. It delegates to a wrapper inside $HOME, which the user can
# write. That is a trust inversion: a root-owned entry point executing a
# user-writable script. If the shim is ever invoked with elevated privileges in a
# context that preserves HOME (sudo -E, sudoers env_keep += HOME, a helper that
# inherits the environment), the user's script runs as root.
#
# Which home the shim reads is not the safety property: it uses an absolute $HOME
# and falls back to the effective account's password-database home. What stops
# the escalation is that it refuses a wrapper that is not a plain file owned by
# the effective account with no group/other write bit, and runs the captured
# original only when it and every folder above it belong to root or the
# effective account and nobody else can write them.
#
# Run from the repo root:  bash scripts/test-shim-trust.sh
set -u
fails=0
checks=0
check() {
	checks=$((checks + 1))
	if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
LS_SUDO="" . "$REPO/tools/launcher-shim.lib.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

REAL_HOME="$TMP/home/real"
FAKE_HOME="$TMP/home/attacker"
mkdir -p "$REAL_HOME/.local/share/SLSsteam/path" "$REAL_HOME/.steam"
mkdir -p "$FAKE_HOME/.local/share/SLSsteam/path" "$FAKE_HOME/.steam"

# A bootstrapped Steam root, which the shim requires before it will use the
# wrapper at all.
STEAM_ROOT="$TMP/steamroot"
mkdir -p "$STEAM_ROOT"
cat > "$STEAM_ROOT/steam.sh" <<'EOF'
#!/bin/sh
echo "REAL_STEAM_SH"
EOF
chmod 0755 "$STEAM_ROOT/steam.sh"
ln -sfn "$STEAM_ROOT" "$REAL_HOME/.steam/steam"
ln -sfn "$STEAM_ROOT" "$FAKE_HOME/.steam/steam"

# The genuine wrapper, and an attacker-planted one in a different home.
for h in "$REAL_HOME" "$FAKE_HOME"; do
	cat > "$h/.local/share/SLSsteam/path/steam" <<EOF
#!/bin/sh
echo "WRAPPER:$(basename "$h")"
EOF
	chmod 0700 "$h/.local/share/SLSsteam/path/steam"
done

# The captured original package launcher.
ORIG_DIR="$TMP/backup"
mkdir -p "$ORIG_DIR"
BACKUP="$ORIG_DIR/steam.slsteam-moon.orig"
cat > "$BACKUP" <<'EOF'
#!/bin/sh
echo "ORIGINAL_LAUNCHER"
EOF
chmod 0755 "$BACKUP"

SHIM="$TMP/steam"
ls_shim_content "$BACKUP" > "$SHIM"
chmod 0755 "$SHIM"

# A stubbed `getent` and `id` let the test decide who the effective user is and
# what the password database says, without needing root.
STUB="$TMP/stub"
mkdir -p "$STUB"
REAL_STAT="$(command -v stat)"
# The shim checks every folder up to /. In a test sandbox / and /tmp are not
# root's (user namespace), so this stub reports them as root's, as on a real
# system. T13 extends it.
base_stat_stub() {
	cat > "$STUB/stat" <<EOF
#!/bin/sh
last=""
for a in "\$@"; do last="\$a"; done
case "\$last:\$*" in
	/:*%u*|/tmp:*%u*) echo 0; exit 0 ;;
esac
exec "$REAL_STAT" "\$@"
EOF
	chmod 0755 "$STUB/stat"
}
base_stat_stub
make_stubs() {
	# make_stubs <euid> <username> <passwd-home>
	cat > "$STUB/id" <<EOF
#!/bin/sh
case "\$1" in
  -u) echo "$1" ;;
  -un) echo "$2" ;;
  *) echo "$2" ;;
esac
EOF
	cat > "$STUB/getent" <<EOF
#!/bin/sh
[ "\$1" = passwd ] || exit 2
echo "$2:x:$1:$1::$3:/bin/sh"
EOF
	chmod 0755 "$STUB/id" "$STUB/getent"
}

run_shim() {
	# run_shim <HOME value>
	env PATH="$STUB:$PATH" HOME="$1" sh "$SHIM" 2>&1
}

MY_UID="$(id -u)"

# ---------------------------------------------------------------------------
# T1: the ordinary case still works. The effective user's passwd home IS the
#     home in the environment, and the wrapper there is used.
# ---------------------------------------------------------------------------
make_stubs "$MY_UID" "real" "$REAL_HOME"
OUT="$(run_shim "$REAL_HOME")"
check "T1 the user's own wrapper is executed" '[ "$OUT" = "WRAPPER:real" ]'

# ---------------------------------------------------------------------------
# T2: a wrapper that the EFFECTIVE account does not own must not be executed.
#     This is the property that stops the escalation: the shim may be entered as
#     one identity while $HOME still names another account's directory, and the
#     wrapper found there belongs to that other account.
# ---------------------------------------------------------------------------
make_stubs 4242 "other" "$FAKE_HOME"
OUT="$(run_shim "$REAL_HOME")"
check "T2 a wrapper owned by another account is refused" \
	'[ "$OUT" != "WRAPPER:real" ]'
check "T2b a captured original another account owns is not run either" \
	'[ "$OUT" != "ORIGINAL_LAUNCHER" ] && printf "%s" "$OUT" | grep -q "no usable launcher"'

# ---------------------------------------------------------------------------
# T3: running as root with HOME preserved. root's passwd home has no wrapper, so
#     the user's script must not be executed.
# ---------------------------------------------------------------------------
ROOT_HOME="$TMP/rootdir"
mkdir -p "$ROOT_HOME"
make_stubs 0 "root" "$ROOT_HOME"
OUT="$(run_shim "$REAL_HOME")"
check "T3 elevated invocation does not run the user's wrapper" \
	'[ "$OUT" != "WRAPPER:real" ]'
check "T3b elevated invocation does not run a captured original the user owns" \
	'[ "$OUT" != "ORIGINAL_LAUNCHER" ]'

# ---------------------------------------------------------------------------
# T4: a wrapper that others can write must be refused even inside the right home:
#     otherwise any local process can replace what the shim executes.
# ---------------------------------------------------------------------------
make_stubs "$MY_UID" "real" "$REAL_HOME"
chmod 0777 "$REAL_HOME/.local/share/SLSsteam/path/steam"
OUT="$(run_shim "$REAL_HOME")"
check "T4 a world-writable wrapper is refused" '[ "$OUT" != "WRAPPER:real" ]'
check "T4b a refused wrapper falls back to the packaged launcher" \
	'[ "$OUT" = "ORIGINAL_LAUNCHER" ]'
chmod 0700 "$REAL_HOME/.local/share/SLSsteam/path/steam"

# A group-writable wrapper is equally unacceptable.
chmod 0770 "$REAL_HOME/.local/share/SLSsteam/path/steam"
OUT="$(run_shim "$REAL_HOME")"
check "T5 a group-writable wrapper is refused" '[ "$OUT" != "WRAPPER:real" ]'
chmod 0700 "$REAL_HOME/.local/share/SLSsteam/path/steam"

# ---------------------------------------------------------------------------
# T6: a symlink in the wrapper's place is refused — it can be repointed at any
#     script without changing the wrapper's own permissions.
# ---------------------------------------------------------------------------
PAYLOAD="$TMP/payload.sh"
cat > "$PAYLOAD" <<'EOF'
#!/bin/sh
echo "PAYLOAD"
EOF
chmod 0700 "$PAYLOAD"
mv "$REAL_HOME/.local/share/SLSsteam/path/steam" "$TMP/wrapper.real"
ln -s "$PAYLOAD" "$REAL_HOME/.local/share/SLSsteam/path/steam"
OUT="$(run_shim "$REAL_HOME")"
check "T6 a symlinked wrapper is refused" '[ "$OUT" != "PAYLOAD" ]'
check "T6b it falls back to the packaged launcher" '[ "$OUT" = "ORIGINAL_LAUNCHER" ]'
rm -f "$REAL_HOME/.local/share/SLSsteam/path/steam"
mv "$TMP/wrapper.real" "$REAL_HOME/.local/share/SLSsteam/path/steam"

# ---------------------------------------------------------------------------
# T12: the captured original runs only from a tree nobody else can change. A
#      folder above it that others can write is refused, even for its owner.
# ---------------------------------------------------------------------------
make_stubs "$MY_UID" "real" "$REAL_HOME"
mv "$REAL_HOME/.local/share/SLSsteam/path/steam" "$TMP/wrapper.real"
chmod 0777 "$ORIG_DIR"
OUT="$(run_shim "$REAL_HOME")"
check "T12 a captured original in a folder others can write is refused" \
	'[ "$OUT" != "ORIGINAL_LAUNCHER" ]'
chmod 0755 "$ORIG_DIR"
OUT="$(run_shim "$REAL_HOME")"
check "T12b the same original runs once only its owner can change the folder" \
	'[ "$OUT" = "ORIGINAL_LAUNCHER" ]'
mv "$TMP/wrapper.real" "$REAL_HOME/.local/share/SLSsteam/path/steam"

# ---------------------------------------------------------------------------
# T13: a captured original that root owns, in folders only root can change,
#      runs for any account. A stat stub reports root as the owner of that
#      tree (and of $TMP above it), since the test cannot create root files.
# ---------------------------------------------------------------------------
SYS_TREE="$TMP/sysroot"
mkdir -p "$SYS_TREE/usr/bin"
printf '#!/bin/sh\necho "SYSTEM_ORIGINAL"\n' > "$SYS_TREE/usr/bin/steam.orig"
chmod 0755 "$SYS_TREE/usr/bin/steam.orig"
ln -s steam.orig "$SYS_TREE/usr/bin/steam"
SYS_SHIM="$TMP/steam-system"
ls_shim_content "$SYS_TREE/usr/bin/steam.orig" > "$SYS_SHIM"
chmod 0755 "$SYS_SHIM"
cat > "$STUB/stat" <<EOF
#!/bin/sh
last=""
for a in "\$@"; do last="\$a"; done
case "\$last" in
	/|/tmp|"$TMP"|"$SYS_TREE"|"$SYS_TREE"/*)
		case "\$*" in *%u*) echo 0; exit 0 ;; esac ;;
esac
exec "$REAL_STAT" "\$@"
EOF
chmod 0755 "$STUB/stat"
make_stubs 4242 "other" "$FAKE_HOME"
OUT="$(env PATH="$STUB:$PATH" HOME="$FAKE_HOME" sh "$SYS_SHIM" 2>&1)"
check "T13 a root-owned captured original runs for another account" \
	'[ "$OUT" = "SYSTEM_ORIGINAL" ]'
make_stubs 0 "root" "$ROOT_HOME"
OUT="$(env PATH="$STUB:$PATH" HOME="$REAL_HOME" sh "$SYS_SHIM" 2>&1)"
check "T13b and for an elevated invocation" '[ "$OUT" = "SYSTEM_ORIGINAL" ]'
base_stat_stub

# ---------------------------------------------------------------------------
# T7: an invocation without a usable $HOME (unset, or not absolute) finds the
#     wrapper through the effective account's password-database home.
# ---------------------------------------------------------------------------
make_stubs "$MY_UID" "real" "$REAL_HOME"
OUT="$(env -u HOME PATH="$STUB:$PATH" sh "$SHIM" 2>&1)"
check "T7 without HOME the password-database home's wrapper is executed" \
	'[ "$OUT" = "WRAPPER:real" ]'
OUT="$(run_shim "relative/home")"
check "T7b a relative HOME gives way to the password-database home" \
	'[ "$OUT" = "WRAPPER:real" ]'

# ---------------------------------------------------------------------------
# T8: with nothing usable at all the shim still exits cleanly rather than
#     executing something unexpected.
# ---------------------------------------------------------------------------
rm -f "$REAL_HOME/.local/share/SLSsteam/path/steam" "$BACKUP"
OUT="$(run_shim "$REAL_HOME")"
check "T8 with no wrapper and no original, the bootstrapped steam.sh is used" \
	'[ "$OUT" = "REAL_STEAM_SH" ]'

# ---------------------------------------------------------------------------
# T9: the LAST-RESORT steam.sh fallback lives under $HOME too, so it needs the
#     same ownership check as the wrapper. Refusing the wrapper and then
#     executing a steam.sh out of the same tree would be the identical
#     escalation by another path. Reproduces as euid 0 with no wrapper and no
#     captured original left to fall back to.
# ---------------------------------------------------------------------------
VICTIM_STEAM="$REAL_HOME/.local/share/Steam"
mkdir -p "$VICTIM_STEAM"
cat > "$VICTIM_STEAM/steam.sh" <<'EOF'
#!/bin/sh
echo "PWNED_STEAMSH"
EOF
chmod 0777 "$VICTIM_STEAM/steam.sh"
# Nothing else must be usable, so the fallback loop is the only path left.
rm -f "$REAL_HOME/.local/share/SLSsteam/path/steam"
rm -f "$STEAM_ROOT/steam.sh"
make_stubs 0 "root" "$ROOT_HOME"
OUT="$(run_shim "$REAL_HOME")"
check "T9 an elevated shim does not exec a user-writable steam.sh" \
	'[ "$OUT" != "PWNED_STEAMSH" ]'
check "T9b it reports that nothing usable was found instead" \
	'printf "%s" "$OUT" | grep -q "no usable launcher"'

# Owned by the effective account and not writable by anyone else: allowed.
chmod 0755 "$VICTIM_STEAM/steam.sh"
make_stubs "$MY_UID" "real" "$REAL_HOME"
OUT="$(run_shim "$REAL_HOME")"
check "T10 a properly owned steam.sh is still used as the last resort" \
	'[ "$OUT" = "PWNED_STEAMSH" ]'

# World-writable, right owner: still refused.
chmod 0777 "$VICTIM_STEAM/steam.sh"
OUT="$(run_shim "$REAL_HOME")"
check "T11 a world-writable steam.sh is refused even for its owner" \
	'[ "$OUT" != "PWNED_STEAMSH" ]'

echo
echo "$checks check(s), $fails failure(s)"
[ "$fails" -eq 0 ] || exit 1
