#!/usr/bin/env bash
# Regression: LS-CI must not claim isolation then boot a nonexistent pool.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
fixture="$(mktemp -d)"
trap 'rm -rf "$fixture"' EXIT
source "$root/tools/lib/ci-pool.sh"
POOL="$fixture/base"; POOL_SNAP="$fixture/snapshot"
KEYFILE="$fixture/key"; KEY_SNAP="$fixture/key-snapshot"
dest="$fixture/slot"
printf base > "$POOL"
pool_restore "$dest"
cmp "$POOL" "$dest"
printf changed > "$dest"
pool_restore "$dest"
cmp "$POOL" "$dest"
printf pristine > "$POOL_SNAP"
printf key > "$KEYFILE"; cp "$KEYFILE" "$KEY_SNAP"
pool_restore "$dest"
cmp "$POOL_SNAP" "$dest"
printf other-key > "$KEY_SNAP"
pool_restore "$dest"
cmp "$POOL" "$dest"
rm "$dest"
LS_CI_POOL_RESTORE=0 pool_restore "$dest"
cmp "$POOL" "$dest"
printf changed > "$dest"
LS_CI_POOL_RESTORE=0 pool_restore "$dest"
[[ "$(cat "$dest")" == changed ]]
# A copy which partially writes and fails must exit before any guest can boot.
if ( cp() { printf truncated > "$dest"; return 1; }; pool_restore "$dest" ) 2>"$fixture/error"; then
    echo 'FAIL: copy failure allowed boot' >&2; exit 1
fi
grep -q 'refusing to boot' "$fixture/error"
rm "$POOL" "$POOL_SNAP"
if ( pool_restore "$dest" ) 2>"$fixture/error"; then
    echo 'FAIL: missing source allowed boot' >&2; exit 1
fi
grep -q 'refusing to boot' "$fixture/error"
echo 'PASS: missing/coherent/stale snapshot, retry isolation, opt-out seeding, fail-closed copies'
