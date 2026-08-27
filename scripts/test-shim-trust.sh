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
# The shim must therefore: resolve HOME from the password database for the
# EFFECTIVE user rather than trusting $HOME, and refuse a wrapper that is not a
# plain file owned by that user with no group/other write bit.
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
check "T2b it falls back to the packaged launcher" \
	'[ "$OUT" = "ORIGINAL_LAUNCHER" ]'

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
check "T3b elevated invocation uses the packaged launcher" \
	'[ "$OUT" = "ORIGINAL_LAUNCHER" ]'

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
# T7: the generated shim must not read HOME straight out of the environment for
#     the wrapper path, and must resolve the account from the password database.
# ---------------------------------------------------------------------------
check "T7 the shim resolves the account home from the password database" \
	'grep -q "getent passwd" "$SHIM"'
check "T7b the wrapper path is not taken from \$HOME directly" \
	'! grep -q "SLSM_WRAPPER=\"\${HOME}" "$SHIM"'
check "T7c the shim validates the wrapper before exec" \
	'grep -q "slsm_wrapper_trusted" "$SHIM"'

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
